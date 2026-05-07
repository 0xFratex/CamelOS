// core/software_update.c - Software Update System for CamelOS
// Implements HTTP-based manifest checking, secure download, and OS update
// Provides both a background checker and an on-demand update API
//
// Architecture:
//   1. Fetch update manifest from a configurable URL (e.g., updates.camelos.org)
//   2. Parse JSON manifest: version, build, download_url, sha256, size, release_notes
//   3. Compare against current build version
//   4. Download update package via HTTP/HTTPS
//   5. Verify SHA-256 checksum of downloaded package
//   6. Apply update by replacing system files or full disk image
//   7. Reboot into updated system

#include "software_update.h"
#include "string.h"
#include "memory.h"
#include "sha256.h"
#include "http.h"
#include "dns.h"
#include "../fs/pfs32.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/timer.h"
#include "../sys/api.h"

// ============================================================================
// Current Version Information
// ============================================================================

// These are set at build time and should be updated with each release
#define CAMEL_OS_VERSION_MAJOR  1
#define CAMEL_OS_VERSION_MINOR  2
#define CAMEL_OS_VERSION_PATCH  0
#define CAMEL_OS_BUILD_NUMBER   2026050700

static const char current_version_str[] = "1.2.0";
static const uint32_t current_build = CAMEL_OS_BUILD_NUMBER;

// ============================================================================
// Update Configuration
// ============================================================================

// Default update server (configurable at runtime)
static char update_server[128] = "updates.camelos.org";
static char update_manifest_path[256] = "/manifest.json";
static char update_channel[32] = "stable";  // stable, beta, dev

// Update check interval (in milliseconds, default: 24 hours)
static uint32_t update_check_interval = 24 * 60 * 60 * 1000;
static uint32_t last_check_time = 0;

// State
static update_info_t cached_update_info;
static update_state_t update_state = UPDATE_STATE_IDLE;
static uint32_t download_progress = 0;  // 0-100 percentage

// ============================================================================
// Manifest Parsing (uses NSJSONSerialization via simplified JSON parser)
// ============================================================================

// Simple JSON field extractor - finds a key in JSON and returns its string value
// Returns 0 on success, -1 if key not found
static int json_get_string(const char* json, const char* key, char* value, int max_len) {
    if (!json || !key || !value) return -1;

    // Build search pattern: "key":"
    char pattern[128];
    pattern[0] = '"';
    int klen = strlen(key);
    if (klen > 120) return -1;
    memcpy(pattern + 1, key, klen);
    pattern[1 + klen] = '"';
    pattern[2 + klen] = '\0';

    // Find the key
    const char* pos = strstr(json, pattern);
    if (!pos) return -1;

    // Skip past the key and find the colon
    pos += strlen(pattern);
    while (*pos && *pos != ':') pos++;
    if (!*pos) return -1;
    pos++;  // Skip colon

    // Skip whitespace
    while (*pos == ' ' || *pos == '\t') pos++;

    // Expect a quote
    if (*pos != '"') return -1;
    pos++;  // Skip opening quote

    // Copy the value
    int i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;  // Skip escape character
        }
        value[i++] = *pos++;
    }
    value[i] = '\0';

    return 0;
}

// Simple JSON integer extractor
static int json_get_int(const char* json, const char* key, int* value) {
    if (!json || !key || !value) return -1;

    char pattern[128];
    pattern[0] = '"';
    int klen = strlen(key);
    if (klen > 120) return -1;
    memcpy(pattern + 1, key, klen);
    pattern[1 + klen] = '"';
    pattern[2 + klen] = '\0';

    const char* pos = strstr(json, pattern);
    if (!pos) return -1;

    pos += strlen(pattern);
    while (*pos && *pos != ':') pos++;
    if (!*pos) return -1;
    pos++;

    while (*pos == ' ' || *pos == '\t') pos++;

    // Parse integer
    int sign = 1;
    if (*pos == '-') { sign = -1; pos++; }

    int result = 0;
    while (*pos >= '0' && *pos <= '9') {
        result = result * 10 + (*pos - '0');
        pos++;
    }
    *value = result * sign;

    return 0;
}

// ============================================================================
// Version Comparison
// ============================================================================

