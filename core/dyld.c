// core/dyld.c - CamelOS Dynamic Linker (dyld-lite) Implementation
// Loads Mach-O dylibs recursively, resolves LC_LOAD_DYLIB dependencies,
// binds undefined symbols across loaded images
// Provides @rpath, @executable_path, @loader_path prefix resolution

#include "dyld.h"
#include "macho_loader.h"
#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

// --- State ---

static dyld_image_t g_dyld_libs[DYLD_MAX_LIBS];
static int g_dyld_count = 0;

static char g_executable_path[256] = "";
static char g_search_paths[DYLD_MAX_SEARCH_PATHS][256];
static int g_search_path_count = 0;

// Global symbol table for fast cross-image resolution
#define DYLD_MAX_GLOBAL_SYMS 2048
static dyld_binding_t g_global_syms[DYLD_MAX_GLOBAL_SYMS];
static int g_global_sym_count = 0;

// --- Initialization ---

void dyld_init(void) {
    memset(g_dyld_libs, 0, sizeof(g_dyld_libs));
    memset(g_global_syms, 0, sizeof(g_global_syms));
    g_dyld_count = 0;
    g_global_sym_count = 0;
    g_search_path_count = 0;

    // Set up default search paths (macOS-compatible layout)
    dyld_add_search_path(DYLD_SYSTEM_LIBS);
    dyld_add_search_path(DYLD_SYSTEM_FRAMWORKS);
    dyld_add_search_path(DYLD_LOCAL_LIBS);
    dyld_add_search_path("/Library/Frameworks");

    s_printf("[dyld] Initialized with ");
    char buf[8];
    int_to_str(g_search_path_count, buf);
    s_printf(buf);
    s_printf(" search paths\n");
}

void dyld_set_executable_path(const char* path) {
    if (!path) return;
    strncpy(g_executable_path, path, 255);
    g_executable_path[255] = 0;
}

void dyld_add_search_path(const char* path) {
    if (!path || g_search_path_count >= DYLD_MAX_SEARCH_PATHS) return;
    strncpy(g_search_paths[g_search_path_count], path, 255);
    g_search_paths[g_search_path_count][255] = 0;
    g_search_path_count++;
}

// --- Path Resolution ---

