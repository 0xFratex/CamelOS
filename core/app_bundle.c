// core/app_bundle.c - CamelOS Application Bundle Loader
// Handles loading of .app bundles (macOS-like) and legacy .cdl files
// Integrates with the existing CDL loader for backward compatibility

#include "app_bundle.h"
#include "sha256.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

// Forward declarations from cdl_loader.c
extern int internal_load_library(const char* path);

// Mirror of loaded_cdl_t from cdl_loader.c (needed for accessing loaded library info)
typedef struct { char name[32]; void* base_addr; uint32_t size; void* exports; int active; } loaded_cdl_t_mirror;
extern loaded_cdl_t_mirror loaded_libraries[];

extern void s_printf(const char* fmt, ...);
extern void int_to_str(int, char*);

#define MAX_LOADED_BUNDLES 32

static LoadedAppBundle loaded_bundles[MAX_LOADED_BUNDLES];

void app_bundle_init_system(void) {
    memset(loaded_bundles, 0, sizeof(loaded_bundles));
}

// Simple plist parser - reads key=value lines from Info.plist
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
    
    // Parse key=value lines
    char* line = buffer;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == 0) { line = next; continue; }
        
        char* eq = strchr(line, '=');
        if (!eq) { line = next; continue; }
        
        *eq = 0;
        char* key = line;
        char* value = eq + 1;
        
        // Trim whitespace
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
        }
        
        line = next;
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
    
    // Load the executable using the existing ELF loader (or Mach-O loader)
    // For now, delegate to internal_load_library which handles ELF
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
