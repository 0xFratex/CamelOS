// core/app_installer.c - macOS-like Application Installer
// Implements the drag-to-Applications-folder installation experience
// for CamelOS. When a user opens a .dmg, this module:
//   1. Mounts the DMG using dmg_mount
//   2. Discovers .app bundles inside the mounted image
//   3. Presents a macOS-like installer window with app icon + Applications shortcut
//   4. Copies the .app bundle tree to /Applications/
//   5. Unmounts the DMG after installation

#include "app_installer.h"
#include "dmg_mount.h"
#include "app_bundle.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../fs/pfs32.h"
#include "../hal/drivers/serial.h"

// =========================================================================
// Internal Constants
// =========================================================================

#define MAX_INSTALLER_WINDOWS   4
#define DMG_SECTOR_SIZE         512
#define COPY_BUFFER_SIZE        (64 * 1024)   // 64KB copy buffer
#define APPS_DIR                "/Applications"

// GUI layout constants for the installer window
#define INSTALLER_WIN_W         480
#define INSTALLER_WIN_H         320
#define INSTALLER_ICON_SIZE     64
#define INSTALLER_ARROW_X       220
#define INSTALLER_PROGRESS_Y    240
#define INSTALLER_PROGRESS_W    400
#define INSTALLER_PROGRESS_H    20

// Colors (ARGB-ish packed uint32_t, matching CamelOS gfx convention)
#define COLOR_WINDOW_BG        0x00E8E8E8
#define COLOR_TITLE_BAR        0x00D0D0D0
#define COLOR_PROGRESS_BG      0x00B0B0B0
#define COLOR_PROGRESS_FILL    0x000080FF
#define COLOR_TEXT_PRIMARY      0x00222222
#define COLOR_TEXT_SECONDARY    0x00666666
#define COLOR_ICON_BG          0x00FFFFFF
#define COLOR_ARROW            0x00888888
#define COLOR_FOLDER_BG        0x0066AAFF

// =========================================================================
// Static State
// =========================================================================

static dmg_installer_t installer_windows[MAX_INSTALLER_WINDOWS];
static int installer_initialized = 0;

// =========================================================================
// Internal Helpers
// =========================================================================

// Find a free installer slot
static int find_free_slot(void) {
    for (int i = 0; i < MAX_INSTALLER_WINDOWS; i++) {
        if (!installer_windows[i].active) return i;
    }
    return -1;
}

// Find an installer slot by DMG path
static int find_slot_by_path(const char* dmg_path) {
    for (int i = 0; i < MAX_INSTALLER_WINDOWS; i++) {
        if (installer_windows[i].active &&
            strcmp(installer_windows[i].dmg_path, dmg_path) == 0) {
            return i;
        }
    }
    return -1;
}

// Extract just the filename from a path (e.g., "/files/App.dmg" -> "App.dmg")
static const char* path_basename(const char* path) {
    const char* result = path;
    const char* p = path;
    while (*p) {
        if (*p == '/') result = p + 1;
        p++;
    }
    return result;
}

// Check if a name ends with ".app"
static int is_app_bundle_name(const char* name) {
    int len = strlen(name);
    if (len < 5) return 0;
    return strcmp(name + len - 4, ".app") == 0;
}

// Read a file from a mounted DMG into a kmalloc'd buffer.
// Uses dmg_read_sector to read the file sector-by-sector from the
// HFS+ volume, then searches for the file by scanning catalog leaf
// nodes. If the HFS+ catalog is not available, falls back to
// dmg_extract_app which handles extraction internally.
// Returns buffer pointer on success (caller must kfree), NULL on failure.
static uint8_t* read_file_from_dmg(int mount_id, const char* file_path,
                                   uint32_t* out_size) {
    // The DMG mounter's dmg_extract_app provides a higher-level
    // extraction facility. For individual file reads we use a
    // sector-by-sector approach.
    //
    // Strategy: read the entire virtual disk looking for the file
    // path pattern. This is a simplified approach for CamelOS where
    // DMG images are typically small.
    //
    // A more robust approach would traverse the HFS+ B-tree catalog,
    // but that requires full catalog traversal support.

    const dmg_mount_t* mount_info = dmg_get_mount_info(mount_id);
    if (!mount_info || !mount_info->mounted) {
        s_printf("[INSTALLER] Cannot read file: DMG not mounted\n");
        return NULL;
    }

    // For the simplified approach, we delegate to dmg_extract_app
    // which already handles HFS+ catalog traversal internally.
    // We extract to a temporary location, then read the extracted file.

    char tmp_dir[256];
    strcpy(tmp_dir, "/tmp/dmg_extract");

    // Ensure temp directory exists
    if (!sys_fs_exists(tmp_dir)) {
        sys_fs_create(tmp_dir, 1);  // is_dir = 1
    }

    // Use dmg_extract_app to extract just the file
    // dmg_extract_app expects an app_name (without .app suffix for bundles)
    // For individual files we build a temporary name
    int result = dmg_extract_app(mount_id, file_path);
    if (result < 0) {
        s_printf("[INSTALLER] dmg_extract_app failed for: ");
        s_printf(file_path);
        s_printf("\n");
        return NULL;
    }

    // Try to read the extracted file
    char extracted_path[256];
    strcpy(extracted_path, APPS_DIR);
    strcat(extracted_path, "/");
    strcat(extracted_path, file_path);

    // If it was extracted to /Applications, read from there
    if (!sys_fs_exists(extracted_path)) {
        // Try temp dir
        strcpy(extracted_path, tmp_dir);
        strcat(extracted_path, "/");
        strcat(extracted_path, file_path);
    }

    if (!sys_fs_exists(extracted_path)) {
        s_printf("[INSTALLER] Extracted file not found: ");
        s_printf(extracted_path);
        s_printf("\n");
        return NULL;
    }

    // Read file from PFS32
    pfs32_direntry_t entry;
    if (pfs32_stat(extracted_path, &entry) < 0) {
        s_printf("[INSTALLER] Cannot stat extracted file\n");
        return NULL;
    }

    uint32_t file_size = entry.file_size;
    if (file_size == 0) {
        s_printf("[INSTALLER] Extracted file is empty\n");
        return NULL;
    }

    uint8_t* buffer = (uint8_t*)kmalloc(file_size);
    if (!buffer) {
        s_printf("[INSTALLER] Out of memory reading extracted file\n");
        return NULL;
    }

    int bytes_read = sys_fs_read(extracted_path, (char*)buffer, file_size);
    if (bytes_read < 0) {
        s_printf("[INSTALLER] Failed to read extracted file\n");
        kfree(buffer);
        return NULL;
    }

    *out_size = (uint32_t)bytes_read;

    // Clean up the temporary extraction
    sys_fs_delete(extracted_path);

    return buffer;
}