// Check if a path starts with a given prefix
static int path_has_prefix(const char* path, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (path[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

// Get directory part of a path (returns static buffer)
static const char* path_dirname(const char* path) {
    static char buf[256];
    if (!path) return ".";
    strncpy(buf, path, 255);
    buf[255] = 0;
    char* last_slash = 0;
    char* p = buf;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }
    if (last_slash) {
        *last_slash = 0;
        return buf;
    }
    return ".";
}

char* dyld_resolve_path(const char* path, const char* executable_path) {
    if (!path) return 0;

    char* resolved = (char*)kmalloc(512);
    if (!resolved) return 0;
    memset(resolved, 0, 512);

    // @executable_path - replace with directory containing the main executable
    if (path_has_prefix(path, "@executable_path/")) {
        const char* exec_dir = path_dirname(executable_path ? executable_path : g_executable_path);
        strcpy(resolved, exec_dir);
        strcat(resolved, path + 17);  // Skip "@executable_path"
    }
    // @loader_path - replace with directory containing the loading image
    else if (path_has_prefix(path, "@loader_path/")) {
        const char* loader_dir = path_dirname(g_executable_path);
        strcpy(resolved, loader_dir);
        strcat(resolved, path + 13);  // Skip "@loader_path"
    }
    // @rpath - search each path in the run-path search list
    else if (path_has_prefix(path, "@rpath/")) {
        const char* suffix = path + 7;  // Skip "@rpath"
        int found = 0;
        for (int i = 0; i < g_search_path_count; i++) {
            strcpy(resolved, g_search_paths[i]);
            strcat(resolved, "/");
            strcat(resolved, suffix);
            if (sys_fs_exists(resolved)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            // Fallback: try system lib paths directly
            strcpy(resolved, DYLD_SYSTEM_LIBS);
            strcat(resolved, "/");
            strcat(resolved, suffix);
        }
    }
    // Absolute path - use as-is
    else if (path[0] == '/') {
        strncpy(resolved, path, 511);
        resolved[511] = 0;
    }
    // Relative path - search library paths
    else {
        int found = 0;
        for (int i = 0; i < g_search_path_count; i++) {
            strcpy(resolved, g_search_paths[i]);
            strcat(resolved, "/");
            strcat(resolved, path);
            if (sys_fs_exists(resolved)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            // Last resort: use path as-is
            strncpy(resolved, path, 511);
            resolved[511] = 0;
        }
    }

    return resolved;
}

char* dyld_find_library(const char* name) {
    if (!name) return 0;

    // If it's already an absolute path, resolve prefixes and check
    if (name[0] == '/' || path_has_prefix(name, "@")) {
        char* resolved = dyld_resolve_path(name, g_executable_path);
        if (resolved && sys_fs_exists(resolved)) {
            return resolved;
        }
        if (resolved) kfree(resolved);
        return 0;
    }

    // Search known paths for the library
    // 1. /usr/lib/
    char* candidate = (char*)kmalloc(512);
    if (!candidate) return 0;

    // Try /usr/lib/ first (most common for libSystem, etc.)
    strcpy(candidate, DYLD_SYSTEM_LIBS);
    strcat(candidate, "/");
    strcat(candidate, name);
    if (sys_fs_exists(candidate)) return candidate;

    // Try /System/Library/Frameworks/<name>.framework/<name>
    // For framework-style libraries like Foundation, AppKit, etc.
    strcpy(candidate, DYLD_SYSTEM_FRAMWORKS);
    strcat(candidate, "/");
    strcat(candidate, name);
    // Remove .dylib suffix to get framework name if present
    char fw_name[64];
    strncpy(fw_name, name, 63);
    fw_name[63] = 0;
    char* dot = fw_name;
    while (*dot) {
        if (*dot == '.') { *dot = 0; break; }
        dot++;
    }
    strcat(candidate, ".framework/");
    strcat(candidate, fw_name);
    if (sys_fs_exists(candidate)) return candidate;

    // Try each search path
    for (int i = 0; i < g_search_path_count; i++) {
        strcpy(candidate, g_search_paths[i]);
        strcat(candidate, "/");
        strcat(candidate, name);
        if (sys_fs_exists(candidate)) return candidate;
    }

    kfree(candidate);
    return 0;
}

// --- Global Symbol Table ---

// Register a symbol in the global symbol table
int dyld_register_global_symbol(const char* name, void* address, dyld_image_t* source) {
    if (!name || !address) return -1;

    // Check if already registered (first definition wins, like real dyld)
    for (int i = 0; i < g_global_sym_count; i++) {
        if (strcmp(g_global_syms[i].name, name) == 0) {
            return 0;  // Already bound
        }
    }

    if (g_global_sym_count >= DYLD_MAX_GLOBAL_SYMS) {
        s_printf("[dyld] WARNING: Global symbol table full\n");
        return -1;
    }

    dyld_binding_t* entry = &g_global_syms[g_global_sym_count++];
    strncpy(entry->name, name, 127);
    entry->name[127] = 0;
    entry->address = address;
    entry->source = source;

    return 0;
}

// Register all exported symbols from a loaded image
static void dyld_register_image_exports(dyld_image_t* dylib) {
    if (!dylib || !dylib->image) return;

    loaded_macho_t* img = dylib->image;

    // We need to re-read the Mach-O to get the symbol table
    // This is a simplified approach - in production we'd cache symtab
    char header_buf[4096];
    int hsize = sys_fs_read(dylib->path, header_buf, sizeof(header_buf));
    if (hsize < (int)sizeof(mach_header_t)) return;

    mach_header_t* header = (mach_header_t*)header_buf;
    if (header->magic != MH_MAGIC) return;

    load_command_t* lc = (load_command_t*)(header_buf + sizeof(mach_header_t));
    uint32_t offset = sizeof(mach_header_t);

    for (uint32_t i = 0; i < header->ncmds && offset < (uint32_t)hsize; i++) {
        if (lc->cmd == LC_SYMTAB) {
            symtab_command_t* symtab = (symtab_command_t*)lc;

            // Read the full file for symbol table access
            int fsize = sys_fs_read(dylib->path, header_buf, sizeof(header_buf));
            if (fsize < (int)(symtab->stroff + symtab->strsize)) break;
            if (fsize < (int)(symtab->symoff + symtab->nsyms * sizeof(nlist_t))) break;

            const char* strtab = (const char*)(header_buf + symtab->stroff);
            nlist_t* symbols = (nlist_t*)(header_buf + symtab->symoff);

            for (uint32_t s = 0; s < symtab->nsyms; s++) {
                if (symbols[s].n_type & N_STAB) continue;
                if (!(symbols[s].n_type & N_EXT)) continue;
                if (symbols[s].n_strx >= symtab->strsize) continue;

                const char* sym_name = strtab + symbols[s].n_strx;
                // Skip leading underscore for registration
                const char* reg_name = sym_name;
                if (sym_name[0] == '_') reg_name = sym_name + 1;

                // Calculate resolved address
                void* addr = 0;
                if (img->base_addr && (symbols[s].n_type & N_TYPE) == N_SECT) {
                    addr = (void*)((uint32_t)img->base_addr + symbols[s].n_value);
                }

                if (addr) {
                    dyld_register_global_symbol(reg_name, addr, dylib);
                }
            }
            break;
        }

        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }
}

// --- Library Loading ---

dyld_image_t* dyld_load_library(const char* path) {
    if (!path) return 0;

    // Check if already loaded (by path)
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].state != DYLD_STATE_UNLOADED &&
            strcmp(g_dyld_libs[i].path, path) == 0) {
            g_dyld_libs[i].ref_count++;
            s_printf("[dyld] Already loaded: ");
            s_printf(path);
            s_printf(" (ref_count=");
            char buf[8]; int_to_str(g_dyld_libs[i].ref_count, buf);
            s_printf(buf); s_printf(")\n");
            return &g_dyld_libs[i];
        }
    }

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < DYLD_MAX_LIBS; i++) {
        if (g_dyld_libs[i].state == DYLD_STATE_UNLOADED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        s_printf("[dyld] No free library slots\n");
        return 0;
    }

    // Mark as loading (circular dependency guard)
    dyld_image_t* dylib = &g_dyld_libs[slot];
    strncpy(dylib->path, path, 255);
    dylib->path[255] = 0;
    dylib->state = DYLD_STATE_LOADING;
    dylib->ref_count = 1;

    // Extract name from path
    const char* name_start = path;
    const char* p = path;
    while (*p) { if (*p == '/') name_start = p + 1; p++; }
    strncpy(dylib->name, name_start, 63);
    dylib->name[63] = 0;

    s_printf("[dyld] Loading library: ");
    s_printf(path);
    s_printf("\n");

    // Load the Mach-O image
    loaded_macho_t* img = macho_load(path);
    if (!img) {
        s_printf("[dyld] Failed to load Mach-O: ");
        s_printf(path);
        s_printf("\n");
        dylib->state = DYLD_STATE_FAILED;
        return 0;
    }

    dylib->image = img;
    dylib->state = DYLD_STATE_LOADED;

    // Register exported symbols in the global symbol table
    dyld_register_image_exports(dylib);

    // Recursively load this library's dependencies
    dyld_load_dependencies(img);

    if (slot >= g_dyld_count) g_dyld_count = slot + 1;

    s_printf("[dyld] Successfully loaded: ");
    s_printf(dylib->name);
    s_printf("\n");

    return dylib;
}

int dyld_load_dependencies(loaded_macho_t* image) {
    if (!image) return -1;

    // Re-read the Mach-O header to find LC_LOAD_DYLIB commands
    // We need the file path to re-read; find it from dyld records
    char path[256] = "";
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].image == image) {
            strncpy(path, g_dyld_libs[i].path, 255);
            break;
        }
    }

    // If not in dyld records, use image name as fallback
    if (path[0] == 0) {
        // Try to construct path from image name
        strcpy(path, "/usr/lib/");
        strcat(path, image->name);
    }

    char header_buf[4096];
    int hsize = sys_fs_read(path, header_buf, sizeof(header_buf));
    if (hsize < (int)sizeof(mach_header_t)) return 0;

    mach_header_t* header = (mach_header_t*)header_buf;
    if (header->magic != MH_MAGIC) return 0;

    load_command_t* lc = (load_command_t*)(header_buf + sizeof(mach_header_t));
    uint32_t offset = sizeof(mach_header_t);
    int dep_count = 0;

    for (uint32_t i = 0; i < header->ncmds && offset < (uint32_t)hsize; i++) {
        if (lc->cmd == LC_LOAD_DYLIB) {
            dylib_command_t* dylib_cmd = (dylib_command_t*)lc;
            const char* lib_name = (const char*)lc + dylib_cmd->name_offset;

            s_printf("[dyld] Dependency: ");
            s_printf(lib_name);
            s_printf("\n");

            // Resolve the library path
            char* resolved = dyld_find_library(lib_name);
            if (!resolved) {
                // Try resolving with prefix expansion
                resolved = dyld_resolve_path(lib_name, g_executable_path);
            }

            if (resolved) {
                if (sys_fs_exists(resolved)) {
                    dyld_image_t* dep = dyld_load_library(resolved);
                    if (dep) {
                        dep_count++;
                        s_printf("[dyld] Loaded dependency: ");
                        s_printf(resolved);
                        s_printf("\n");
                    } else {
                        s_printf("[dyld] WARNING: Failed to load dependency: ");
                        s_printf(resolved);
                        s_printf("\n");
                    }
                } else {
                    s_printf("[dyld] WARNING: Dependency not found: ");
                    s_printf(resolved);
                    s_printf("\n");
                }
                kfree(resolved);
            } else {
                s_printf("[dyld] WARNING: Cannot resolve dependency: ");
                s_printf(lib_name);
                s_printf("\n");
            }
        }

        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }

    return dep_count > 0 ? 0 : 0;  // Return 0 even if deps missing (graceful)
}

