// core/macho_loader.c - Mach-O Binary Loader Implementation for CamelOS
// Loads 32-bit Mach-O executables and dynamic libraries
// Uses the ravynos SDK/KDK approach for macOS app compatibility

#include "macho_loader.h"
#include "objc_runtime.h"
#include "dyld.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

#define MAX_LOADED_MACHO 8
static loaded_macho_t g_macho_images[MAX_LOADED_MACHO];

// --- Magic Detection ---

int macho_check_magic(const uint8_t* data, uint32_t size) {
    if (!data || size < sizeof(mach_header_t)) return 0;
    
    uint32_t magic = *(uint32_t*)data;
    
    if (magic == MH_MAGIC) {
        return 1;  // 32-bit Mach-O
    } else if (magic == MH_MAGIC_64) {
        s_printf("[MachO] 64-bit Mach-O not supported on CamelOS (32-bit OS)\n");
        return 0;
    } else if (magic == FAT_MAGIC) {
        s_printf("[MachO] Fat/universal binary detected - extracting 32-bit slice...\n");
        // TODO: Parse fat binary to find i386 slice
        return 0;
    }
    
    return 0;  // Not a Mach-O
}

// --- Section/Segment Helpers ---

static void* macho_find_section(segment_command_t* seg, const char* sectname,
                                 const uint8_t* raw, uint32_t raw_size) {
    section_t* sect = (section_t*)(seg + 1);
    for (uint32_t i = 0; i < seg->nsects; i++) {
        if (strncmp(sect[i].sectname, sectname, 16) == 0) {
            if (sect[i].offset + sect[i].size > raw_size) {
                s_printf("[MachO] Section out of bounds: ");
                s_printf(sectname);
                s_printf("\n");
                return 0;
            }
            return (void*)(raw + sect[i].offset);
        }
    }
    return 0;
}

// --- Symbol Resolution ---

static void* macho_resolve_symbol(loaded_macho_t* image, const char* name,
                                   symtab_command_t* symtab,
                                   const uint8_t* raw, uint32_t raw_size) {
    if (!symtab || !name) return 0;
    
    // Read string table
    if (symtab->stroff + symtab->strsize > raw_size) return 0;
    const char* strtab = (const char*)(raw + symtab->stroff);
    
    // Read symbol table
    if (symtab->symoff + symtab->nsyms * sizeof(nlist_t) > raw_size) return 0;
    nlist_t* symbols = (nlist_t*)(raw + symtab->symoff);
    
    for (uint32_t i = 0; i < symtab->nsyms; i++) {
        if (symbols[i].n_type & N_STAB) continue;  // Skip debug symbols
        
        if (symbols[i].n_strx < symtab->strsize) {
            const char* sym_name = strtab + symbols[i].n_strx;
            // Mach-O symbols often have a leading underscore
            const char* compare_name = sym_name;
            if (sym_name[0] == '_') compare_name = sym_name + 1;
            
            if (strcmp(compare_name, name) == 0) {
                // Found the symbol
                if (symbols[i].n_type & N_EXT) {
                    // External symbol - resolve address
                    if (image->base_addr) {
                        return (void*)((uint32_t)image->base_addr + symbols[i].n_value);
                    }
                    return (void*)symbols[i].n_value;
                }
            }
        }
    }
    
    return 0;
}

// --- ObjC Section Processing ---