// Compare two version strings (e.g., "1.2.0" vs "1.3.1")
// Returns: negative if v1 < v2, 0 if equal, positive if v1 > v2
static int version_compare(const char* v1, const char* v2) {
    if (!v1 || !v2) return 0;

    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    // Parse v1
    const char* p = v1;
    major1 = 0; while (*p >= '0' && *p <= '9') { major1 = major1 * 10 + (*p - '0'); p++; }
    if (*p == '.') p++;
    minor1 = 0; while (*p >= '0' && *p <= '9') { minor1 = minor1 * 10 + (*p - '0'); p++; }
    if (*p == '.') p++;
    patch1 = 0; while (*p >= '0' && *p <= '9') { patch1 = patch1 * 10 + (*p - '0'); p++; }

    // Parse v2
    p = v2;
    major2 = 0; while (*p >= '0' && *p <= '9') { major2 = major2 * 10 + (*p - '0'); p++; }
    if (*p == '.') p++;
    minor2 = 0; while (*p >= '0' && *p <= '9') { minor2 = minor2 * 10 + (*p - '0'); p++; }
    if (*p == '.') p++;
    patch2 = 0; while (*p >= '0' && *p <= '9') { patch2 = patch2 * 10 + (*p - '0'); p++; }

    // Compare
    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

// ============================================================================
// Update Check
// ============================================================================

int software_update_check(update_info_t* info) {
    if (!info) return UPDATE_ERROR_INVALID_ARG;

    update_state = UPDATE_STATE_CHECKING;
    s_printf("[SWUpdate] Checking for updates...\n");

    // Build the manifest URL
    char url[512];
    // Format: http://<server>/manifest.json?channel=<channel>&version=<current>
    strcpy(url, "http://");
    strcat(url, update_server);
    strcat(url, update_manifest_path);
    strcat(url, "?channel=");
    strcat(url, update_channel);
    strcat(url, "&version=");
    strcat(url, current_version_str);

    s_printf("[SWUpdate] Fetching manifest: ");
    s_printf(url);
    s_printf("\n");

    // Resolve the update server's IP address
    char server_ip[16];
    if (dns_resolve(update_server, server_ip, sizeof(server_ip)) != 0) {
        s_printf("[SWUpdate] Failed to resolve update server\n");
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_NETWORK;
    }

    // Fetch the manifest via HTTP GET
    char manifest_buf[4096];
    int manifest_len = http_get(url, manifest_buf, sizeof(manifest_buf) - 1, 0, 0);
    if (manifest_len <= 0) {
        s_printf("[SWUpdate] Failed to fetch manifest\n");
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_NETWORK;
    }
    manifest_buf[manifest_len] = '\0';

    s_printf("[SWUpdate] Manifest received (");
    char buf[16]; int_to_str(manifest_len, buf); s_printf(buf);
    s_printf(" bytes)\n");

    // Parse the manifest
    memset(info, 0, sizeof(update_info_t));

    // Extract version
    if (json_get_string(manifest_buf, "version", info->version, sizeof(info->version)) != 0) {
        s_printf("[SWUpdate] Manifest missing 'version' field\n");
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_MANIFEST;
    }

    // Extract build number
    if (json_get_int(manifest_buf, "build", (int*)&info->build_number) != 0) {
        info->build_number = 0;
    }

    // Extract download URL
    if (json_get_string(manifest_buf, "download_url", info->download_url, sizeof(info->download_url)) != 0) {
        s_printf("[SWUpdate] Manifest missing 'download_url' field\n");
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_MANIFEST;
    }

    // Extract SHA-256 checksum
    if (json_get_string(manifest_buf, "sha256", info->sha256_hex, sizeof(info->sha256_hex)) != 0) {
        info->sha256_hex[0] = '\0';
    }

    // Extract file size
    if (json_get_int(manifest_buf, "size", (int*)&info->file_size) != 0) {
        info->file_size = 0;
    }

    // Extract release notes
    if (json_get_string(manifest_buf, "release_notes", info->release_notes, sizeof(info->release_notes)) != 0) {
        strcpy(info->release_notes, "No release notes available.");
    }

    // Extract minimum compatible version (if specified)
    if (json_get_string(manifest_buf, "min_compatible_version", info->min_compatible_version, sizeof(info->min_compatible_version)) != 0) {
        strcpy(info->min_compatible_version, "0.0.0");
    }

    // Compare versions
    int cmp = version_compare(info->version, current_version_str);
    if (cmp > 0) {
        info->update_available = 1;
        s_printf("[SWUpdate] Update available: ");
        s_printf(current_version_str);
        s_printf(" -> ");
        s_printf(info->version);
        s_printf("\n");
        s_printf("[SWUpdate] Release notes: ");
        s_printf(info->release_notes);
        s_printf("\n");
    } else {
        info->update_available = 0;
        s_printf("[SWUpdate] System is up to date (");
        s_printf(current_version_str);
        s_printf(")\n");
    }

    // Cache the update info
    cached_update_info = *info;
    last_check_time = get_tick_count();
    update_state = UPDATE_STATE_IDLE;

    return info->update_available ? UPDATE_AVAILABLE : UPDATE_UP_TO_DATE;
}

// ============================================================================
// Update Download
// ============================================================================

int software_update_download(const char* download_url, const char* dest_path,
                             update_progress_cb progress_cb) {
    if (!download_url || !dest_path) return UPDATE_ERROR_INVALID_ARG;

    update_state = UPDATE_STATE_DOWNLOADING;
    download_progress = 0;

    s_printf("[SWUpdate] Downloading update from: ");
    s_printf(download_url);
    s_printf("\n");

    // Download the update file via HTTP
    char* file_buf = (char*)kmalloc(1024 * 1024);  // 1MB buffer
    if (!file_buf) {
        s_printf("[SWUpdate] Out of memory for download buffer\n");
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_DISK_SPACE;
    }

    int total_size = http_get(download_url, file_buf, 1024 * 1024, 0, 0);
    if (total_size <= 0) {
        s_printf("[SWUpdate] Download failed\n");
        kfree(file_buf);
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_NETWORK;
    }

    s_printf("[SWUpdate] Downloaded ");
    char buf[16]; int_to_str(total_size, buf); s_printf(buf);
    s_printf(" bytes\n");

    download_progress = 50;  // Download complete, 50% done

    // Write the file to the local filesystem
    int write_result = sys_fs_write(dest_path, file_buf, total_size);
    if (write_result < 0) {
        s_printf("[SWUpdate] Failed to write update file\n");
        kfree(file_buf);
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_DISK_SPACE;
    }

    download_progress = 100;

    // Verify SHA-256 checksum (if provided in manifest)
    if (cached_update_info.sha256_hex[0]) {
        s_printf("[SWUpdate] Verifying checksum...\n");

        // Compute SHA-256 of the downloaded file
        uint8_t hash[32];
        sha256_hash(file_buf, total_size, hash);

        // Convert computed hash to hex string
        char hash_hex[65];
        for (int i = 0; i < 32; i++) {
            char hi = (hash[i] >> 4) < 10 ? ('0' + (hash[i] >> 4)) : ('a' + (hash[i] >> 4) - 10);
            char lo = (hash[i] & 0xF) < 10 ? ('0' + (hash[i] & 0xF)) : ('a' + (hash[i] & 0xF) - 10);
            hash_hex[i * 2] = hi;
            hash_hex[i * 2 + 1] = lo;
        }
        hash_hex[64] = '\0';

        // Compare checksums
        if (strcmp(hash_hex, cached_update_info.sha256_hex) != 0) {
            s_printf("[SWUpdate] CHECKSUM MISMATCH!\n");
            s_printf("[SWUpdate] Expected: ");
            s_printf(cached_update_info.sha256_hex);
            s_printf("\n");
            s_printf("[SWUpdate] Got: ");
            s_printf(hash_hex);
            s_printf("\n");

            // Delete the corrupt file
            sys_fs_delete(dest_path);
            kfree(file_buf);
            update_state = UPDATE_STATE_IDLE;
            return UPDATE_ERROR_CHECKSUM;
        }

        s_printf("[SWUpdate] Checksum verified\n");
    }

    kfree(file_buf);

    if (progress_cb) {
        progress_cb(100, "Download complete");
    }

    s_printf("[SWUpdate] Update saved to: ");
    s_printf(dest_path);
    s_printf("\n");

    update_state = UPDATE_STATE_DOWNLOADED;
    return UPDATE_SUCCESS;
}

// ============================================================================
// Update Installation
// ============================================================================

int software_update_install(const char* update_file) {
    if (!update_file) return UPDATE_ERROR_INVALID_ARG;

    update_state = UPDATE_STATE_INSTALLING;
    s_printf("[SWUpdate] Installing update from: ");
    s_printf(update_file);
    s_printf("\n");

    // Read the update package
    char* pkg_buf = (char*)kmalloc(512 * 1024);  // 512KB buffer
    if (!pkg_buf) {
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_DISK_SPACE;
    }

    int pkg_size = sys_fs_read(update_file, pkg_buf, 512 * 1024);
    if (pkg_size <= 0) {
        s_printf("[SWUpdate] Failed to read update package\n");
        kfree(pkg_buf);
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_DISK_SPACE;
    }

    // The update package format is:
    // - First 4 bytes: magic number (0x434D4C55 = "CMLU" = CamelOS Update)
    // - Next 4 bytes: package version
    // - Next 4 bytes: number of files in the package
    // - Then for each file:
    //   - 64 bytes: target path (null-terminated)
    //   - 4 bytes: file size
    //   - N bytes: file data

    uint32_t* header = (uint32_t*)pkg_buf;
    if (header[0] != 0x434D4C55) {
        s_printf("[SWUpdate] Invalid update package (bad magic number)\n");
        kfree(pkg_buf);
        update_state = UPDATE_STATE_IDLE;
        return UPDATE_ERROR_INSTALL;
    }

    uint32_t pkg_version = header[1];
    uint32_t num_files = header[2];

    s_printf("[SWUpdate] Package version: ");
    char buf[16]; int_to_str(pkg_version, buf); s_printf(buf);
    s_printf(", Files: "); int_to_str(num_files, buf); s_printf(buf);
    s_printf("\n");

    // Extract and install each file
    uint32_t offset = 12;  // Skip header
    for (uint32_t i = 0; i < num_files && offset < (uint32_t)pkg_size; i++) {
        char* target_path = pkg_buf + offset;
        uint32_t file_size = *(uint32_t*)(pkg_buf + offset + 64);
        char* file_data = pkg_buf + offset + 68;

        s_printf("[SWUpdate] Installing: ");
        s_printf(target_path);
        s_printf(" (");
        int_to_str(file_size, buf); s_printf(buf);
        s_printf(" bytes)\n");

        // Write the file to the filesystem
        int result = sys_fs_write(target_path, file_data, file_size);
        if (result < 0) {
            s_printf("[SWUpdate] Failed to install: ");
            s_printf(target_path);
            s_printf("\n");
            // Continue with other files
        }

        offset += 68 + file_size;
    }

    kfree(pkg_buf);

    s_printf("[SWUpdate] Installation complete. Reboot required.\n");

    // Clean up the update file
    sys_fs_delete(update_file);

    update_state = UPDATE_STATE_REBOOT_REQUIRED;
    return UPDATE_SUCCESS;
}

// ============================================================================
// Background Update Checker
// ============================================================================

void software_update_check_background(void) {
    // Only check if enough time has passed since the last check
    uint32_t now = get_tick_count();
    if (last_check_time > 0 && (now - last_check_time) < update_check_interval) {
        return;  // Not time yet
    }

    update_info_t info;
    software_update_check(&info);

    if (info.update_available) {
        // Post a notification about the available update
        extern int notify_post(const char*, const char*, const char*, int, int);
        char title[64];
        strcpy(title, "Software Update");
        char subtitle[64];
        strcpy(subtitle, "CamelOS ");
        strcat(subtitle, info.version);
        strcat(subtitle, " is available");
        notify_post(title, subtitle, info.release_notes, 1, 2);
    }
}

// ============================================================================
// Configuration
// ============================================================================

void software_update_set_server(const char* server) {
    if (!server) return;
    strncpy(update_server, server, sizeof(update_server) - 1);
    update_server[sizeof(update_server) - 1] = '\0';
}

void software_update_set_channel(const char* channel) {
    if (!channel) return;
    strncpy(update_channel, channel, sizeof(update_channel) - 1);
    update_channel[sizeof(update_channel) - 1] = '\0';
}

void software_update_set_check_interval(uint32_t interval_ms) {
    update_check_interval = interval_ms;
}

// ============================================================================
// State Queries
// ============================================================================

update_state_t software_update_get_state(void) {
    return update_state;
}

uint32_t software_update_get_progress(void) {
    return download_progress;
}

const char* software_update_get_current_version(void) {
    return current_version_str;
}

uint32_t software_update_get_current_build(void) {
    return current_build;
}

const update_info_t* software_update_get_cached_info(void) {
    return &cached_update_info;
}

// ============================================================================
// Initialization
// ============================================================================

void software_update_init(void) {
    memset(&cached_update_info, 0, sizeof(cached_update_info));
    update_state = UPDATE_STATE_IDLE;
    download_progress = 0;
    last_check_time = 0;

    s_printf("[SWUpdate] Software Update system initialized (v");
    s_printf(current_version_str);
    s_printf(" build ");
    char buf[16]; int_to_str(current_build, buf); s_printf(buf);
    s_printf(")\n");
}
