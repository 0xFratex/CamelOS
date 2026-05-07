// core/software_update.h - Software Update System for CamelOS
// Provides OS update checking, downloading, and installation

#ifndef SOFTWARE_UPDATE_H
#define SOFTWARE_UPDATE_H

#include "../include/types.h"

// Update states
typedef enum {
    UPDATE_STATE_IDLE = 0,
    UPDATE_STATE_CHECKING,
    UPDATE_STATE_DOWNLOADING,
    UPDATE_STATE_DOWNLOADED,
    UPDATE_STATE_INSTALLING,
    UPDATE_STATE_REBOOT_REQUIRED,
    UPDATE_STATE_ERROR
} update_state_t;

// Update result codes
#define UPDATE_SUCCESS           0
#define UPDATE_AVAILABLE         1
#define UPDATE_UP_TO_DATE        2
#define UPDATE_ERROR_INVALID_ARG -1
#define UPDATE_ERROR_NETWORK     -2
#define UPDATE_ERROR_MANIFEST    -3
#define UPDATE_ERROR_CHECKSUM    -4
#define UPDATE_ERROR_DISK_SPACE  -5
#define UPDATE_ERROR_INSTALL     -6
#define UPDATE_ERROR_SIGNATURE   -7

// Update information from the manifest
typedef struct {
    int update_available;                    // 1 if an update is available
    char version[32];                        // New version string (e.g., "1.3.0")
    uint32_t build_number;                   // New build number
    char download_url[256];                  // URL to download the update package
    char sha256_hex[65];                     // Hex-encoded SHA-256 of the package
    uint32_t file_size;                      // Size of the update package in bytes
    char release_notes[512];                 // Release notes text
    char min_compatible_version[32];         // Minimum version required to install
} update_info_t;

// Progress callback for downloads
typedef void (*update_progress_cb)(uint32_t percent, const char* status);

// Initialize the software update system
void software_update_init(void);

// Check for available updates (blocking network call)
// Returns UPDATE_AVAILABLE if an update was found, UPDATE_UP_TO_DATE if not,
// or a negative error code on failure
int software_update_check(update_info_t* info);

// Download an update from the given URL to the specified local path
// progress_cb is called periodically with download progress (0-100)
int software_update_download(const char* download_url, const char* dest_path,
                             update_progress_cb progress_cb);

// Install a previously downloaded update package
// The update file should be a CamelOS Update Package (.cup)
// Returns UPDATE_SUCCESS on success, requires reboot afterward
int software_update_install(const char* update_file);

// Background update checker - call from main event loop
// Only checks if the check interval has elapsed
void software_update_check_background(void);

// Configuration
void software_update_set_server(const char* server);
void software_update_set_channel(const char* channel);
void software_update_set_check_interval(uint32_t interval_ms);

// State queries
update_state_t software_update_get_state(void);
uint32_t software_update_get_progress(void);
const char* software_update_get_current_version(void);
uint32_t software_update_get_current_build(void);
const update_info_t* software_update_get_cached_info(void);

#endif /* SOFTWARE_UPDATE_H */
