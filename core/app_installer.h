// core/app_installer.h - macOS-like Application Installer
// Implements the drag-to-Applications-folder installation experience
#ifndef APP_INSTALLER_H
#define APP_INSTALLER_H

#include "../include/types.h"

// Installer states (for the GUI animation)
typedef enum {
    INSTALLER_IDLE,
    INSTALLER_MOUNTING_DMG,
    INSTALLER_EXTRACTING,
    INSTALLER_COPYING,
    INSTALLER_INSTALLING,
    INSTALLER_DONE,
    INSTALLER_ERROR
} installer_state_t;

// Installation result
typedef struct {
    int success;
    char app_name[64];
    char app_path[256];
    char error_msg[128];
    uint32_t bytes_copied;
    uint32_t total_bytes;
} install_result_t;

// DMG installer window state
typedef struct {
    int active;
    int dmg_mount_id;
    char dmg_path[256];
    char app_name[64];
    installer_state_t state;
    float progress;
    int show_window;
    // Window handles
    void* window;
} dmg_installer_t;

// Initialize the installer subsystem
void app_installer_init(void);

// Open a .dmg file and show the installer window
// This mounts the DMG and presents the drag-to-Applications UI
int app_installer_open_dmg(const char* dmg_path);

// Install an app from a mounted DMG to /Applications
// This is the core "drag to Applications" logic
int app_installer_install_app(const char* dmg_path, const char* app_name);

// Quick install: mount DMG, find .app bundle, copy to /Applications, unmount
install_result_t app_installer_quick_install(const char* dmg_path);

// Unmount a DMG and close the installer window
int app_installer_close_dmg(const char* dmg_path);

// Render the installer window (called from main GUI loop)
void app_installer_render(dmg_installer_t* installer);

// Check if a path is a .dmg file
int app_installer_is_dmg(const char* path);

// Ensure /Applications directory exists
void app_installer_ensure_applications_dir(void);

// Get installer state for a DMG path
dmg_installer_t* app_installer_get_state(const char* dmg_path);

// Install by copying .app bundle from source to /Applications
// This handles the actual file/directory copy for .app bundles
int app_install_bundle(const char* source_path, const char* dest_dir);

#endif