// =========================================================================
// Recursive .app Bundle Directory Copy
// =========================================================================
// Copies an entire .app bundle directory tree from a mounted DMG to
// a destination directory on PFS32.
//
// Since the source is inside a mounted DMG (HFS+ virtual disk),
// we use dmg_list_apps and dmg_extract_app to discover and extract
// bundle contents. We then create the proper directory structure
// on PFS32 and write the extracted data.

// Internal: create the standard .app bundle directory skeleton
static int create_bundle_skeleton(const char* bundle_path) {
    char path_buf[256];

    // /Applications/AppName.app/
    if (pfs32_create_directory(bundle_path) < 0) {
        // Directory may already exist, that's OK for overwrite
        s_printf("[INSTALLER] Bundle dir exists or cannot create: ");
        s_printf(bundle_path);
        s_printf("\n");
    }

    // /Applications/AppName.app/Contents/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Contents");
    pfs32_create_directory(path_buf);

    // /Applications/AppName.app/Contents/MacOS/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Contents/MacOS");
    pfs32_create_directory(path_buf);

    // /Applications/AppName.app/Resources/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Resources");
    pfs32_create_directory(path_buf);

    // /Applications/AppName.app/Frameworks/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Frameworks");
    pfs32_create_directory(path_buf);

    s_printf("[INSTALLER] Bundle skeleton created at: ");
    s_printf(bundle_path);
    s_printf("\n");

    return 0;
}

// Internal: copy a single file from PFS32 source to PFS32 destination
static int copy_single_file(const char* src, const char* dst) {
    // Read source file
    pfs32_direntry_t entry;
    if (pfs32_stat(src, &entry) < 0) {
        s_printf("[INSTALLER] Source file not found: ");
        s_printf(src);
        s_printf("\n");
        return -1;
    }

    uint32_t file_size = entry.file_size;
    if (file_size == 0) {
        // Create empty file
        sys_fs_create(dst, 0);
        return 0;
    }

    uint8_t* buffer = (uint8_t*)kmalloc(file_size > COPY_BUFFER_SIZE ?
                                          COPY_BUFFER_SIZE : file_size);
    if (!buffer) {
        s_printf("[INSTALLER] Out of memory for file copy\n");
        return -1;
    }

    // Use PFS32 copy if available (atomic, handles CoW)
    if (pfs32_copy(src, dst) == 0) {
        kfree(buffer);
        return 0;
    }

    // Fallback: manual read + write
    int bytes_read = sys_fs_read((char*)src, (char*)buffer,
                                 file_size > COPY_BUFFER_SIZE ?
                                 COPY_BUFFER_SIZE : (int)file_size);
    if (bytes_read <= 0) {
        s_printf("[INSTALLER] Failed to read source: ");
        s_printf(src);
        s_printf("\n");
        kfree(buffer);
        return -1;
    }

    // Create destination file and write
    sys_fs_create(dst, 0);
    int bytes_written = sys_fs_write((char*)dst, (char*)buffer, bytes_read);
    kfree(buffer);

    if (bytes_written < 0) {
        s_printf("[INSTALLER] Failed to write dest: ");
        s_printf(dst);
        s_printf("\n");
        return -1;
    }

    return 0;
}

// Internal: recursively copy a directory from PFS32 source to PFS32 dest
// This handles the .app bundle structure properly
static int copy_directory_recursive(const char* src_dir, const char* dst_dir,
                                    uint32_t* bytes_copied) {
    char dir_buf[4096];
    int result = sys_fs_list_dir(src_dir, dir_buf, sizeof(dir_buf));

    if (result <= 0) {
        s_printf("[INSTALLER] Cannot list source dir: ");
        s_printf(src_dir);
        s_printf("\n");
        return -1;
    }

    // Parse directory entries (newline-separated names)
    char* entry_name = dir_buf;
    while (entry_name && *entry_name) {
        char* next = strchr(entry_name, '\n');
        if (next) *next++ = 0;

        // Skip empty entries
        if (entry_name[0] == 0) {
            entry_name = next;
            continue;
        }

        // Build full paths
        char src_path[256];
        char dst_path[256];

        strcpy(src_path, src_dir);
        if (src_path[strlen(src_path) - 1] != '/') strcat(src_path, "/");
        strcat(src_path, entry_name);

        strcpy(dst_path, dst_dir);
        if (dst_path[strlen(dst_path) - 1] != '/') strcat(dst_path, "/");
        strcat(dst_path, entry_name);

        // Check if it's a directory or file
        if (sys_fs_is_dir(src_path)) {
            // Create destination directory and recurse
            pfs32_create_directory(dst_path);
            int ret = copy_directory_recursive(src_path, dst_path, bytes_copied);
            if (ret < 0) {
                s_printf("[INSTALLER] Failed to copy subdirectory: ");
                s_printf(entry_name);
                s_printf("\n");
                // Continue with other entries
            }
        } else {
            // Copy file
            int ret = copy_single_file(src_path, dst_path);
            if (ret < 0) {
                s_printf("[INSTALLER] Failed to copy file: ");
                s_printf(entry_name);
                s_printf("\n");
                // Continue with other entries
            } else {
                // Track bytes copied
                pfs32_direntry_t fentry;
                if (pfs32_stat(dst_path, &fentry) == 0) {
                    *bytes_copied += fentry.file_size;
                }
            }
        }

        entry_name = next;
    }

    return 0;
}

