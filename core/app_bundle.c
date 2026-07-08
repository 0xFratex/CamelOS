// core/app_bundle.c - CamelOS Application Bundle Loader
// Handles loading of .app bundles (macOS-like) and legacy .cdl files
// Integrates with the existing CDL loader for backward compatibility

#include "app_bundle.h"
#include "sha256.h"
#include "string.h"
#include "memory.h"
#include "macho_loader.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

// Forward declarations from cdl_loader.c
extern int internal_load_library(const char* path);

// Mirror of loaded_cdl_t from cdl_loader.c (needed for accessing loaded library info)
typedef struct { char name[32]; void* base_addr; uint32_t size; void* exports; int active; } loaded_cdl_t_mirror;
extern loaded_cdl_t_mirror loaded_libraries[];

extern void int_to_str(int, char*);

#define MAX_LOADED_BUNDLES 32

static LoadedAppBundle loaded_bundles[MAX_LOADED_BUNDLES];

void app_bundle_init_system(void) {
    memset(loaded_bundles, 0, sizeof(loaded_bundles));
}

// Simple plist parser - reads either XML or key=value format Info.plist.
//
// History: this function originally only parsed plain "key=value" lines.
// But app_bootstrap.c writes XML plists (the macOS standard format),
// so EVERY plist failed to parse — the registry fell back to deriving
// the app name from the directory name. This made CFBundleExecutable,
// CFBundleIconFile, CamelBuiltin, etc. invisible to the launcher.
//
// The new parser handles both formats:
//   * XML:    <key>CFBundleName</key>\n  <string>Files</string>
//             <key>CamelBuiltin</key>\n  <true/>
//   * key=value: CFBundleName=Files
//                CamelBuiltin=true
//
// The XML parser is intentionally minimal — it only understands the
// subset of plist XML that app_bootstrap.c actually emits: <key>,
// <string>, <true/>, <false/>. That's enough to round-trip our own
// plists and to parse real macOS Info.plist files for the common keys.
int app_bundle_parse_plist(const char* bundle_path, AppBundleInfo* info) {
    char plist_path[BUNDLE_PATH_MAX];
    char buffer[BUNDLE_PLIST_MAX];

    // Build path to Info.plist
    strcpy(plist_path, bundle_path);
    strcat(plist_path, "/Info.plist");

    // Try to read plist
    int result = sys_fs_read(plist_path, buffer, sizeof(buffer) - 1);
    if (result <= 0) {
        // No plist found - use defaults
        memset(info, 0, sizeof(AppBundleInfo));
        strcpy(info->type, APP_TYPE_CDL_COMPAT);
        return -1;
    }

    buffer[result] = 0;

    memset(info, 0, sizeof(AppBundleInfo));
    // Default type if not specified in plist
    strcpy(info->type, APP_TYPE_BUILTIN);

    // Detect format: XML starts with '<?xml' or '<plist'.
    // Skip leading whitespace.
    char* p = buffer;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int is_xml = (p[0] == '<');

    if (is_xml) {
        // -------- XML plist parser --------
        // Walk through the buffer finding <key>K</key> followed by
        // <string>V</string> (or <true/> / <false/>). When we find a
        // known key, store the value in info.
        char current_key[64] = {0};

        while (*p) {
            // Find next '<'
            if (*p != '<') { p++; continue; }
            p++;  // skip '<'

            // Skip XML comments, declarations, doctype, <?xml ... ?>
            if (*p == '?' || *p == '!') {
                while (*p && *p != '>') p++;
                if (*p) p++;
                continue;
            }

            // Parse the tag name.
            char tag[32] = {0};
            int ti = 0;
            while (*p && *p != '>' && *p != ' ' && *p != '/' && ti < 31) {
                tag[ti++] = *p++;
            }
            tag[ti] = 0;

            // Skip attributes (we don't care about them).
            while (*p && *p != '>' && *p != '/') p++;

            int self_closing = (*p == '/');
            if (*p && *p != '>') p++;  // skip '/'
            if (*p) p++;  // skip '>'

            if (strcmp(tag, "key") == 0) {
                // Read text until </key>
                char* close = strstr(p, "</key>");
                if (!close) break;
                int klen = close - p;
                if (klen >= (int)sizeof(current_key)) klen = sizeof(current_key) - 1;
                memcpy(current_key, p, klen);
                current_key[klen] = 0;
                p = close + 6;  // skip </key>
            } else if (strcmp(tag, "string") == 0) {
                // Read text until </string>
                char* close = strstr(p, "</string>");
                if (!close) break;
                int vlen = close - p;
                char value[256];
                if (vlen >= (int)sizeof(value)) vlen = sizeof(value) - 1;
                memcpy(value, p, vlen);
                value[vlen] = 0;
                p = close + 9;  // skip </string>

                // Trim leading/trailing whitespace from value.
                char* v = value;
                while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
                int vl = strlen(v);
                while (vl > 0 && (v[vl-1] == ' ' || v[vl-1] == '\t' ||
                                  v[vl-1] == '\n' || v[vl-1] == '\r')) {
                    v[--vl] = 0;
                }

                if (strcmp(current_key, PLIST_KEY_CFNAME) == 0) {
                    strncpy(info->name, v, BUNDLE_NAME_MAX - 1);
                } else if (strcmp(current_key, PLIST_KEY_CFIDENTIFIER) == 0) {
                    strncpy(info->identifier, v, BUNDLE_ID_MAX - 1);
                } else if (strcmp(current_key, PLIST_KEY_CFEXECUTABLE) == 0) {
                    strncpy(info->executable, v, BUNDLE_NAME_MAX - 1);
                } else if (strcmp(current_key, PLIST_KEY_CFVERSION) == 0) {
                    strncpy(info->version, v, 15);
                } else if (strcmp(current_key, PLIST_KEY_CFTYPE) == 0) {
                    strncpy(info->type, v, 15);
                } else if (strcmp(current_key, PLIST_KEY_CFICON) == 0) {
                    strncpy(info->icon_file, v, 63);
                } else if (strcmp(current_key, PLIST_KEY_CFMINOS) == 0) {
                    strncpy(info->min_os_version, v, 15);
                } else if (strcmp(current_key, PLIST_KEY_CFCDLPATH) == 0) {
                    strncpy(info->cdl_path, v, BUNDLE_PATH_MAX - 1);
                }
                current_key[0] = 0;  // consume
            } else if (strcmp(tag, "true") == 0 && self_closing) {
                // Boolean true. Currently we only care about CamelBuiltin,
                // which we encode as type="builtin" if true (and the
                // explicit CFBundleType overrides this if set).
                if (strcmp(current_key, "CamelBuiltin") == 0) {
                    if (info->type[0] == 0) {
                        strcpy(info->type, APP_TYPE_BUILTIN);
                    }
                }
                current_key[0] = 0;
            } else if (strcmp(tag, "false") == 0 && self_closing) {
                current_key[0] = 0;
            } else {
                // Unknown tag — consume, don't crash.
                current_key[0] = 0;
            }
        }
    } else {
        // -------- key=value parser (legacy fallback) --------
        char* line = buffer;
        while (line && *line) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;

            if (line[0] == '#' || line[0] == 0) { line = next; continue; }

            char* eq = strchr(line, '=');
            if (!eq) { line = next; continue; }
            *eq = 0;
            char* key = line;
            char* value = eq + 1;
            while (*key == ' ') key++;
            while (*value == ' ') value++;

            if (strcmp(key, PLIST_KEY_CFNAME) == 0) {
                strncpy(info->name, value, BUNDLE_NAME_MAX - 1);
            } else if (strcmp(key, PLIST_KEY_CFIDENTIFIER) == 0) {
                strncpy(info->identifier, value, BUNDLE_ID_MAX - 1);
            } else if (strcmp(key, PLIST_KEY_CFEXECUTABLE) == 0) {
                strncpy(info->executable, value, BUNDLE_NAME_MAX - 1);
            } else if (strcmp(key, PLIST_KEY_CFVERSION) == 0) {
                strncpy(info->version, value, 15);
            } else if (strcmp(key, PLIST_KEY_CFTYPE) == 0) {
                strncpy(info->type, value, 15);
            } else if (strcmp(key, PLIST_KEY_CFICON) == 0) {
                strncpy(info->icon_file, value, 63);
            } else if (strcmp(key, PLIST_KEY_CFMINOS) == 0) {
                strncpy(info->min_os_version, value, 15);
            } else if (strcmp(key, PLIST_KEY_CFCDLPATH) == 0) {
                strncpy(info->cdl_path, value, BUNDLE_PATH_MAX - 1);
            }
            line = next;
        }
    }

    return 0;
}