static void macho_process_objc_sections(loaded_macho_t* image,
                                         segment_command_t* seg,
                                         const uint8_t* raw, uint32_t raw_size) {
    // Look for __objc_classlist section
    section_t* sect = (section_t*)(seg + 1);
    for (uint32_t i = 0; i < seg->nsects; i++) {
        // __objc_classlist - contains pointers to Class structures
        if (strncmp(sect[i].sectname, "__objc_classlist", 16) == 0) {
            s_printf("[MachO] Found __objc_classlist section (");
            char buf[16];
            int_to_str(sect[i].size / 4, buf);
            s_printf(buf);
            s_printf(" classes)\n");
            
            // Register each class found in the list
            if (sect[i].offset + sect[i].size <= raw_size) {
                uint32_t* class_ptrs = (uint32_t*)(raw + sect[i].offset);
                uint32_t class_count = sect[i].size / sizeof(uint32_t);
                
                for (uint32_t c = 0; c < class_count; c++) {
                    if (image->base_addr) {
                        Class cls = (Class)((uint32_t)image->base_addr + class_ptrs[c]);
                        if (cls && cls->name[0]) {
                            // Register the class with the ObjC runtime
                            s_printf("[MachO] Registering ObjC class: ");
                            s_printf(cls->name);
                            s_printf("\n");
                        }
                    }
                }
            }
        }
        
        // __objc_protolist - protocol references
        if (strncmp(sect[i].sectname, "__objc_protolist", 16) == 0) {
            s_printf("[MachO] Found __objc_protolist section\n");
        }
        
        // __objc_methname - method names for dynamic dispatch
        if (strncmp(sect[i].sectname, "__objc_methname", 16) == 0) {
            s_printf("[MachO] Found __objc_methname section (");
            char buf[16];
            int_to_str(sect[i].size, buf);
            s_printf(buf);
            s_printf(" bytes)\n");
        }
        
        // __objc_selrefs - selector references (for messaging)
        if (strncmp(sect[i].sectname, "__objc_selrefs", 16) == 0) {
            s_printf("[MachO] Found __objc_selrefs section\n");
            // Register all selectors found
            if (sect[i].offset + sect[i].size <= raw_size && image->base_addr) {
                uint32_t* sel_refs = (uint32_t*)((uint32_t)image->base_addr + 
                    ((uint32_t*)(raw + sect[i].offset) - (uint32_t*)raw));
                // Note: selrefs need relocation first - simplified here
            }
        }
    }
}

// --- Main Loader ---