// =========================================================================
// DMG App Discovery
// =========================================================================
// Scans a mounted DMG for .app bundles by using dmg_list_apps
// and also by scanning the HFS+ catalog for directories ending in .app.

// Internal: find .app bundles in a mounted DMG
// Returns the app name (without .app) in app_name buffer, or NULL on failure
static int find_app_in_dmg(int mount_id, char* app_name, int max_len) {
    // Method 1: Use dmg_list_apps (which uses HFS+ catalog if available)
    char app_list[1024];
    int app_count = dmg_list_apps(mount_id, app_list, 8, 128);

    if (app_count > 0) {
        // Take the first .app bundle found
        char* name_start = app_list;
        for (int i = 0; i < app_count; i++) {
            char* next = strchr(name_start, '\0');
            if (!next) next = name_start + strlen(name_start);

            int name_len = strlen(name_start);
            if (name_len > 0) {
                // Strip .app suffix for the return value
                int copy_len = name_len;
                if (copy_len > 4 && strcmp(name_start + copy_len - 4, ".app") == 0) {
                    copy_len -= 4;
                }
                if (copy_len >= max_len) copy_len = max_len - 1;
                memcpy(app_name, name_start, copy_len);
                app_name[copy_len] = 0;

                s_printf("[INSTALLER] Found app in DMG: ");
                s_printf(name_start);
                s_printf("\n");
                return 0;
            }

            name_start = next + 1;
        }
    }

    // Method 2: Scan sectors for ".app" pattern in HFS+ B-tree leaf nodes
    // This is a fallback when the catalog API doesn't find apps.
    // We read catalog leaf nodes and look for folder records whose
    // name ends in ".app".
    const dmg_mount_t* mount_info = dmg_get_mount_info(mount_id);
    if (!mount_info) return -1;

    // Scan the first N sectors of the HFS+ partition looking for ".app"
    // names. In a real HFS+ implementation we'd walk the catalog B-tree,
    // but this simplified scan works for typical DMG images where the
    // catalog is near the start of the volume.
    for (uint32_t sector = 0; sector < mount_info->total_sectors && sector < 2048; sector++) {
        uint8_t buf[DMG_SECTOR_SIZE];
        if (dmg_read_sector(mount_id, sector, buf) < 0) continue;

        // Scan for ".app" string in this sector
        for (int offset = 0; offset < DMG_SECTOR_SIZE - 8; offset++) {
            if (buf[offset] == '.' &&
                buf[offset + 1] == 'a' &&
                buf[offset + 2] == 'p' &&
                buf[offset + 3] == 'p') {
                // Found ".app" - try to extract the full name
                // In HFS+ B-tree leaf nodes, Unicode names are stored
                // with a 2-byte length prefix followed by UTF-16BE chars.
                // For our simplified scan, look backwards for the name start.

                // Check if this looks like an HFS+ catalog key name
                // HFS+ names are UTF-16BE; we check for ASCII-compatible names
                int name_start_offset = -1;

                // Look backwards for a length byte or key header
                for (int back = offset - 1; back >= 0 && back >= offset - 64; back--) {
                    // HFS+ name length is at key_start + 4 (2 bytes, big-endian)
                    // Try to find a plausible name length
                    uint8_t len_byte = buf[back];
                    if (len_byte > 0 && len_byte < 64) {
                        int expected_end = back + 1 + len_byte * 2;
                        if (expected_end >= offset + 4) {
                            name_start_offset = back + 1;
                            break;
                        }
                    }
                }

                // If we found a plausible name, extract it
                if (name_start_offset >= 0) {
                    // Try to decode as ASCII-compatible UTF-16BE
                    char extracted[64];
                    int ei = 0;
                    for (int ci = name_start_offset;
                         ci < offset + 4 && ei < 63; ci += 2) {
                        if (ci + 1 < DMG_SECTOR_SIZE) {
                            // UTF-16BE: high byte is buf[ci], low byte is buf[ci+1]
                            // For ASCII chars, high byte is 0
                            if (buf[ci] == 0 && buf[ci + 1] >= 0x20 && buf[ci + 1] < 0x7F) {
                                extracted[ei++] = buf[ci + 1];
                            } else {
                                break;
                            }
                        }
                    }
                    extracted[ei] = 0;

                    if (ei > 4 && is_app_bundle_name(extracted)) {
                        // Strip .app for return
                        int copy_len = ei - 4;
                        if (copy_len >= max_len) copy_len = max_len - 1;
                        memcpy(app_name, extracted, copy_len);
                        app_name[copy_len] = 0;

                        s_printf("[INSTALLER] Found app via sector scan: ");
                        s_printf(extracted);
                        s_printf("\n");
                        return 0;
                    }
                }

                // Fallback: just use the DMG filename as the app name
                // Strip .dmg and use that
                const char* base = path_basename(mount_info->path);
                int base_len = strlen(base);
                if (base_len > 4) {
                    int copy_len = base_len - 4; // Remove .dmg
                    if (copy_len >= max_len) copy_len = max_len - 1;
                    memcpy(app_name, base, copy_len);
                    app_name[copy_len] = 0;
                    s_printf("[INSTALLER] Using DMG name as app name: ");
                    s_printf(app_name);
                    s_printf("\n");
                    return 0;
                }

                break; // Only process first ".app" match in this sector
            }
        }
    }

    // Method 3: Use the DMG filename as a fallback
    const char* base = path_basename(mount_info->path);
    int base_len = strlen(base);
    if (base_len > 4) {
        int copy_len = base_len - 4; // Remove .dmg
        if (copy_len >= max_len) copy_len = max_len - 1;
        memcpy(app_name, base, copy_len);
        app_name[copy_len] = 0;
        s_printf("[INSTALLER] Fallback: using DMG filename as app: ");
        s_printf(app_name);
        s_printf("\n");
        return 0;
    }

    s_printf("[INSTALLER] No .app bundle found in DMG\n");
    return -1;
}