// Resolve an app path to its actual executable
const char* app_bundle_resolve_executable(const char* app_path) {
    static char resolved[BUNDLE_PATH_MAX];
    int len = strlen(app_path);
    
    // Check if it's a .app bundle
    if (len > 4 && strcmp(app_path + len - 4, ".app") == 0) {
        // Try macOS-style bundle layout: Contents/MacOS/Executable
        AppBundleInfo info;
        memset(&info, 0, sizeof(info));
        app_bundle_parse_plist(app_path, &info);
        
        // If the plist specifies a direct CDL path, use it first
        if (info.cdl_path[0] && sys_fs_exists(info.cdl_path)) {
            strcpy(resolved, info.cdl_path);
            return resolved;
        }
        
        // If the app type is "builtin", return a special path that the
        // kernel's execute_program can recognize
        if (strcmp(info.type, "builtin") == 0) {
            // Built-in apps are compiled into the kernel
            // Return the app path itself - the launcher will detect the type
            strcpy(resolved, app_path);
            return resolved;
        }
        
        if (info.executable[0]) {
            // Use the executable name from plist
            strcpy(resolved, app_path);
            strcat(resolved, "/Contents/MacOS/");
            strcat(resolved, info.executable);
            if (sys_fs_exists(resolved)) return resolved;
        }
        
        // Try root of bundle directory
        strcpy(resolved, app_path);
        strcat(resolved, "/");
        // Derive executable name from bundle name
        const char* name_start = app_path;
        const char* p = app_path;
        while (*p) { if (*p == '/') name_start = p + 1; p++; }
        int name_len = len - (name_start - app_path) - 4; // Remove .app
        if (name_len > 0 && name_len < BUNDLE_NAME_MAX) {
            strncat(resolved, name_start, name_len);
            if (sys_fs_exists(resolved)) return resolved;
        }
        
        // Fall back to legacy CDL file
        // Convert MyApp.app -> /usr/apps/MyApp.cdl
        strcpy(resolved, "/usr/apps/");
        strncat(resolved, name_start, name_len);
        strcat(resolved, ".cdl");
        if (sys_fs_exists(resolved)) return resolved;
        
        // Also try /Applications path with .cdl extension
        strcpy(resolved, app_path);
        strcpy(resolved + len - 4, ".cdl");
        if (sys_fs_exists(resolved)) return resolved;
        
        return app_path; // Return original as last resort
    }
    
    // Direct path to .cdl file
    return app_path;
}

