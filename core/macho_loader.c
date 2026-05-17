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

// --- Relocation Processing ---

// Relocation info entry (32-bit Mach-O)
typedef struct {
    int32_t  r_address;     // Offset in the section to what is being relocated
    uint32_t r_symbolnum:24;// Symbol index if r_extern, else section ordinal
    uint32_t r_pcrel:1;     // Was relocated pc-relative already
    uint32_t r_length:2;    // 0=byte, 1=word, 2=long, 3=quad
    uint32_t r_extern:1;    // Does not include value of symbol referenced
    uint32_t r_type:4;      // Machine-specific relocation type
} relocation_info_t;

// i386 relocation types
#define GENERIC_RELOC_VANILLA   0   // Generic relocation

static void macho_apply_relocations(loaded_macho_t* image,
                                     const uint8_t* raw, uint32_t raw_size,
                                     uint32_t min_vmaddr) {
    if (!image || !raw || !image->base_addr) return;

    mach_header_t* header = (mach_header_t*)raw;
    load_command_t* lc = (load_command_t*)(raw + sizeof(mach_header_t));
    uint32_t offset = sizeof(mach_header_t);

    symtab_command_t* symtab = 0;
    dysymtab_command_t* dysymtab = 0;

    // Find LC_SYMTAB and LC_DYSYMTAB from the full file data
    for (uint32_t i = 0; i < header->ncmds && offset < raw_size; i++) {
        if (lc->cmd == LC_SYMTAB) {
            symtab = (symtab_command_t*)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab = (dysymtab_command_t*)lc;
        }
        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }

    if (!dysymtab || !symtab) {
        s_printf("[MachO] No dynamic symbol info - skipping relocations\n");
        return;
    }

    // Validate symbol table access
    if (symtab->stroff + symtab->strsize > raw_size) return;
    if (symtab->symoff + symtab->nsyms * sizeof(nlist_t) > raw_size) return;

    const char* strtab = (const char*)(raw + symtab->stroff);
    nlist_t* symbols = (nlist_t*)(raw + symtab->symoff);

    // Compute the slide (difference between actual load address and preferred VM address)
    uint32_t image_base = (uint32_t)image->base_addr;
    uint32_t slide = image_base - min_vmaddr;

    int reloc_count = 0;
    int reloc_resolved = 0;

    // Walk all segments and sections looking for relocation entries
    lc = (load_command_t*)(raw + sizeof(mach_header_t));
    offset = sizeof(mach_header_t);

    for (uint32_t i = 0; i < header->ncmds && offset < raw_size; i++) {
        if (lc->cmd == LC_SEGMENT) {
            segment_command_t* seg = (segment_command_t*)lc;
            section_t* sect = (section_t*)(seg + 1);

            for (uint32_t j = 0; j < seg->nsects; j++) {
                if (sect[j].nreloc == 0 || sect[j].reloff == 0) continue;

                // Validate relocation table access
                if (sect[j].reloff + sect[j].nreloc * sizeof(relocation_info_t) > raw_size)
                    continue;

                relocation_info_t* relocs = (relocation_info_t*)(raw + sect[j].reloff);

                // Calculate the section's base address in the loaded image
                uint8_t* section_base = (uint8_t*)(image_base + (sect[j].addr - min_vmaddr));

                // Section type from flags (lower 8 bits)
                uint32_t section_type = sect[j].flags & 0xFF;

                for (uint32_t r = 0; r < sect[j].nreloc; r++) {
                    int32_t r_address = relocs[r].r_address;

                    // Negative r_address means scattered relocation - skip
                    if (r_address < 0) continue;

                    // Bounds check: make sure target is within the loaded image
                    if ((uint32_t)r_address >= sect[j].size) continue;

                    uint32_t* target = (uint32_t*)(section_base + r_address);
                    uint32_t r_type = relocs[r].r_type;
                    uint32_t r_extern = relocs[r].r_extern;
                    uint32_t r_length = relocs[r].r_length;
                    uint32_t r_symbolnum = relocs[r].r_symbolnum;

                    reloc_count++;

                    // Process RELOC_VANILLA (generic absolute relocation for i386)
                    if (r_type == GENERIC_RELOC_VANILLA) {
                        if (r_extern) {
                            // External symbol reference - look up in symbol table
                            if (r_symbolnum < symtab->nsyms) {
                                nlist_t* sym = &symbols[r_symbolnum];
                                if (sym->n_strx < symtab->strsize) {
                                    const char* sym_name = strtab + sym->n_strx;
                                    // Skip leading underscore for lookup
                                    const char* lookup_name = sym_name;
                                    if (sym_name[0] == '_') lookup_name = sym_name + 1;

                                    // Try to resolve via dyld
                                    void* resolved = dyld_resolve_symbol(lookup_name);
                                    if (resolved) {
                                        // For pointer-sized (long) relocations, add base
                                        if (r_length == 2) {
                                            *target = (uint32_t)resolved + *target;
                                        } else {
                                            *target = (uint32_t)resolved;
                                        }
                                        reloc_resolved++;
                                    } else if ((sym->n_type & N_TYPE) == N_SECT) {
                                        // Defined in this image at a given section
                                        *target = image_base + (sym->n_value - min_vmaddr) + *target;
                                        reloc_resolved++;
                                    } else if ((sym->n_type & N_TYPE) == N_ABS) {
                                        // Absolute symbol - no relocation needed
                                        reloc_resolved++;
                                    }
                                }
                            }
                        } else {
                            // Internal relocation - apply slide for pointer-type sections
                            // S_REGULAR and S_ABSOLUTE sections with internal relocs
                            if (section_type == S_REGULAR || section_type == 0) {
                                if (r_length == 2) {  // Long (pointer-sized)
                                    *target += slide;
                                    reloc_resolved++;
                                }
                            }
                        }
                    }
                    // Other relocation types (RELOC_PAIR, RELOC_SECTDIFF, etc.)
                    // are handled implicitly by skipping - they come in pairs
                    // and the primary entry is processed above
                }
            }
        }
        offset += lc->cmdsize;
        lc = (load_command_t*)((uint8_t*)lc + lc->cmdsize);
    }

    s_printf("[MachO] Relocations: ");
    char buf[16];
    int_to_str(reloc_resolved, buf);
    s_printf(buf);
    s_printf("/");
    int_to_str(reloc_count, buf);
    s_printf(buf);
    s_printf(" resolved (slide=0x");
    int_to_str(slide, buf);
    s_printf(buf);
    s_printf(")\n");
}