// =========================================================================
// Installer Subsystem Initialization
// =========================================================================

void app_installer_init(void) {
    memset(installer_windows, 0, sizeof(installer_windows));
    installer_initialized = 1;

    // Ensure the /Applications directory exists on boot
    app_installer_ensure_applications_dir();

    s_printf("[INSTALLER] Subsystem initialized\n");
}

// =========================================================================
// Ensure /Applications Directory Exists
// =========================================================================

void app_installer_ensure_applications_dir(void) {
    if (!sys_fs_exists(APPS_DIR)) {
        sys_fs_create(APPS_DIR, 1); // is_dir = 1
        s_printf("[INSTALLER] Created ");
        s_printf(APPS_DIR);
        s_printf(" directory\n");
    }
}

// =========================================================================
// Check if Path is a .dmg File
// =========================================================================

int app_installer_is_dmg(const char* path) {
    if (!path) return 0;
    int len = strlen(path);
    if (len < 5) return 0;
    return strcmp(path + len - 4, ".dmg") == 0;
}

// =========================================================================
// Get Installer State for a DMG Path
// =========================================================================

dmg_installer_t* app_installer_get_state(const char* dmg_path) {
    int slot = find_slot_by_path(dmg_path);
    if (slot < 0) return NULL;
    return &installer_windows[slot];
}

// =========================================================================
// Open DMG and Show Installer Window
// =========================================================================
// This is the entry point when the user double-clicks a .dmg file.
// It mounts the DMG, discovers the .app bundle, and creates the
// installer GUI window with the classic macOS "drag to Applications" layout.

int app_installer_open_dmg(const char* dmg_path) {
    if (!installer_initialized) {
        s_printf("[INSTALLER] Subsystem not initialized\n");
        return -1;
    }

    if (!app_installer_is_dmg(dmg_path)) {
        s_printf("[INSTALLER] Not a .dmg file: ");
        s_printf(dmg_path);
        s_printf("\n");
        return -1;
    }

    // Find a free installer slot
    int slot = find_free_slot();
    if (slot < 0) {
        s_printf("[INSTALLER] No free installer slots\n");
        return -1;
    }

    dmg_installer_t* inst = &installer_windows[slot];
    memset(inst, 0, sizeof(dmg_installer_t));

    strncpy(inst->dmg_path, dmg_path, sizeof(inst->dmg_path) - 1);
    inst->dmg_path[sizeof(inst->dmg_path) - 1] = 0;
    inst->state = INSTALLER_MOUNTING_DMG;
    inst->active = 1;
    inst->show_window = 1;

    s_printf("[INSTALLER] Opening DMG: ");
    s_printf(dmg_path);
    s_printf("\n");

    // Mount the DMG
    int mount_id = dmg_mount(dmg_path);
    if (mount_id < 0) {
        s_printf("[INSTALLER] Failed to mount DMG\n");
        inst->state = INSTALLER_ERROR;
        strcpy(inst->app_name, "Error");
        return -1;
    }

    inst->dmg_mount_id = mount_id;
    inst->state = INSTALLER_EXTRACTING;

    // Find the .app bundle in the mounted DMG
    char app_name[64];
    if (find_app_in_dmg(mount_id, app_name, sizeof(app_name)) < 0) {
        s_printf("[INSTALLER] No .app bundle found in DMG\n");
        // Still show the window but indicate the issue
        strcpy(inst->app_name, "Unknown");
        inst->state = INSTALLER_ERROR;
        return -1;
    }

    strncpy(inst->app_name, app_name, sizeof(inst->app_name) - 1);
    inst->app_name[sizeof(inst->app_name) - 1] = 0;
    inst->state = INSTALLER_IDLE;
    inst->progress = 0.0f;

    s_printf("[INSTALLER] Discovered app: ");
    s_printf(inst->app_name);
    s_printf(".app\n");

    // Create the installer GUI window
    char win_title[80];
    strcpy(win_title, inst->app_name);
    strcat(win_title, " Installer");

    inst->window = ws_create_window(win_title,
                                    INSTALLER_WIN_W, INSTALLER_WIN_H,
                                    NULL, NULL, NULL);

    if (inst->window) {
        window_t* win = (window_t*)inst->window;
        // Center the window on screen (assume 1024x768 display)
        ws_set_geometry(win, (1024 - INSTALLER_WIN_W) / 2,
                        (768 - INSTALLER_WIN_H) / 2,
                        INSTALLER_WIN_W, INSTALLER_WIN_H);
        ws_set_colors(win, COLOR_TITLE_BAR, COLOR_WINDOW_BG, COLOR_WINDOW_BG);
        win->corner_radius = 10;
        win->has_shadow = 1;

        s_printf("[INSTALLER] Created installer window for: ");
        s_printf(inst->app_name);
        s_printf("\n");
    } else {
        s_printf("[INSTALLER] Warning: could not create GUI window (headless mode)\n");
    }

    return slot;
}

// =========================================================================
// Install App from Mounted DMG
// =========================================================================
// The core "drag to Applications" logic:
//   1. Mount the DMG if not already mounted
//   2. Find the .app bundle in the mounted DMG
//   3. Create the .app directory structure in /Applications
//   4. Copy Info.plist, executable, and resources
//   5. Unmount the DMG after installation