int app_bundle_load(const char* path) {
    int len = strlen(path);
    char resolved_path[BUNDLE_PATH_MAX];
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_LOADED_BUNDLES; i++) {
        if (!loaded_bundles[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        s_printf("[BUNDLE] No free slot for: ");
        s_printf(path);
        s_printf("\n");
        return -1;
    }
    
    LoadedAppBundle* bundle = &loaded_bundles[slot];
    memset(bundle, 0, sizeof(LoadedAppBundle));
    strncpy(bundle->bundle_path, path, BUNDLE_PATH_MAX - 1);
    
    // Check if it's a .app bundle directory
    if (len > 4 && strcmp(path + len - 4, ".app") == 0) {
        // Parse the bundle's Info.plist
        app_bundle_parse_plist(path, &bundle->info);
        
        // Handle built-in apps - these are compiled into the kernel
        // and launched via the kernel's built-in app dispatch mechanism
        if (strcmp(bundle->info.type, APP_TYPE_BUILTIN) == 0) {
            bundle->active = 1;
            s_printf("[BUNDLE] Built-in app: ");
            s_printf(bundle->info.name[0] ? bundle->info.name : path);
            s_printf(" (launched via kernel dispatch)\n");
            return slot;
        }
        
        // Resolve the executable
        const char* exec_path = app_bundle_resolve_executable(path);
        strncpy(resolved_path, exec_path, BUNDLE_PATH_MAX - 1);
        
        // Check the app type for Mach-O detection
        if (strcmp(bundle->info.type, APP_TYPE_MACHO) == 0 ||
            strcmp(bundle->info.type, APP_TYPE_OBJC) == 0) {
            bundle->is_macho = 1;
        }
    } else {
        // Legacy .cdl file
        strncpy(resolved_path, path, BUNDLE_PATH_MAX - 1);
        strcpy(bundle->info.type, APP_TYPE_CDL_COMPAT);
        
        // Extract name from filename
        const char* name_start = path;
        const char* p = path;
        while (*p) { if (*p == '/') name_start = p + 1; p++; }
        int name_len = strlen(name_start);
        if (name_len > 4) {
            strncpy(bundle->info.name, name_start, name_len - 4);
        }
    }
    
    // Load the executable using the appropriate loader
    if (bundle->is_macho) {
        // Mach-O binary - use the Mach-O loader
        loaded_macho_t* macho_img = macho_load(resolved_path);
        if (!macho_img) {
            s_printf("[BUNDLE] Mach-O load failed: ");
            s_printf(resolved_path);
            s_printf("\n");
            return -1;
        }
        bundle->loaded_image = macho_img->base_addr;
        bundle->image_size = macho_img->image_size;
        bundle->active = 1;
        
        s_printf("[BUNDLE] Loaded Mach-O: ");
        s_printf(bundle->info.name[0] ? bundle->info.name : path);
        s_printf("\n");
        
        return slot;
    }
    
    // CDL/ELF binary - use the CDL loader
    int cdl_slot = internal_load_library(resolved_path);
    if (cdl_slot < 0) {
        s_printf("[BUNDLE] Failed to load executable: ");
        s_printf(resolved_path);
        s_printf("\n");
        return -1;
    }
    
    // Store the CDL slot reference
    bundle->loaded_image = loaded_libraries[cdl_slot].base_addr;
    bundle->image_size = loaded_libraries[cdl_slot].size;
    bundle->exports = loaded_libraries[cdl_slot].exports;
    bundle->active = 1;
    
    s_printf("[BUNDLE] Loaded: ");
    s_printf(bundle->info.name[0] ? bundle->info.name : path);
    s_printf(" (type=");
    s_printf(bundle->info.type);
    s_printf(")\n");
    
    return slot;
}

const AppBundleInfo* app_bundle_get_info(int slot) {
    if (slot < 0 || slot >= MAX_LOADED_BUNDLES || !loaded_bundles[slot].active)
        return 0;
    return &loaded_bundles[slot].info;
}

int app_bundle_find(const char* name) {
    for (int i = 0; i < MAX_LOADED_BUNDLES; i++) {
        if (loaded_bundles[i].active) {
            if (strcmp(loaded_bundles[i].info.name, name) == 0 ||
                strcmp(loaded_bundles[i].bundle_path, name) == 0) {
                return i;
            }
        }
    }
    return -1;
}

void app_bundle_unload(int slot) {
    if (slot >= 0 && slot < MAX_LOADED_BUNDLES) {
        loaded_bundles[slot].active = 0;
    }
}

int app_bundle_list_installed(AppBundleInfo* out_list, int max_count) {
    int count = 0;
    // List files in /Applications
    char dir_buf[4096];
    int result = sys_fs_list_dir("/Applications", dir_buf, sizeof(dir_buf));
    
    if (result > 0) {
        // Parse directory entries
        char* entry = dir_buf;
        while (entry && *entry && count < max_count) {
            char* next = strchr(entry, '\n');
            if (next) *next++ = 0;
            
            int elen = strlen(entry);
            if (elen > 4 && strcmp(entry + elen - 4, ".app") == 0) {
                char full_path[BUNDLE_PATH_MAX];
                strcpy(full_path, "/Applications/");
                strcat(full_path, entry);
                app_bundle_parse_plist(full_path, &out_list[count]);
                if (out_list[count].name[0] == 0) {
                    strncpy(out_list[count].name, entry, elen - 4);
                }
                count++;
            }
            
            entry = next;
        }
    }
    
    return count;
}
