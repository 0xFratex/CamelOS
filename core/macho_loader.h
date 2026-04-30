// core/macho_loader.h - Mach-O Binary Loader for CamelOS
// Loads macOS Mach-O executables using the ravynos SDK/KDK approach
// Supports MH_MAGIC (32-bit) Mach-O format for macOS app compatibility
#ifndef MACHO_LOADER_H
#define MACHO_LOADER_H

#include "../include/types.h"

// Mach-O Magic Numbers
#define MH_MAGIC    0xfeedface  // 32-bit
#define MH_MAGIC_64 0xfeedfacf  // 64-bit (not supported on CamelOS)
#define FAT_MAGIC   0xcafebabe  // Universal binary (fat)

// Mach-O File Types
#define MH_OBJECT    0x1   // Relocatable object file
#define MH_EXECUTE   0x2   // Demand paged executable
#define MH_FVMLIB    0x3   // Fixed VM shared library
#define MH_CORE      0x4   // Core dump
#define MH_PRELOAD   0x5   // Preloaded executable
#define MH_DYLIB     0x6   // Dynamic library
#define MH_DYLINKER  0x7   // Dynamic linker
#define MH_BUNDLE    0x8   // Loadable bundle
#define MH_DYLIB_STUB 0x9  // Shared library stub

// CPU Types
#define CPU_TYPE_I386    7
#define CPU_TYPE_X86_64  0x01000007
#define CPU_TYPE_ARM     12
#define CPU_TYPE_POWERPC 18

// CPU Subtypes
#define CPU_SUBTYPE_I386_ALL  3
#define CPU_SUBTYPE_X86_ALL   3
#define CPU_SUBTYPE_ARM_ALL   0

// Load Command Types
#define LC_SEGMENT     0x1   // Segment of executable
#define LC_SYMTAB      0x2   // Symbol table
#define LC_SYMSEG      0x3   // Symbol segment
#define LC_THREAD      0x4   // Thread state
#define LC_UNIXTHREAD  0x5   // Thread state (unix)
#define LC_DYSYMTAB    0xB   // Dynamic symbol table
#define LC_LOAD_DYLIB  0xC   // Load dynamic library
#define LC_ID_DYLIB    0xD   // Dynamic library ID
#define LC_LOAD_DYLINKER 0xE // Load dynamic linker
#define LC_UUID        0x1B  // UUID
#define LC_MAIN        0x80000028  // Entry point (new)

// Segment flags
#define SG_HIGHVM      0x1
#define SG_FVMLIB      0x2
#define SG_NORELOC     0x4

// Section types
#define S_REGULAR            0x0
#define S_ZEROFILL           0x1
#define S_CSTRING_LITERALS   0x2
#define S_4BYTE_LITERALS     0x3
#define S_8BYTE_LITERALS     0x4
#define S_LITERAL_POINTERS   0x5
#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS      0x7
#define S_SYMBOL_STUBS       0x8
#define S_MOD_INIT_FUNC_POINTERS    0x9
#define S_MOD_TERM_FUNC_POINTERS    0xA

// Mach-O Header (32-bit)
typedef struct {
    uint32_t magic;       // MH_MAGIC
    uint32_t cputype;     // CPU_TYPE_*
    uint32_t cpusubtype;  // CPU_SUBTYPE_*
    uint32_t filetype;    // MH_EXECUTE, MH_DYLIB, etc.
    uint32_t ncmds;       // Number of load commands
    uint32_t sizeofcmds;  // Size of all load commands
    uint32_t flags;       // Flags
} mach_header_t;

// Load Command
typedef struct {
    uint32_t cmd;         // LC_SEGMENT, etc.
    uint32_t cmdsize;     // Size of this command
} load_command_t;

// Segment Command (32-bit)
typedef struct {
    uint32_t cmd;         // LC_SEGMENT
    uint32_t cmdsize;     // Total size
    char segname[16];     // Segment name
    uint32_t vmaddr;      // Virtual memory address
    uint32_t vmsize;      // Virtual memory size
    uint32_t fileoff;     // File offset
    uint32_t filesize;    // File size
    uint32_t maxprot;     // Maximum protection
    uint32_t initprot;    // Initial protection
    uint32_t nsects;      // Number of sections
    uint32_t flags;       // Segment flags
} segment_command_t;

// Section (32-bit)
typedef struct {
    char sectname[16];    // Section name
    char segname[16];     // Segment name
    uint32_t addr;        // Virtual address
    uint32_t size;        // Size
    uint32_t offset;      // File offset
    uint32_t align;       // Alignment
    uint32_t reloff;      // Relocation offset
    uint32_t nreloc;      // Number of relocations
    uint32_t flags;       // Section flags
    uint32_t reserved1;
    uint32_t reserved2;
} section_t;

// Symbol Table Command
typedef struct {
    uint32_t cmd;         // LC_SYMTAB
    uint32_t cmdsize;
    uint32_t symoff;      // Symbol table offset
    uint32_t nsyms;       // Number of symbols
    uint32_t stroff;      // String table offset
    uint32_t strsize;     // String table size
} symtab_command_t;

// Dynamic Symbol Table Command
typedef struct {
    uint32_t cmd;         // LC_DYSYMTAB
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} dysymtab_command_t;

// nlist (Symbol Table Entry, 32-bit)
typedef struct {
    uint32_t n_strx;      // Index into string table
    uint8_t  n_type;      // Symbol type
    uint8_t  n_sect;      // Section number
    int16_t  n_desc;      // Description
    uint32_t n_value;     // Symbol value (address)
} nlist_t;

// Symbol type bits
#define N_STAB 0xe0
#define N_PEXT 0x10
#define N_TYPE 0x0e
#define N_EXT  0x01
#define N_UNDF 0x00
#define N_ABS  0x02
#define N_SECT 0x0e

// Dylib Command
typedef struct {
    uint32_t cmd;         // LC_LOAD_DYLIB
    uint32_t cmdsize;
    uint32_t name_offset; // Offset to library name
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
} dylib_command_t;

// Loaded Mach-O state
typedef struct {
    mach_header_t header;
    void* base_addr;          // Loaded image base
    uint32_t image_size;      // Total image size
    void* entry_point;        // Entry point function
    char name[64];            // Image name
    int active;
    int is_64bit;             // 0 = 32-bit, 1 = 64-bit (unsupported)
} loaded_macho_t;

// Check if a file is a Mach-O binary
int macho_check_magic(const uint8_t* data, uint32_t size);

// Load a Mach-O binary from filesystem
// Returns loaded_macho_t pointer on success, 0 on failure
loaded_macho_t* macho_load(const char* path);

// Get a symbol from a loaded Mach-O image
void* macho_get_symbol(loaded_macho_t* image, const char* symbol_name);

// Unload a Mach-O image
void macho_unload(loaded_macho_t* image);

// Resolve Objective-C class from a loaded Mach-O
// Uses the __objc_classlist section to find registered classes
Class macho_get_objc_class(loaded_macho_t* image, const char* class_name);

// Execute a Mach-O application (calls entry point)
int macho_execute(loaded_macho_t* image, int argc, char** argv);

#endif // MACHO_LOADER_H