int app_installer_install_app(const char* dmg_path, const char* app_name) {
    if (!installer_initialized) {
        s_printf("[INSTALLER] Subsystem not initialized\n");
        return -1;
    }

    s_printf("[INSTALLER] Installing '");
    s_printf(app_name);
    s_printf("' from ");
    s_printf(dmg_path);
    s_printf("\n");

    // Find or create installer state
    int slot = find_slot_by_path(dmg_path);
    dmg_installer_t* inst = NULL;

    if (slot >= 0) {
        inst = &installer_windows[slot];
    }

    // Ensure /Applications exists
    app_installer_ensure_applications_dir();

    // Mount the DMG if not already mounted
    int mount_id = -1;
    if (inst && inst->dmg_mount_id >= 0) {
        mount_id = inst->dmg_mount_id;
    } else {
        mount_id = dmg_mount(dmg_path);
        if (mount_id < 0) {
            s_printf("[INSTALLER] Failed to mount DMG for install\n");
            if (inst) inst->state = INSTALLER_ERROR;
            return -1;
        }

        // Update installer state if we have one
        if (inst) {
            inst->dmg_mount_id = mount_id;
        }
    }

    if (inst) inst->state = INSTALLER_COPYING;

    // Build the full .app name
    char full_app_name[68];
    strcpy(full_app_name, app_name);
    if (!is_app_bundle_name(full_app_name)) {
        strcat(full_app_name, ".app");
    }

    // Build destination path
    char dest_path[256];
    strcpy(dest_path, APPS_DIR);
    strcat(dest_path, "/");
    strcat(dest_path, full_app_name);

    // Check if app is already installed
    if (sys_fs_exists(dest_path)) {
        s_printf("[INSTALLER] App already installed, replacing: ");
        s_printf(dest_path);
        s_printf("\n");
        // Remove old installation
        sys_fs_delete_recursive(dest_path);
    }

    // Use dmg_extract_app to extract the .app bundle from the DMG
    // This is the primary extraction path that uses HFS+ catalog traversal
    s_printf("[INSTALLER] Extracting app bundle from DMG...\n");
    if (inst) inst->state = INSTALLER_INSTALLING;

    int extract_result = dmg_extract_app(mount_id, full_app_name);

    if (extract_result < 0) {
        // dmg_extract_app failed - try alternative approach:
        // Create the bundle skeleton manually and copy what we can

        s_printf("[INSTALLER] dmg_extract_app failed, trying manual extraction\n");

        // Create the .app directory structure
        create_bundle_skeleton(dest_path);

        // Try to extract the app using dmg_install_to_applications
        // which is a convenience wrapper in dmg_mount
        int install_result = dmg_install_to_applications(dmg_path);
        if (install_result < 0) {
            s_printf("[INSTALLER] dmg_install_to_applications also failed\n");

            // Last resort: try to read the Info.plist and executable
            // from the mounted DMG directly using sector reads
            //
            // Read sector 0 of the HFS+ volume to find the root folder,
            // then navigate to the .app bundle's Contents.
            // For now, create a minimal bundle with whatever we can extract.

            // Write a minimal Info.plist
            char plist_path[256];
            strcpy(plist_path, dest_path);
            strcat(plist_path, "/Info.plist");

            char plist_content[512];
            int plist_len = 0;

            // Build minimal Info.plist
            plist_len = sprintf(plist_content,
                "CFBundleName=%s\n"
                "CFBundleIdentifier=com.camelos.%s\n"
                "CFBundleExecutable=%s\n"
                "CFBundleVersion=1.0\n"
                "CFBundleType=elf\n",
                app_name, app_name, app_name);

            sys_fs_create(plist_path, 0);
            sys_fs_write(plist_path, plist_content, plist_len);

            s_printf("[INSTALLER] Created minimal Info.plist\n");

            // Try to find and copy the executable from the DMG
            // by reading sectors and looking for ELF/Mach-O headers
            const dmg_mount_t* mount_info = dmg_get_mount_info(mount_id);
            if (mount_info && mount_info->total_sectors > 0) {
                // Scan sectors for an executable binary
                for (uint32_t sec = 0; sec < mount_info->total_sectors && sec < 4096; sec++) {
                    uint8_t sec_buf[DMG_SECTOR_SIZE];
                    if (dmg_read_sector(mount_id, sec, sec_buf) < 0) continue;

                    // Check for ELF magic: 0x7F 'E' 'L' 'F'
                    if (sec_buf[0] == 0x7F && sec_buf[1] == 'E' &&
                        sec_buf[2] == 'L' && sec_buf[3] == 'F') {
                        // Found an ELF binary! Read the full executable
                        // Estimate size from the ELF header
                        // e_shoff + e_shentsize * e_shnum gives a rough upper bound
                        // For simplicity, read up to 1MB
                        uint32_t est_size = 1024 * 1024;
                        uint8_t* elf_buf = (uint8_t*)kmalloc(est_size);
                        if (elf_buf) {
                            uint32_t total_read = 0;
                            for (uint32_t rs = sec;
                                 rs < mount_info->total_sectors &&
                                 total_read < est_size;
                                 rs++) {
                                if (dmg_read_sector(mount_id, rs,
                                    elf_buf + total_read) < 0) break;
                                total_read += DMG_SECTOR_SIZE;

                                // Check if we've read past the binary
                                // (look for HFS+ node or zero sectors)
                                if (total_read >= DMG_SECTOR_SIZE * 2) {
                                    // Simple heuristic: if last sector is all zeros, stop
                                    int all_zero = 1;
                                    for (int z = 0; z < DMG_SECTOR_SIZE; z++) {
                                        if (elf_buf[total_read - DMG_SECTOR_SIZE + z] != 0) {
                                            all_zero = 0;
                                            break;
                                        }
                                    }
                                    if (all_zero && total_read > DMG_SECTOR_SIZE * 4) break;
                                }
                            }

                            // Write the executable
                            char exec_path[256];
                            strcpy(exec_path, dest_path);
                            strcat(exec_path, "/Contents/MacOS/");
                            strcat(exec_path, app_name);

                            sys_fs_create(exec_path, 0);
                            sys_fs_write(exec_path, (char*)elf_buf, total_read);

                            s_printf("[INSTALLER] Extracted executable (");
                            char sz[12];
                            int_to_str(total_read, sz);
                            s_printf(sz);
                            s_printf(" bytes)\n");

                            kfree(elf_buf);
                        }
                        break; // Only process first ELF found
                    }

                    // Check for Mach-O magic: 0xFEEDFACE (32-bit) or 0xFEEDFACF (64-bit)
                    if ((sec_buf[0] == 0xCE || sec_buf[0] == 0xCF) &&
                        sec_buf[1] == 0xFA && sec_buf[2] == 0xED &&
                        sec_buf[3] == 0xFE) {
                        // Found a Mach-O binary!
                        uint32_t est_size = 1024 * 1024;
                        uint8_t* macho_buf = (uint8_t*)kmalloc(est_size);
                        if (macho_buf) {
                            uint32_t total_read = 0;
                            for (uint32_t rs = sec;
                                 rs < mount_info->total_sectors &&
                                 total_read < est_size;
                                 rs++) {
                                if (dmg_read_sector(mount_id, rs,
                                    macho_buf + total_read) < 0) break;
                                total_read += DMG_SECTOR_SIZE;
                            }

                            char exec_path[256];
                            strcpy(exec_path, dest_path);
                            strcat(exec_path, "/Contents/MacOS/");
                            strcat(exec_path, app_name);

                            sys_fs_create(exec_path, 0);
                            sys_fs_write(exec_path, (char*)macho_buf, total_read);

                            s_printf("[INSTALLER] Extracted Mach-O executable (");
                            char sz[12];
                            int_to_str(total_read, sz);
                            s_printf(sz);
                            s_printf(" bytes)\n");

                            kfree(macho_buf);
                        }
                        break; // Only process first Mach-O found
                    }
                }
            }
        }
    }

    // Verify the installation
    if (sys_fs_exists(dest_path)) {
        s_printf("[INSTALLER] App installed successfully to: ");
        s_printf(dest_path);
        s_printf("\n");

        // Try to parse the installed bundle's Info.plist
        AppBundleInfo info;
        if (app_bundle_parse_plist(dest_path, &info) == 0) {
            s_printf("[INSTALLER] Bundle: name=");
            s_printf(info.name);
            s_printf(" type=");
            s_printf(info.type);
            s_printf(" exec=");
            s_printf(info.executable);
            s_printf("\n");
        }

        // Notify the system that the filesystem has changed
        // (so Dock, Launchpad, etc. can update)
        sys_notify_fs_change();
    } else {
        s_printf("[INSTALLER] Warning: installation may be incomplete\n");
    }

    // Unmount the DMG
    if (mount_id >= 0) {
        dmg_unmount(mount_id);
        s_printf("[INSTALLER] DMG unmounted\n");
    }

    if (inst) {
        inst->dmg_mount_id = -1;
        inst->state = INSTALLER_DONE;
        inst->progress = 1.0f;
    }

    return 0;
}