loaded_macho_t* macho_load(const char* path) {
    if (!path) return 0;
    
    s_printf("[MachO] Loading: ");
    s_printf(path);
    s_printf("\n");
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_LOADED_MACHO; i++) {
        if (!g_macho_images[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        s_printf("[MachO] No free image slots\n");
        return 0;
    }
    
    // Read the file
    char header_buf[4096];
    int hsize = sys_fs_read(path, header_buf, sizeof(header_buf));
    if (hsize < (int)sizeof(mach_header_t)) {
        s_printf("[MachO] File too small or not found\n");
        return 0;
    }
    
    // Verify magic
    if (!macho_check_magic((uint8_t*)header_buf, hsize)) {
        s_printf("[MachO] Not a valid 32-bit Mach-O\n");
        return 0;
    }
    
    mach_header_t* header = (mach_header_t*)header_buf;
    
    // Check CPU type
    if (header->cputype != CPU_TYPE_I386) {
        s_printf("[MachO] Wrong CPU type (expected i386)\n");
        return 0;
    }
    
    s_printf("[MachO] Valid Mach-O: type=");
    char buf[16];
    int_to_str(header->filetype, buf);
    s_printf(buf);
    s_printf(" cmds=");
    int_to_str(header->ncmds, buf);
    s_printf(buf);
    s_printf("\n");
    
    // Parse load commands to determine image size
    uint32_t total_vm_size = 0;
    uint32_t min_vmaddr = 0xFFFFFFFF;
    uint32_t max_vmaddr = 0;
    uint32_t entry_point = 0;
    symtab_command_t* symtab = 0;
    dysymtab_command_t* dysymtab = 0;
    
    load_command_t* lc = (load_command_t*)(header_buf + sizeof(mach_header_t));
    uint32_t offset = sizeof(mach_header_t);
    
    for (uint32_t i = 0; i < header->ncmds && offset < hsize; i++) {
        if (lc->cmd == LC_SEGMENT) {
            segment_command_t* seg = (segment_command_t*)lc;
            
            if (seg->vmaddr < min_vmaddr) min_vmaddr = seg->vmaddr;
            if (seg->vmaddr + seg->vmsize > max_vmaddr) 
                max_vmaddr = seg->vmaddr + seg->vmsize;
            
            s_printf("[MachO] Segment: ");
            s_printf(seg->segname);
            s_printf(" vmaddr=");
            int_to_str(seg->vmaddr, buf);
            s_printf(buf);
            s_printf(" vmsize=");
            int_to_str(seg->vmsize, buf);
            s_printf(buf);
            s_printf("\n");
            
        } else if (lc->cmd == LC_UNIXTHREAD || lc->cmd == LC_THREAD) {
            // Extract entry point from thread state
            // For LC_UNIXTHREAD on i386, the register state starts after cmd+cmdsize
            // EIP is at offset 40 in i386_thread_state
            uint32_t* thread_state = (uint32_t*)lc + 2;  // Skip cmd and cmdsize
            // i386 thread state: EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP, SS, EFLAGS, EIP, CS, DS, ES, FS, GS
            // The exact offset depends on flavor, but typically EIP is at index 10
            // Simplified: try to find it
            for (int j = 0; j < 20 && (offset + j * 4 + 8) < (uint32_t)hsize; j++) {
                uint32_t val = thread_state[j];
                // Heuristic: entry point is typically in the 0x00001000-0x10000000 range
                if (val >= min_vmaddr && val < max_vmaddr && val != 0) {
                    entry_point = val;
                    break;
                }
            }
            
        } else if (lc->cmd == LC_MAIN) {
            // New-style entry point
            uint64_t* main_info = (uint64_t*)lc + 1;
            entry_point = (uint32_t)main_info[0];  // entryoff
            entry_point += min_vmaddr;  // Add base
            
        } else if (lc->cmd == LC_SYMTAB) {
            symtab = (symtab_command_t*)lc;
            
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab = (dysymtab_command_t*)lc;
            
        } else if (lc->cmd == LC_LOAD_DYLIB) {
            dylib_command_t* dylib = (dylib_command_t*)lc;
            const char* lib_name = (const char*)lc + dylib->name_offset;
            s_printf("[MachO] Depends on: ");
            s_printf(lib_name);
            s_printf("\n");
        }
        
        offset += lc->cmdsize;
        if (offset >= hsize) break;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }
    
    // Guard against malformed Mach-O with no segments
    if (max_vmaddr == 0 || min_vmaddr == 0xFFFFFFFF) {
        s_printf("[MachO] No valid segments found in binary\n");
        return 0;
    }
    
    total_vm_size = max_vmaddr - min_vmaddr;
    total_vm_size = (total_vm_size + 4095) & ~4095;  // Page align
    
    s_printf("[MachO] Total VM size: ");
    int_to_str(total_vm_size, buf);
    s_printf(buf);
    s_printf(" entry: ");
    int_to_str(entry_point, buf);
    s_printf(buf);
    s_printf("\n");
    
    // Now read the entire file and load segments
    // First, read the full file
    uint8_t* raw = (uint8_t*)kmalloc(total_vm_size + 65536);  // Extra for raw data
    if (!raw) {
        s_printf("[MachO] Failed to allocate raw buffer\n");
        return 0;
    }
    
    int fsize = sys_fs_read(path, (char*)raw, total_vm_size + 65536);
    if (fsize < (int)sizeof(mach_header_t)) {
        s_printf("[MachO] Failed to read full file\n");
        kfree(raw);
        return 0;
    }
    
    // Allocate memory for the loaded image
    void* image_base = kmalloc(total_vm_size);
    if (!image_base) {
        s_printf("[MachO] Failed to allocate image memory\n");
        kfree(raw);
        return 0;
    }
    memset(image_base, 0, total_vm_size);
    
    // Load segments into allocated memory
    header = (mach_header_t*)raw;
    lc = (load_command_t*)(raw + sizeof(mach_header_t));
    offset = sizeof(mach_header_t);
    
    segment_command_t* objc_seg = 0;  // Track __OBJC segment
    
    for (uint32_t i = 0; i < header->ncmds && offset < (uint32_t)fsize; i++) {
        if (lc->cmd == LC_SEGMENT) {
            segment_command_t* seg = (segment_command_t*)lc;
            
            // Check for __OBJC segment
            if (strncmp(seg->segname, "__OBJC", 16) == 0) {
                objc_seg = seg;
            }
            
            if (seg->filesize > 0) {
                uint32_t dest_offset = seg->vmaddr - min_vmaddr;
                if (dest_offset + seg->filesize <= total_vm_size &&
                    seg->fileoff + seg->filesize <= (uint32_t)fsize) {
                    memcpy((uint8_t*)image_base + dest_offset,
                           raw + seg->fileoff,
                           seg->filesize);
                }
            }
        }
        
        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }
    
    // Process ObjC sections if found
    // NOTE: Must set up image struct BEFORE processing ObjC sections,
    // because macho_process_objc_sections() dereferences image->base_addr
    loaded_macho_t* image = &g_macho_images[slot];
    memcpy(&image->header, header, sizeof(mach_header_t));
    image->base_addr = image_base;
    image->image_size = total_vm_size;
    image->entry_point = entry_point ? (void*)((uint32_t)image_base + (entry_point - min_vmaddr)) : 0;
    image->active = 1;
    image->is_64bit = 0;
    
    // Extract name from path
    const char* name_start = path;
    const char* p = path;
    while (*p) { if (*p == '/') name_start = p + 1; p++; }
    strncpy(image->name, name_start, 63);
    image->name[63] = 0;
    
    // Process ObjC sections now that image struct is properly set up
    if (objc_seg) {
        macho_process_objc_sections(image, objc_seg, raw, fsize);
    }
    
    kfree(raw);
    
    // Register with dyld and load dependencies
    dyld_register_image(image, path);
    dyld_load_dependencies(image);
    dyld_bind_image(image);
    
    s_printf("[MachO] Successfully loaded: ");
    s_printf(image->name);
    s_printf("\n");
    
    return image;
}

// --- Symbol Lookup ---

void* macho_get_symbol(loaded_macho_t* image, const char* symbol_name) {
    if (!image || !symbol_name || !image->active) return 0;
    
    // We need to re-read the file to get the symbol table
    // In a real implementation, we'd cache this
    // For now, use the ObjC runtime for class lookups
    
    // Try ObjC runtime first
    Class cls = objc_getClass(symbol_name);
    if (cls) return cls;
    
    return 0;
}

Class macho_get_objc_class(loaded_macho_t* image, const char* class_name) {
    if (!image || !class_name) return 0;
    return objc_getClass(class_name);
}

// --- Execution ---

int macho_execute(loaded_macho_t* image, int argc, char** argv) {
    if (!image || !image->entry_point) return -1;
    
    s_printf("[MachO] Executing: ");
    s_printf(image->name);
    s_printf("\n");
    
    // For MH_EXECUTE, the entry point is typically main()
    // For MH_BUNDLE, we need to find the principal class
    
    if (image->header.filetype == MH_BUNDLE) {
        // Loadable bundle - find principal class and initialize
        // Convention: look for a class matching the bundle name
        char class_name[64];
        strncpy(class_name, image->name, 63);
        // Remove .app/.bundle extension
        char* dot = class_name;
        while (*dot) {
            if (*dot == '.') { *dot = 0; break; }
            dot++;
        }
        
        Class principal = objc_getClass(class_name);
        if (principal) {
            s_printf("[MachO] Found principal class: ");
            s_printf(class_name);
            s_printf("\n");
            // Send +load and +initialize messages
            SEL load_sel = sel_registerName("load");
            Method load_method = class_getClassMethod(principal, load_sel);
            if (load_method && load_method->imp) {
                typedef void (*load_fn)(Class, SEL);
                ((load_fn)load_method->imp)(principal, load_sel);
            }
        }
    } else {
        // MH_EXECUTE - call main()
        typedef int (*main_fn)(int, char**);
        main_fn main_ptr = (main_fn)image->entry_point;
        if (main_ptr) {
            return main_ptr(argc, argv);
        }
    }
    
    return 0;
}

void macho_unload(loaded_macho_t* image) {
    if (!image || !image->active) return;
    
    if (image->base_addr) {
        kfree(image->base_addr);
    }
    image->active = 0;
    image->base_addr = 0;
}
