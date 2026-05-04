// core/dyld.h - CamelOS Dynamic Linker (dyld-lite)
// Loads Mach-O dylibs, resolves LC_LOAD_DYLIB commands, binds symbols across images
// Provides @rpath, @executable_path, @loader_path resolution
#ifndef DYLD_H
#define DYLD_H

#include "../include/types.h"
#include "macho_loader.h"

// Maximum simultaneous loaded dylibs
#define DYLD_MAX_LIBS        32

// Library search paths (colon-separated conceptually, we use an array)
#define DYLD_MAX_SEARCH_PATHS 8

// Known framework/library paths on CamelOS
#define DYLD_SYSTEM_FRAMWORKS  "/System/Library/Frameworks"
#define DYLD_SYSTEM_LIBS       "/usr/lib"
#define DYLD_LOCAL_LIBS        "/usr/local/lib"

// Dylib load state
typedef enum {
    DYLD_STATE_UNLOADED = 0,
    DYLD_STATE_LOADING  = 1,   // Circular dependency guard
    DYLD_STATE_LOADED   = 2,
    DYLD_STATE_FAILED   = 3
} dyld_load_state_t;

// Loaded dylib record
typedef struct {
    char path[256];             // Resolved absolute path
    char name[64];              // Library name (e.g., "libSystem.B.dylib")
    loaded_macho_t* image;      // Loaded Mach-O image
    dyld_load_state_t state;    // Current load state
    uint32_t ref_count;         // Reference count (how many images depend on this)
    uint32_t timestamp;         // From dylib_command
    uint32_t current_version;   // From dylib_command
    uint32_t compat_version;    // From dylib_command
} dyld_image_t;

// Symbol binding record (for re-exported symbols)
typedef struct {
    char name[128];             // Symbol name (without leading _)
    void* address;              // Resolved address
    dyld_image_t* source;       // Which image provides this symbol
} dyld_binding_t;

// Initialize the dynamic linker
void dyld_init(void);

// Load all dependencies for a Mach-O image (processes LC_LOAD_DYLIB commands)
// Returns 0 on success, -1 on failure
int dyld_load_dependencies(loaded_macho_t* image);

// Load a single dylib by path (with search path resolution)
// Returns the dyld_image_t record, or NULL on failure
dyld_image_t* dyld_load_library(const char* path);

// Resolve a symbol across all loaded images
// Searches: requesting image first, then dependencies, then all loaded images
void* dyld_resolve_symbol(const char* name);

// Resolve a symbol within a specific image's dependency chain
void* dyld_resolve_symbol_in_image(loaded_macho_t* image, const char* name);

// Resolve @rpath, @executable_path, @loader_path prefixes
// Returns a newly allocated resolved path (caller must kfree)
char* dyld_resolve_path(const char* path, const char* executable_path);

// Find a library on the search paths
// Returns a newly allocated resolved path (caller must kfree), or NULL
char* dyld_find_library(const char* name);

// Bind undefined symbols in an image against loaded dylibs
// Returns the number of unresolved symbols (0 = fully bound)
int dyld_bind_image(loaded_macho_t* image);

// Get the dyld_image_t record for a loaded Mach-O image
dyld_image_t* dyld_get_image_record(loaded_macho_t* image);

// Register an already-loaded image with dyld (e.g., the main executable)
void dyld_register_image(loaded_macho_t* image, const char* path);

// Register a symbol in the global symbol table (used by framework stubs)
int dyld_register_global_symbol(const char* name, void* address, dyld_image_t* source);

// Unload a dylib (decrements ref count; unloads if count reaches 0)
void dyld_unload_library(const char* path);

// Set the executable path for @executable_path resolution
void dyld_set_executable_path(const char* path);

// Add a library search path
void dyld_add_search_path(const char* path);

// Get count of loaded libraries
int dyld_get_loaded_count(void);

// Dump loaded libraries (debug)
void dyld_dump_loaded(void);

#endif // DYLD_H