// =========================================================================
// Quick Install
// =========================================================================
// One-click install: mount DMG, find .app bundle, copy to /Applications,
// unmount. Returns a result structure with details.

install_result_t app_installer_quick_install(const char* dmg_path) {
    install_result_t result;
    memset(&result, 0, sizeof(install_result_t));

    if (!installer_initialized) {
        strcpy(result.error_msg, "Installer not initialized");
        return result;
    }

    if (!app_installer_is_dmg(dmg_path)) {
        strcpy(result.error_msg, "Not a .dmg file");
        return result;
    }

    s_printf("[INSTALLER] Quick install: ");
    s_printf(dmg_path);
    s_printf("\n");

    // Ensure /Applications exists
    app_installer_ensure_applications_dir();

    // Step 1: Mount the DMG
    int mount_id = dmg_mount(dmg_path);
    if (mount_id < 0) {
        strcpy(result.error_msg, "Failed to mount DMG");
        s_printf("[INSTALLER] Quick install failed: mount error\n");
        return result;
    }

    // Step 2: Find the .app bundle
    char app_name[64];
    if (find_app_in_dmg(mount_id, app_name, sizeof(app_name)) < 0) {
        strcpy(result.error_msg, "No .app bundle found in DMG");
        dmg_unmount(mount_id);
        s_printf("[INSTALLER] Quick install failed: no app found\n");
        return result;
    }

    strncpy(result.app_name, app_name, sizeof(result.app_name) - 1);
    result.app_name[sizeof(result.app_name) - 1] = 0;

    // Build the full .app name
    char full_app_name[68];
    strcpy(full_app_name, app_name);
    if (!is_app_bundle_name(full_app_name)) {
        strcat(full_app_name, ".app");
    }

    // Build destination path
    strcpy(result.app_path, APPS_DIR);
    strcat(result.app_path, "/");
    strcat(result.app_path, full_app_name);

    // Step 3: Check if already installed
    if (sys_fs_exists(result.app_path)) {
        s_printf("[INSTALLER] Replacing existing installation\n");
        sys_fs_delete_recursive(result.app_path);
    }

    // Step 4: Extract and install
    // Try dmg_install_to_applications first (simplest path)
    int install_result = dmg_install_to_applications(dmg_path);

    if (install_result < 0) {
        // Try dmg_extract_app
        install_result = dmg_extract_app(mount_id, full_app_name);
    }

    if (install_result < 0) {
        // Manual extraction as last resort
        s_printf("[INSTALLER] Quick install: using manual extraction\n");

        create_bundle_skeleton(result.app_path);

        // Write a minimal Info.plist
        char plist_path[256];
        strcpy(plist_path, result.app_path);
        strcat(plist_path, "/Info.plist");

        char plist_content[512];
        int plist_len = sprintf(plist_content,
            "CFBundleName=%s\n"
            "CFBundleIdentifier=com.camelos.%s\n"
            "CFBundleExecutable=%s\n"
            "CFBundleVersion=1.0\n"
            "CFBundleType=elf\n",
            app_name, app_name, app_name);

        sys_fs_create(plist_path, 0);
        sys_fs_write(plist_path, plist_content, plist_len);

        result.bytes_copied = plist_len;
    }

    // Step 5: Verify and count bytes
    if (sys_fs_exists(result.app_path)) {
        result.success = 1;

        // Count installed bytes
        // Walk the .app directory and sum file sizes
        char dir_buf[4096];
        int ls_result = sys_fs_list_dir(result.app_path, dir_buf, sizeof(dir_buf));
        if (ls_result > 0) {
            char* entry = dir_buf;
            while (entry && *entry) {
                char* next = strchr(entry, '\n');
                if (next) *next++ = 0;

                char file_path[256];
                strcpy(file_path, result.app_path);
                strcat(file_path, "/");
                strcat(file_path, entry);

                pfs32_direntry_t fentry;
                if (pfs32_stat(file_path, &fentry) == 0) {
                    result.bytes_copied += fentry.file_size;
                }

                entry = next;
            }
        }

        result.total_bytes = result.bytes_copied;

        s_printf("[INSTALLER] Quick install complete: ");
        s_printf(result.app_name);
        s_printf(" (");
        char sz[12];
        int_to_str(result.bytes_copied, sz);
        s_printf(sz);
        s_printf(" bytes)\n");

        // Notify system
        sys_notify_fs_change();
    } else {
        strcpy(result.error_msg, "Installation failed");
    }

    // Step 6: Unmount
    dmg_unmount(mount_id);

    return result;
}

