// core/app_bundle.h - CamelOS Application Bundle Format
// Replaces CDL with macOS-like .app bundle structure for better organization
// and future macOS app compatibility
#ifndef APP_BUNDLE_H
#define APP_BUNDLE_H

#include "../include/types.h"

// App Bundle Structure on disk:
// /Applications/MyApp.app/
//   Info.plist       - Application metadata (key=value format)
//   Contents/MacOS/  - Executable directory
//     MyApp          - Main ELF executable (or Mach-O in future)
//   Resources/       - Icons, sounds, etc.
//   Frameworks/      - Embedded frameworks

// Info.plist keys (simplified text format, not XML)
#define PLIST_KEY_CFNAME         "CFBundleName"
#define PLIST_KEY_CFVERSION      "CFBundleVersion"
#define PLIST_KEY_CFIDENTIFIER   "CFBundleIdentifier"
#define PLIST_KEY_CFEXECUTABLE   "CFBundleExecutable"
#define PLIST_KEY_CFTYPE         "CFBundleType"
#define PLIST_KEY_CFICON         "CFBundleIconFile"
#define PLIST_KEY_CFMINOS        "CFBundleMinOSVersion"
#define PLIST_KEY_CFCDLPATH      "CFBundleCDLPath"

// App bundle types
#define APP_TYPE_CDL_COMPAT      "cdl"      // Legacy CDL-based app
#define APP_TYPE_ELF             "elf"      // Native ELF executable
#define APP_TYPE_MACHO           "macho"    // Mach-O binary (macOS compat)
#define APP_TYPE_OBJC            "objc"     // Objective-C app
#define APP_TYPE_BUILTIN         "builtin"  // Built-in kernel app (compiled in)

// Maximum sizes
#define BUNDLE_PATH_MAX  256
#define BUNDLE_NAME_MAX  64
#define BUNDLE_ID_MAX    128
#define BUNDLE_PLIST_MAX 2048

// App bundle metadata (parsed from Info.plist)
typedef struct {
    char name[BUNDLE_NAME_MAX];
    char identifier[BUNDLE_ID_MAX];
    char executable[BUNDLE_NAME_MAX];
    char version[16];
    char type[16];           // "cdl", "elf", "macho", "objc", "builtin"
    char icon_file[64];
    char min_os_version[16];
    char cdl_path[BUNDLE_PATH_MAX];  // Direct path to CDL file (if type=cdl)
} AppBundleInfo;

// Loaded app bundle state
typedef struct {
    AppBundleInfo info;
    char bundle_path[BUNDLE_PATH_MAX];  // Path to .app directory
    void* loaded_image;                  // Loaded binary image
    uint32_t image_size;
    void* exports;                       // cdl_exports_t or objc_class
    int active;
    int is_macho;                        // 1 if loaded Mach-O, 0 if ELF
} LoadedAppBundle;

// Initialize the app bundle system
void app_bundle_init_system(void);

// Load an app bundle (.app directory or legacy .cdl file)
// Returns slot index on success, -1 on failure
int app_bundle_load(const char* path);

// Get bundle info from a loaded bundle
const AppBundleInfo* app_bundle_get_info(int slot);

// Find a loaded bundle by name
int app_bundle_find(const char* name);

// Unload a bundle
void app_bundle_unload(int slot);

// Parse Info.plist from a bundle directory
int app_bundle_parse_plist(const char* bundle_path, AppBundleInfo* info);

// Resolve an app path to its executable
// Handles both .app bundles and legacy .cdl files
const char* app_bundle_resolve_executable(const char* app_path);

// List all installed applications in /Applications
int app_bundle_list_installed(AppBundleInfo* out_list, int max_count);

#endif // APP_BUNDLE_H