// --- Symbol Resolution ---

void* dyld_resolve_symbol(const char* name) {
    if (!name) return 0;

    // Search global symbol table
    for (int i = 0; i < g_global_sym_count; i++) {
        if (strcmp(g_global_syms[i].name, name) == 0) {
            return g_global_syms[i].address;
        }
    }

    // Try ObjC runtime for class symbols
    Class cls = objc_getClass(name);
    if (cls) return cls;

    // Try with leading underscore (Mach-O convention)
    char prefixed[128];
    prefixed[0] = '_';
    strncpy(prefixed + 1, name, 126);
    prefixed[127] = 0;

    for (int i = 0; i < g_global_sym_count; i++) {
        if (strcmp(g_global_syms[i].name, prefixed) == 0 ||
            strcmp(g_global_syms[i].name, name) == 0) {
            return g_global_syms[i].address;
        }
    }

    return 0;
}

void* dyld_resolve_symbol_in_image(loaded_macho_t* image, const char* name) {
    if (!image || !name) return 0;

    // First try the image's own symbols via macho_get_symbol
    void* sym = macho_get_symbol(image, name);
    if (sym) return sym;

    // Then try the global symbol table
    return dyld_resolve_symbol(name);
}

int dyld_bind_image(loaded_macho_t* image) {
    if (!image) return -1;

    s_printf("[dyld] Binding image: ");
    s_printf(image->name);
    s_printf("\n");

    // Find the path for this image to re-read the Mach-O
    char path[256] = "";
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].image == image) {
            strncpy(path, g_dyld_libs[i].path, 255);
            break;
        }
    }
    if (path[0] == 0) {
        strcpy(path, "/usr/lib/");
        strcat(path, image->name);
    }

    // Read the Mach-O header to find LC_DYSYMTAB, LC_SYMTAB, and LC_SEGMENT(__DATA)
    char header_buf[8192];
    int hsize = sys_fs_read(path, header_buf, sizeof(header_buf));
    if (hsize < (int)sizeof(mach_header_t)) {
        s_printf("[dyld] WARNING: Cannot read file for binding\n");
        return -1;
    }

    mach_header_t* header = (mach_header_t*)header_buf;
    if (header->magic != MH_MAGIC) {
        s_printf("[dyld] WARNING: Invalid Mach-O for binding\n");
        return -1;
    }

    symtab_command_t* symtab = 0;
    dysymtab_command_t* dysymtab = 0;
    segment_command_t* data_seg = 0;
    uint32_t min_vmaddr = 0xFFFFFFFF;

    // First pass: find commands and min_vmaddr
    load_command_t* lc = (load_command_t*)(header_buf + sizeof(mach_header_t));
    uint32_t offset = sizeof(mach_header_t);

    for (uint32_t i = 0; i < header->ncmds && offset < (uint32_t)hsize; i++) {
        if (lc->cmd == LC_SYMTAB) {
            symtab = (symtab_command_t*)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab = (dysymtab_command_t*)lc;
        } else if (lc->cmd == LC_SEGMENT) {
            segment_command_t* seg = (segment_command_t*)lc;
            if (seg->vmaddr < min_vmaddr) min_vmaddr = seg->vmaddr;
            // Look for __DATA segment (contains GOT sections)
            if (strncmp(seg->segname, "__DATA", 16) == 0) {
                data_seg = seg;
            }
        }
        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }

    if (!symtab || !dysymtab || !data_seg || !image->base_addr) {
        s_printf("[dyld] WARNING: Missing required load commands for binding\n");
        return 0;  // Graceful: ObjC dispatch may still work
    }

    // We need the full file for symbol/string table access
    // Re-read a larger buffer
    uint8_t* full_data = (uint8_t*)kmalloc(65536);
    if (!full_data) return -1;
    int fsize = sys_fs_read(path, (char*)full_data, 65536);
    if (fsize < (int)sizeof(mach_header_t)) {
        kfree(full_data);
        return -1;
    }

    // Re-parse from full data
    header = (mach_header_t*)full_data;
    lc = (load_command_t*)(full_data + sizeof(mach_header_t));
    offset = sizeof(mach_header_t);
    symtab = 0;
    dysymtab = 0;
    data_seg = 0;
    min_vmaddr = 0xFFFFFFFF;

    for (uint32_t i = 0; i < header->ncmds && offset < (uint32_t)fsize; i++) {
        if (lc->cmd == LC_SYMTAB) {
            symtab = (symtab_command_t*)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab = (dysymtab_command_t*)lc;
        } else if (lc->cmd == LC_SEGMENT) {
            segment_command_t* seg = (segment_command_t*)lc;
            if (seg->vmaddr < min_vmaddr) min_vmaddr = seg->vmaddr;
            if (strncmp(seg->segname, "__DATA", 16) == 0) {
                data_seg = seg;
            }
        }
        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }

    if (!symtab || !dysymtab || !data_seg) {
        kfree(full_data);
        return 0;
    }

    // Validate access to symbol tables
    if (symtab->stroff + symtab->strsize > (uint32_t)fsize ||
        symtab->symoff + symtab->nsyms * sizeof(nlist_t) > (uint32_t)fsize ||
        dysymtab->indirectsymoff + dysymtab->nindirectsyms * sizeof(uint32_t) > (uint32_t)fsize) {
        kfree(full_data);
        return 0;
    }

    const char* strtab = (const char*)(full_data + symtab->stroff);
    nlist_t* symbols = (nlist_t*)(full_data + symtab->symoff);
    uint32_t* indirect_syms = (uint32_t*)(full_data + dysymtab->indirectsymoff);

    uint32_t image_base = (uint32_t)image->base_addr;

    int bound_count = 0;
    int unresolved_count = 0;

    // Walk __DATA segment sections looking for symbol pointer sections
    section_t* sect = (section_t*)(data_seg + 1);
    for (uint32_t i = 0; i < data_seg->nsects; i++) {
        uint32_t section_type = sect[i].flags & 0xFF;

        // Process lazy symbol pointer sections and non-lazy symbol pointer sections
        if (section_type == S_LAZY_SYMBOL_POINTERS ||
            section_type == S_NON_LAZY_SYMBOL_POINTERS) {

            // Each entry is a 4-byte pointer
            uint32_t entry_count = sect[i].size / sizeof(uint32_t);
            // sect.reserved1 is the starting index in the indirect symbol table
            uint32_t indirect_offset = sect[i].reserved1;

            // Calculate the address of this section in the loaded image
            uint32_t* got_base = (uint32_t*)(image_base + (sect[i].addr - min_vmaddr));

            const char* section_name = (section_type == S_LAZY_SYMBOL_POINTERS) ?
                "__la_symbol_ptr" : "__nl_symbol_ptr";

            for (uint32_t e = 0; e < entry_count; e++) {
                if (indirect_offset + e >= dysymtab->nindirectsyms) break;

                uint32_t sym_idx = indirect_syms[indirect_offset + e];

                // Special indices
                #define INDIRECT_SYMBOL_ABS   0x80000000
                #define INDIRECT_SYMBOL_LOCAL  0x80000001

                if (sym_idx == INDIRECT_SYMBOL_ABS || sym_idx == INDIRECT_SYMBOL_LOCAL) {
                    // Absolute or local symbol - just apply slide
                    got_base[e] += (image_base - min_vmaddr);
                    bound_count++;
                    continue;
                }

                if (sym_idx >= symtab->nsyms) continue;

                // Look up the symbol name
                nlist_t* sym = &symbols[sym_idx];
                if (sym->n_strx >= symtab->strsize) continue;

                const char* sym_name = strtab + sym->n_strx;
                // Skip leading underscore
                const char* lookup_name = sym_name;
                if (sym_name[0] == '_') lookup_name = sym_name + 1;

                // Resolve the symbol
                void* resolved = dyld_resolve_symbol(lookup_name);
                if (resolved) {
                    got_base[e] = (uint32_t)resolved;
                    bound_count++;
                } else {
                    // Check if it's defined in this image
                    if ((sym->n_type & N_TYPE) == N_SECT) {
                        got_base[e] = image_base + (sym->n_value - min_vmaddr);
                        bound_count++;
                    } else {
                        unresolved_count++;
                        s_printf("[dyld] WARNING: Unresolved symbol: ");
                        s_printf(sym_name);
                        s_printf("\n");
                    }
                }
            }
        }
    }

    kfree(full_data);

    s_printf("[dyld] Binding complete for ");
    s_printf(image->name);
    s_printf(": ");
    char buf[16];
    int_to_str(bound_count, buf);
    s_printf(buf);
    s_printf(" bound, ");
    int_to_str(unresolved_count, buf);
    s_printf(buf);
    s_printf(" unresolved\n");

    return unresolved_count;
}