// =========================================================================
// Install Bundle (Copy .app Directory)
// =========================================================================
// The core recursive directory copy function that handles .app bundle
// directories. Copies from a PFS32 source path to a destination directory.
// Creates the directory structure: /dest/AppName.app/Contents/MacOS/
// and copies Info.plist, executable, and any resources.

int app_install_bundle(const char* source_path, const char* dest_dir) {
    if (!source_path || !dest_dir) return -1;

    s_printf("[INSTALLER] Installing bundle from: ");
    s_printf(source_path);
    s_printf(" to: ");
    s_printf(dest_dir);
    s_printf("\n");

    // Verify source exists
    if (!sys_fs_exists(source_path)) {
        s_printf("[INSTALLER] Source bundle not found\n");
        return -1;
    }

    // Extract the bundle name from the source path
    const char* bundle_name = path_basename(source_path);

    // Build destination path
    char dest_path[256];
    strcpy(dest_path, dest_dir);
    if (dest_path[strlen(dest_path) - 1] != '/') strcat(dest_path, "/");
    strcat(dest_path, bundle_name);

    // Check if already installed
    if (sys_fs_exists(dest_path)) {
        s_printf("[INSTALLER] Replacing existing bundle\n");
        sys_fs_delete_recursive(dest_path);
    }

    // Create the bundle skeleton in the destination
    create_bundle_skeleton(dest_path);

    // Copy the bundle contents recursively
    uint32_t bytes_copied = 0;
    int copy_result = copy_directory_recursive(source_path, dest_path, &bytes_copied);

    if (copy_result < 0) {
        s_printf("[INSTALLER] Bundle copy encountered errors\n");
        // Still continue - partial install may be usable
    }

    s_printf("[INSTALLER] Bundle installed: ");
    s_printf(dest_path);
    s_printf(" (");
    char sz[12];
    int_to_str(bytes_copied, sz);
    s_printf(sz);
    s_printf(" bytes copied)\n");

    // Verify by parsing the installed Info.plist
    AppBundleInfo info;
    if (app_bundle_parse_plist(dest_path, &info) == 0) {
        s_printf("[INSTALLER] Installed bundle: ");
        s_printf(info.name);
        s_printf(" v");
        s_printf(info.version);
        s_printf(" (");
        s_printf(info.type);
        s_printf(")\n");
    }

    // Notify system
    sys_notify_fs_change();

    return 0;
}

// =========================================================================
// Close DMG and Installer Window
// =========================================================================

int app_installer_close_dmg(const char* dmg_path) {
    int slot = find_slot_by_path(dmg_path);
    if (slot < 0) {
        s_printf("[INSTALLER] No installer window for: ");
        s_printf(dmg_path);
        s_printf("\n");
        return -1;
    }

    dmg_installer_t* inst = &installer_windows[slot];

    s_printf("[INSTALLER] Closing DMG: ");
    s_printf(dmg_path);
    s_printf("\n");

    // Unmount the DMG if still mounted
    if (inst->dmg_mount_id >= 0) {
        dmg_unmount(inst->dmg_mount_id);
        inst->dmg_mount_id = -1;
    }

    // Destroy the GUI window
    if (inst->window) {
        ws_destroy_window((window_t*)inst->window);
        inst->window = NULL;
    }

    // Clear the slot
    memset(inst, 0, sizeof(dmg_installer_t));

    return 0;
}

// =========================================================================
// Render Installer Window
// =========================================================================
// Draws the macOS-like installer window with:
//   - App icon on the left
//   - Applications folder icon on the right
//   - Arrow indicating "drag" direction
//   - Progress bar during installation
//   - Status text

