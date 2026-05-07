// core/app_registry.h - CamelOS Application Registry
// System-wide database of all installed and discovered applications.
// Used by Dock, Spotlight, Launcher, and Package Manager to find apps.
#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include "app_bundle.h"

// Maximum number of apps the registry can hold
#define REG_MAX_APPS          128

// Maximum categories per app (reserved for future use)
#define REG_MAX_CATEGORIES    8

// Path to the persistent registry database on disk
#define REG_DB_PATH           "/Library/PackageManager/app_registry.db"

// App categories
typedef enum {
    APP_CAT_ALL = 0,
    APP_CAT_PRODUCTIVITY,
    APP_CAT_UTILITIES,
    APP_CAT_INTERNET,
    APP_CAT_MEDIA,
    APP_CAT_DEVELOPER,
    APP_CAT_SYSTEM,
    APP_CAT_OTHER
} app_category_t;

// Extended app info (beyond AppBundleInfo)
typedef struct {
    AppBundleInfo bundle_info;     // Base info from Info.plist
    char install_path[256];        // Full path to .app bundle
    app_category_t category;       // App category
    int launch_count;              // Times launched
    uint32_t last_launch_time;     // Last launch timestamp
    int is_builtin;                // 1 if compiled into kernel
    int is_visible;                // 1 if shown in Dock/Launcher
    char source[64];               // "builtin", "cpkg", "dmg", "manual"
} app_registry_entry_t;

// Initialize the app registry (call once at boot)
void app_registry_init(void);

// Scan /Applications and /System/Applications for .app bundles
// Called at boot time after filesystem is mounted
int app_registry_scan_applications(void);

// Register a built-in app (compiled into kernel)
int app_registry_register_builtin(const char* name, const char* identifier,
                                   const char* executable, const char* icon,
                                   app_category_t category);

// Register an installed app from a .app bundle path
int app_registry_register_app(const char* app_path, const char* source);

// Unregister an app by name or identifier
int app_registry_unregister(const char* name_or_id);

// Find an app by name (exact match, then case-insensitive partial match)
// Returns registry index, or -1 if not found
int app_registry_find(const char* name);

// Find an app by bundle identifier (exact match)
// Returns registry index, or -1 if not found
int app_registry_find_by_id(const char* identifier);

// Get registry entry by index (returns NULL if invalid)
const app_registry_entry_t* app_registry_get(int index);

// Get registry entry by name (returns NULL if not found)
const app_registry_entry_t* app_registry_get_by_name(const char* name);

// List apps by category into output array
// Returns number of apps found
int app_registry_list_by_category(app_category_t category,
                                   app_registry_entry_t* out, int max);

// Search apps by name (partial match, case-insensitive)
// Returns number of matches
int app_registry_search(const char* query, app_registry_entry_t* out, int max);

// Record an app launch (increment launch count, update timestamp)
void app_registry_record_launch(const char* name);

// Get total registered app count
int app_registry_get_count(void);

// Save registry to disk (key=value text format)
int app_registry_save(void);

// Load registry from disk (merges with current registry)
int app_registry_load(void);

#endif // APP_REGISTRY_H