// --- ObjC Section Processing ---

static void macho_process_objc_sections(loaded_macho_t* image,
                                         segment_command_t* seg,
                                         const uint8_t* raw, uint32_t raw_size) {
    uint32_t image_base = (uint32_t)image->base_addr;

    // First pass: find all relevant sections and the __objc_methname data
    section_t* classlist_sect = 0;
    section_t* protolist_sect = 0;
    section_t* methname_sect = 0;
    section_t* methlist_sect = 0;
    section_t* selrefs_sect = 0;
    section_t* classrefs_sect = 0;
    section_t* superrefs_sect = 0;
    section_t* mod_init_sect = 0;

    section_t* sect = (section_t*)(seg + 1);
    for (uint32_t i = 0; i < seg->nsects; i++) {
        if (strncmp(sect[i].sectname, "__objc_classlist", 16) == 0) {
            classlist_sect = &sect[i];
        } else if (strncmp(sect[i].sectname, "__objc_protolist", 16) == 0) {
            protolist_sect = &sect[i];
        } else if (strncmp(sect[i].sectname, "__objc_methname", 16) == 0) {
            methname_sect = &sect[i];
        } else if (strncmp(sect[i].sectname, "__objc_methlist", 16) == 0) {
            methlist_sect = &sect[i];
        } else if (strncmp(sect[i].sectname, "__objc_selrefs", 16) == 0) {
            selrefs_sect = &sect[i];
        } else if (strncmp(sect[i].sectname, "__objc_classrefs", 16) == 0) {
            classrefs_sect = &sect[i];
        } else if (strncmp(sect[i].sectname, "__objc_superrefs", 16) == 0) {
            superrefs_sect = &sect[i];
        } else if (sect[i].flags == 0x9 || strncmp(sect[i].sectname, "__mod_init_func", 16) == 0) {
            // S_MOD_INIT_FUNC_POINTERS = section type 9
            mod_init_sect = &sect[i];
        }
    }

    // Register selectors from __objc_selrefs first (needed before class registration)
    if (selrefs_sect && selrefs_sect->offset + selrefs_sect->size <= raw_size && image_base) {
        s_printf("[MachO] Found __objc_selrefs section\n");
        // Each selref is a pointer (4 bytes on 32-bit) to a selector name in __objc_methname
        uint32_t* sel_ptrs = (uint32_t*)(raw + selrefs_sect->offset);
        uint32_t sel_count = selrefs_sect->size / sizeof(uint32_t);

        for (uint32_t s = 0; s < sel_count; s++) {
            // The pointer value in the raw data is the original VM address
            // After relocation it should point to the methname string
            // Try to read the selector name from the relocated pointer
            uint32_t sel_ptr_val = sel_ptrs[s];
            if (sel_ptr_val != 0) {
                // The relocated pointer was already fixed up by macho_apply_relocations
                // Read from the loaded image at the adjusted address
                uint32_t* loaded_sel_ptr = (uint32_t*)(image_base +
                    ((uint32_t*)sel_ptrs + s - (uint32_t*)raw));
                if (*loaded_sel_ptr != 0) {
                    // The loaded pointer points to a C string (selector name)
                    const char* sel_name = (const char*)(*loaded_sel_ptr);
                    if (sel_name && sel_name[0]) {
                        sel_registerName(sel_name);
                    }
                }
            }
        }
    }

    // Also register all method names from __objc_methname as selectors
    if (methname_sect && methname_sect->offset + methname_sect->size <= raw_size) {
        s_printf("[MachO] Found __objc_methname section (");
        char buf[16];
        int_to_str(methname_sect->size, buf);
        s_printf(buf);
        s_printf(" bytes)\n");

        // Walk the method names section - entries are null-terminated C strings
        const char* methnames = (const char*)(raw + methname_sect->offset);
        uint32_t pos = 0;
        while (pos < methname_sect->size) {
            const char* name = methnames + pos;
            if (name[0]) {
                sel_registerName(name);
            }
            // Advance past the null terminator
            while (pos < methname_sect->size && methnames[pos]) pos++;
            pos++;  // Skip the null terminator
        }
    }

    // Process __objc_classlist - register each class with the ObjC runtime
    if (classlist_sect && classlist_sect->offset + classlist_sect->size <= raw_size) {
        s_printf("[MachO] Found __objc_classlist section (");
        char buf[16];
        int_to_str(classlist_sect->size / 4, buf);
        s_printf(buf);
        s_printf(" classes)\n");

        uint32_t* class_ptrs = (uint32_t*)(raw + classlist_sect->offset);
        uint32_t class_count = classlist_sect->size / sizeof(uint32_t);

        for (uint32_t c = 0; c < class_count; c++) {
            if (!image_base) continue;

            // Read the class pointer from the loaded image
            // The classlist contains offsets relative to the image base
            uint32_t* loaded_class_ptr = (uint32_t*)(image_base +
                ((uint32_t*)class_ptrs + c - (uint32_t*)raw));
            uint32_t class_addr = *loaded_class_ptr;

            if (class_addr == 0) continue;

            // The class structure is in the loaded image
            Class cls = (Class)class_addr;
            if (!cls || !cls->name[0]) continue;

            // Check if this class is already registered in the runtime
            Class existing = objc_lookUpClass(cls->name);
            if (existing) {
                s_printf("[MachO] ObjC class already registered: ");
                s_printf(cls->name);
                s_printf("\n");
                continue;
            }

            // Find the superclass - try to look it up by name
            // If cls->superclass points to another class struct in the image,
            // read its name to look up the registered version
            Class superclass = objc_getClass("NSObject");  // Default superclass
            if (cls->superclass) {
                Class raw_super = cls->superclass;
                if (raw_super->name[0]) {
                    Class registered_super = objc_lookUpClass(raw_super->name);
                    if (registered_super) {
                        superclass = registered_super;
                    }
                }
            }

            // Allocate a new class pair in the runtime
            Class new_cls = objc_allocateClassPair(superclass, cls->name,
                cls->instance_size > sizeof(struct objc_object) ?
                cls->instance_size - sizeof(struct objc_object) : 0);
            if (!new_cls) {
                s_printf("[MachO] WARNING: Failed to allocate class: ");
                s_printf(cls->name);
                s_printf("\n");
                continue;
            }

            // Read the method list from the class structure and register methods
            // The class's methods pointer should already be relocated
            if (cls->methods) {
                Method m = cls->methods;
                int method_count = 0;
                while (m && method_count < 256) {  // Safety limit
                    if (m->selector && m->imp) {
                        const char* sel_name = sel_getName(m->selector);
                        if (sel_name && sel_name[0]) {
                            SEL sel = sel_registerName(sel_name);
                            class_addMethod(new_cls, sel, m->imp, m->types);
                        }
                    }
                    m = m->next;
                    method_count++;
                }
            }

            // Register class methods from the metaclass
            if (cls->class_methods) {
                Method m = cls->class_methods;
                int method_count = 0;
                while (m && method_count < 256) {
                    if (m->selector && m->imp) {
                        const char* sel_name = sel_getName(m->selector);
                        if (sel_name && sel_name[0]) {
                            SEL sel = sel_registerName(sel_name);
                            // Add to the metaclass (isa of the new class)
                            if (new_cls->isa) {
                                class_addMethod(new_cls->isa, sel, m->imp, m->types);
                            }
                        }
                    }
                    m = m->next;
                    method_count++;
                }
            }

            objc_registerClassPair(new_cls);

            s_printf("[MachO] Registered ObjC class: ");
            s_printf(cls->name);
            s_printf("\n");
        }
    }

    // Log __objc_protolist
    if (protolist_sect) {
        s_printf("[MachO] Found __objc_protolist section\n");
    }

    // Fix up __objc_classrefs — these are pointers that need to point to the
    // actual registered Class objects. Without this fixup, any code that
    // references a class via __objc_classrefs (e.g., [SomeClass alloc])
    // will crash because the pointer is stale/null.
    if (classrefs_sect && classrefs_sect->offset + classrefs_sect->size <= raw_size && image_base) {
        s_printf("[MachO] Processing __objc_classrefs\n");
        uint32_t* refs = (uint32_t*)(image_base + classrefs_sect->addr);
        uint32_t ref_count = classrefs_sect->size / sizeof(uint32_t);
        for (uint32_t r = 0; r < ref_count; r++) {
            // Each classref initially points to the class struct in the loaded image.
            // We need to replace it with the registered Class pointer from our runtime.
            if (refs[r] != 0) {
                // The pointer may have been relocated to point into the loaded image.
                // Try to read the class name from the struct at that address.
                struct objc_class* candidate = (struct objc_class*)(refs[r]);
                if (candidate && candidate->name[0]) {
                    Class registered = objc_getClass(candidate->name);
                    if (registered) {
                        refs[r] = (uint32_t)registered;
                    }
                }
            }
        }
    }

    // Fix up __objc_superrefs — these point to the superclass struct and need
    // to be updated to point to the registered Class for the superclass
    if (superrefs_sect && superrefs_sect->offset + superrefs_sect->size <= raw_size && image_base) {
        s_printf("[MachO] Processing __objc_superrefs\n");
        uint32_t* refs = (uint32_t*)(image_base + superrefs_sect->addr);
        uint32_t ref_count = superrefs_sect->size / sizeof(uint32_t);
        for (uint32_t r = 0; r < ref_count; r++) {
            if (refs[r] != 0) {
                struct objc_class* candidate = (struct objc_class*)(refs[r]);
                if (candidate && candidate->name[0]) {
                    Class registered = objc_getClass(candidate->name);
                    if (registered) {
                        refs[r] = (uint32_t)registered;
                    }
                }
            }
        }
    }

    // Process S_MOD_INIT_FUNC_POINTERS — these are constructor functions
    // (C++ static initializers, __attribute__((constructor)) functions)
    // that must run before main(). Without this, global C++ objects aren't
    // constructed and constructor functions don't run.
    if (mod_init_sect && mod_init_sect->offset + mod_init_sect->size <= raw_size && image_base) {
        s_printf("[MachO] Processing __mod_init_func constructors\n");
        uint32_t* init_funcs = (uint32_t*)(image_base + mod_init_sect->addr);
        uint32_t func_count = mod_init_sect->size / sizeof(uint32_t);
        for (uint32_t f = 0; f < func_count; f++) {
            if (init_funcs[f] != 0) {
                typedef void (*init_func_t)(void);
                init_func_t fn = (init_func_t)init_funcs[f];
                s_printf("[MachO] Calling constructor function ");
                char addr_buf[16];
                int_to_str((uint32_t)fn, addr_buf);
                s_printf(addr_buf);
                s_printf("\n");
                fn();
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
    
    // Apply relocations to fix up external references and base address adjustments
    macho_apply_relocations(image, raw, fsize, min_vmaddr);

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