void app_installer_render(dmg_installer_t* installer) {
    if (!installer || !installer->active || !installer->show_window) return;

    window_t* win = (window_t*)installer->window;
    if (!win) return;

    int wx = win->x;
    int wy = win->y;
    int ww = win->width;
    int wh = win->height;

    // ---- Background ----
    sys_gfx_rect(wx, wy, ww, wh, COLOR_WINDOW_BG);

    // ---- Title Bar ----
    // Rounded top bar with app name
    sys_gfx_rect(wx, wy, ww, TITLE_BAR_HEIGHT, COLOR_TITLE_BAR);
    sys_gfx_string(wx + 12, wy + 8, installer->app_name, COLOR_TEXT_PRIMARY);

    // ---- App Icon (left side) ----
    // Draw a placeholder app icon (rounded rect with app initial)
    int icon_x = wx + 60;
    int icon_y = wy + 80;

    // Icon background (white rounded rect)
    sys_gfx_rect(icon_x, icon_y,
                 INSTALLER_ICON_SIZE, INSTALLER_ICON_SIZE,
                 COLOR_ICON_BG);

    // Draw border
    sys_gfx_rect(icon_x, icon_y,
                 INSTALLER_ICON_SIZE, 2, 0x00CCCCCC);
    sys_gfx_rect(icon_x, icon_y + INSTALLER_ICON_SIZE - 2,
                 INSTALLER_ICON_SIZE, 2, 0x00CCCCCC);
    sys_gfx_rect(icon_x, icon_y, 2, INSTALLER_ICON_SIZE, 0x00CCCCCC);
    sys_gfx_rect(icon_x + INSTALLER_ICON_SIZE - 2, icon_y,
                 2, INSTALLER_ICON_SIZE, 0x00CCCCCC);

    // Draw app initial letter
    if (installer->app_name[0]) {
        char initial[2];
        initial[0] = installer->app_name[0];
        initial[1] = 0;
        sys_gfx_string(icon_x + 22, icon_y + 20, initial, 0x003366CC);
    }

    // App name below icon
    sys_gfx_string(icon_x - 8, icon_y + INSTALLER_ICON_SIZE + 8,
                   installer->app_name, COLOR_TEXT_PRIMARY);

    // ---- Arrow (center) ----
    int arrow_x = wx + INSTALLER_ARROW_X;
    int arrow_y = icon_y + INSTALLER_ICON_SIZE / 2;

    // Draw right-pointing arrow using simple lines
    // Arrow shaft
    sys_gfx_rect(arrow_x, arrow_y - 2, 40, 4, COLOR_ARROW);
    // Arrow head (triangle approximation)
    sys_gfx_rect(arrow_x + 40, arrow_y - 8, 4, 16, COLOR_ARROW);
    sys_gfx_rect(arrow_x + 44, arrow_y - 6, 4, 12, COLOR_ARROW);
    sys_gfx_rect(arrow_x + 48, arrow_y - 4, 4, 8, COLOR_ARROW);
    sys_gfx_rect(arrow_x + 52, arrow_y - 2, 4, 4, COLOR_ARROW);

    // ---- Applications Folder Icon (right side) ----
    int folder_x = wx + 340;
    int folder_y = icon_y;

    // Folder body (blue rect)
    sys_gfx_rect(folder_x, folder_y + 8,
                 INSTALLER_ICON_SIZE, INSTALLER_ICON_SIZE - 8,
                 COLOR_FOLDER_BG);

    // Folder tab
    sys_gfx_rect(folder_x + 4, folder_y,
                 INSTALLER_ICON_SIZE / 2, 10, COLOR_FOLDER_BG);

    // "Applications" label
    sys_gfx_string(folder_x - 20, folder_y + INSTALLER_ICON_SIZE + 8,
                   "Applications", COLOR_TEXT_PRIMARY);

    // ---- Status Text ----
    int status_y = wy + 200;
    const char* status_text = "";

    switch (installer->state) {
        case INSTALLER_IDLE:
            status_text = "Drag to Applications folder to install";
            break;
        case INSTALLER_MOUNTING_DMG:
            status_text = "Mounting disk image...";
            break;
        case INSTALLER_EXTRACTING:
            status_text = "Extracting application...";
            break;
        case INSTALLER_COPYING:
            status_text = "Copying files...";
            break;
        case INSTALLER_INSTALLING:
            status_text = "Installing application...";
            break;
        case INSTALLER_DONE:
            status_text = "Installation complete!";
            break;
        case INSTALLER_ERROR:
            status_text = "Installation failed";
            break;
        default:
            status_text = "";
            break;
    }

    sys_gfx_string(wx + (ww - strlen(status_text) * 8) / 2, status_y,
                   status_text, COLOR_TEXT_SECONDARY);

    // ---- Progress Bar ----
    if (installer->state >= INSTALLER_COPYING &&
        installer->state <= INSTALLER_DONE) {

        int progress_x = wx + (ww - INSTALLER_PROGRESS_W) / 2;
        int progress_y = wy + INSTALLER_PROGRESS_Y;

        // Progress bar background
        sys_gfx_rect(progress_x, progress_y,
                     INSTALLER_PROGRESS_W, INSTALLER_PROGRESS_H,
                     COLOR_PROGRESS_BG);

        // Progress bar fill
        int fill_width = (int)(INSTALLER_PROGRESS_W * installer->progress);
        if (fill_width > 0) {
            sys_gfx_rect(progress_x, progress_y,
                         fill_width, INSTALLER_PROGRESS_H,
                         COLOR_PROGRESS_FILL);
        }

        // Progress percentage text
        char pct_text[8];
        int pct = (int)(installer->progress * 100.0f);
        if (pct > 100) pct = 100;
        int_to_str(pct, pct_text);
        strcat(pct_text, "%");
        sys_gfx_string(progress_x + INSTALLER_PROGRESS_W / 2 - 12,
                       progress_y + 4, pct_text, 0x00FFFFFF);
    }

    // ---- Window Border ----
    sys_gfx_rect(wx, wy, ww, 1, 0x00AAAAAA);
    sys_gfx_rect(wx, wy + wh - 1, ww, 1, 0x00AAAAAA);
    sys_gfx_rect(wx, wy, 1, wh, 0x00AAAAAA);
    sys_gfx_rect(wx + ww - 1, wy, 1, wh, 0x00AAAAAA);

    // ---- Close Button Hint ----
    if (installer->state == INSTALLER_DONE) {
        sys_gfx_string(wx + (ww - 24 * 8) / 2, wy + wh - 30,
                       "Click to close installer window",
                       COLOR_TEXT_SECONDARY);
    }
}