// --- Image Registration ---

void dyld_register_image(loaded_macho_t* image, const char* path) {
    if (!image) return;

    // Check if already registered
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].image == image) return;
    }

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < DYLD_MAX_LIBS; i++) {
        if (g_dyld_libs[i].state == DYLD_STATE_UNLOADED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return;

    dyld_image_t* rec = &g_dyld_libs[slot];
    if (path) {
        strncpy(rec->path, path, 255);
        rec->path[255] = 0;
    }
    strncpy(rec->name, image->name, 63);
    rec->name[63] = 0;
    rec->image = image;
    rec->state = DYLD_STATE_LOADED;
    rec->ref_count = 1;

    // Register this image's exported symbols
    dyld_register_image_exports(rec);

    if (slot >= g_dyld_count) g_dyld_count = slot + 1;
}

dyld_image_t* dyld_get_image_record(loaded_macho_t* image) {
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].image == image) return &g_dyld_libs[i];
    }
    return 0;
}

void dyld_unload_library(const char* path) {
    if (!path) return;

    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].state == DYLD_STATE_LOADED &&
            strcmp(g_dyld_libs[i].path, path) == 0) {
            g_dyld_libs[i].ref_count--;
            if (g_dyld_libs[i].ref_count == 0) {
                if (g_dyld_libs[i].image) {
                    macho_unload(g_dyld_libs[i].image);
                }
                g_dyld_libs[i].state = DYLD_STATE_UNLOADED;
                g_dyld_libs[i].image = 0;
                s_printf("[dyld] Unloaded: ");
                s_printf(path);
                s_printf("\n");
            }
            return;
        }
    }
}

int dyld_get_loaded_count(void) {
    int count = 0;
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].state == DYLD_STATE_LOADED) count++;
    }
    return count;
}

void dyld_dump_loaded(void) {
    s_printf("[dyld] Loaded libraries:\n");
    for (int i = 0; i < g_dyld_count; i++) {
        if (g_dyld_libs[i].state == DYLD_STATE_LOADED) {
            s_printf("  ");
            s_printf(g_dyld_libs[i].path);
            s_printf(" (refs=");
            char buf[8]; int_to_str(g_dyld_libs[i].ref_count, buf);
            s_printf(buf);
            s_printf(")\n");
        }
    }
}
