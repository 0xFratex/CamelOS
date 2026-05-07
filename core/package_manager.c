// core/package_manager.c - CamelOS Package Manager Implementation
// Handles .cpkg package installation, removal, database management,
// and .dmg-based app installation for the CamelOS ecosystem

#include "package_manager.h"
#include "string.h"
#include "memory.h"
#include "app_bundle.h"
#include "app_installer.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

// =========================================================================
// Static State
// =========================================================================

static pkg_entry_t  pkg_db[PKG_MAX_INSTALLED];
static int          pkg_count = 0;
static char         pkg_last_error[PKG_MAX_ERROR_LEN];
static int          pkg_initialized = 0;

// =========================================================================
// Forward Declarations (Internal Helpers)
// =========================================================================

extern void int_to_str(int, char*);

static void         pkg_set_error(const char* msg);
static int          pkg_find_entry(const char* name_or_id);
static int          pkg_hex_decode(const char* hex, unsigned char* out, int max_len);
static int          pkg_parse_cpkg_meta(const char* data, int data_len, cpkg_metadata_t* meta);
static int          pkg_extract_cpkg_file(const char* data, int data_len, const char* filename,
                                          char* out_buf, int out_max);
static int          pkg_create_bundle_dirs(const char* bundle_path);
static int          pkg_write_plist(const char* bundle_path, const cpkg_metadata_t* meta);
static int          pkg_register(const cpkg_metadata_t* meta, const char* install_path,
                                 const char* source);
static int          pkg_unregister(int entry_idx);
static int          pkg_load_database(void);
static int          pkg_save_database(void);
static void         pkg_ensure_dirs(void);
static const char*  pkg_path_basename(const char* path);

// =========================================================================
// Error Handling
// =========================================================================

static void pkg_set_error(const char* msg) {
    strncpy(pkg_last_error, msg, PKG_MAX_ERROR_LEN - 1);
    pkg_last_error[PKG_MAX_ERROR_LEN - 1] = 0;
}

const char* pkg_get_error(void) {
    return pkg_last_error;
}

// =========================================================================
// Path Helpers
// =========================================================================

// Extract the filename portion of a path (after last '/')
static const char* pkg_path_basename(const char* path) {
    const char* result = path;
    const char* p = path;
    while (*p) {
        if (*p == '/') result = p + 1;
        p++;
    }
    return result;
}

// =========================================================================
// Hex Decode
// =========================================================================

// Decode a hex string into binary output buffer.
// Returns number of bytes decoded, or negative on error.
static int pkg_hex_decode(const char* hex, unsigned char* out, int max_len) {
    if (!hex || !out) return PKG_ERR_INVALID;

    int hex_len = strlen(hex);
    // Hex must have even length
    if (hex_len & 1) return PKG_ERR_PARSE;

    int byte_count = hex_len / 2;
    if (byte_count > max_len) byte_count = max_len;

    for (int i = 0; i < byte_count; i++) {
        unsigned char hi = 0, lo = 0;
        char ch;

        // Decode high nibble
        ch = hex[i * 2];
        if (ch >= '0' && ch <= '9')      hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        else return PKG_ERR_PARSE;

        // Decode low nibble
        ch = hex[i * 2 + 1];
        if (ch >= '0' && ch <= '9')      lo = ch - '0';
        else if (ch >= 'a' && ch <= 'f') lo = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') lo = ch - 'A' + 10;
        else return PKG_ERR_PARSE;

        out[i] = (hi << 4) | lo;
    }

    return byte_count;
}

// =========================================================================
// .cpkg Metadata Parsing
// =========================================================================

// Parse key=value pairs from the [CPKG] header section.
// data points to the beginning of the [CPKG] line; data_len is total length.
// Stops at [FILES], [DATA], or [/CPKG].
static int pkg_parse_cpkg_meta(const char* data, int data_len, cpkg_metadata_t* meta) {
    if (!data || !meta) return PKG_ERR_INVALID;

    memset(meta, 0, sizeof(cpkg_metadata_t));

    // Skip the [CPKG] header line
    const char* p = data;
    const char* end = data + data_len;

    // Find the end of the [CPKG] line
    while (p < end && *p != '\n') p++;
    if (p < end) p++; // skip the newline

    // Parse key=value lines until we hit another section marker
    char line[PKG_MAX_LINE_LEN];
    while (p < end) {
        // Extract one line
        int li = 0;
        while (p < end && *p != '\n' && li < PKG_MAX_LINE_LEN - 1) {
            line[li++] = *p++;
        }
        if (p < end && *p == '\n') p++; // skip newline
        line[li] = 0;

        // Trim trailing CR
        if (li > 0 && line[li - 1] == '\r') line[li - 1] = 0;

        // Empty line — skip
        if (line[0] == 0) continue;

        // Section marker — stop parsing metadata
        if (line[0] == '[') break;

        // Find '=' separator
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = 0;
        char* key = line;
        char* value = eq + 1;

        // Trim leading whitespace from value
        while (*value == ' ' || *value == '\t') value++;

        if (strcmp(key, CPKG_KEY_NAME) == 0) {
            strncpy(meta->name, value, sizeof(meta->name) - 1);
        } else if (strcmp(key, CPKG_KEY_VERSION) == 0) {
            strncpy(meta->version, value, sizeof(meta->version) - 1);
        } else if (strcmp(key, CPKG_KEY_IDENTIFIER) == 0) {
            strncpy(meta->identifier, value, sizeof(meta->identifier) - 1);
        } else if (strcmp(key, CPKG_KEY_TYPE) == 0) {
            strncpy(meta->type, value, sizeof(meta->type) - 1);
        } else if (strcmp(key, CPKG_KEY_EXECUTABLE) == 0) {
            strncpy(meta->executable, value, sizeof(meta->executable) - 1);
        } else if (strcmp(key, CPKG_KEY_ICON) == 0) {
            strncpy(meta->icon, value, sizeof(meta->icon) - 1);
        } else if (strcmp(key, CPKG_KEY_MIN_OS) == 0) {
            strncpy(meta->min_os, value, sizeof(meta->min_os) - 1);
        } else if (strcmp(key, CPKG_KEY_DESC) == 0) {
            strncpy(meta->description, value, sizeof(meta->description) - 1);
        }
    }

    // Validate required fields
    if (meta->name[0] == 0) {
        pkg_set_error("Missing package name in .cpkg metadata");
        return PKG_ERR_PARSE;
    }
    if (meta->identifier[0] == 0) {
        pkg_set_error("Missing package identifier in .cpkg metadata");
        return PKG_ERR_PARSE;
    }
    if (meta->version[0] == 0) {
        // Default version
        strcpy(meta->version, "1.0.0");
    }
    if (meta->type[0] == 0) {
        // Default type
        strcpy(meta->type, APP_TYPE_ELF);
    }
    if (meta->executable[0] == 0) {
        // Default executable = name
        strcpy(meta->executable, meta->name);
    }

    return PKG_OK;
}

// =========================================================================
// .cpkg Data Section Extraction
// =========================================================================

// Find and extract the data for a specific file from the [DATA] section.
// The [DATA] section contains entries of the form:
//   === filename ===
//   <content lines>
// The content is hex-encoded for binary files; text files may be stored
// as-is (depending on whether the content has non-printable chars).
//
// out_buf receives the raw bytes (hex-decoded if applicable).
// Returns number of bytes written to out_buf, or negative on error.
// If the file is not found, returns 0.
static int pkg_extract_cpkg_file(const char* data, int data_len, const char* filename,
                                  char* out_buf, int out_max) {
    if (!data || !filename || !out_buf) return PKG_ERR_INVALID;

    const char* p = data;
    const char* end = data + data_len;

    // Find the [DATA] section
    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (p < end) p++; // skip newline

        // Check for [DATA] marker
        if (line_len == 6 && strncmp(line_start, CPKG_DATA_SECTION, 6) == 0) {
            // Now we're inside the DATA section
            break;
        }
    }

    if (p >= end) {
        // No [DATA] section found
        return 0;
    }

    // Search for the file entry: === filename ===
    char marker[PKG_MAX_LINE_LEN];
    int marker_len = sprintf(marker, "=== %s ===", filename);
    if (marker_len >= PKG_MAX_LINE_LEN) marker_len = PKG_MAX_LINE_LEN - 1;

    int found = 0;
    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (p < end) p++; // skip newline

        // Check for file marker
        if (line_len == marker_len && strncmp(line_start, marker, marker_len) == 0) {
            found = 1;
            break;
        }

        // Check for next file marker (=== ... ===) — skip past other files
        if (line_len >= 7 &&
            line_start[0] == '=' && line_start[1] == '=' && line_start[2] == '=' &&
            line_start[line_len - 3] == '=' && line_start[line_len - 2] == '=' &&
            line_start[line_len - 1] == '=') {
            // Another file's data — skip its content until next marker
            continue;
        }

        // Check for section end
        if (line_len == 7 && strncmp(line_start, CPKG_FOOTER, 7) == 0) break;
    }

    if (!found) return 0;

    // Collect content lines until the next === marker or [/CPKG]
    int out_pos = 0;
    int is_hex = 1; // Assume hex by default; detect if text

    // Temporary buffer for collecting raw content (may be hex)
    char* content_buf = (char*)kmalloc(out_max + 1);
    if (!content_buf) return PKG_ERR_NO_MEM;

    int content_pos = 0;

    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (p < end) p++; // skip newline

        // Check for next file marker
        if (line_len >= 7 &&
            line_start[0] == '=' && line_start[1] == '=' && line_start[2] == '=' &&
            line_start[line_len - 3] == '=' && line_start[line_len - 2] == '=' &&
            line_start[line_len - 1] == '=') {
            break;
        }

        // Check for section end
        if (line_len == 7 && strncmp(line_start, CPKG_FOOTER, 7) == 0) break;

        // Check for another section marker
        if (line_len > 0 && line_start[0] == '[') break;

        // Accumulate content
        if (content_pos + line_len <= out_max) {
            memcpy(content_buf + content_pos, line_start, line_len);
            content_pos += line_len;
        }
    }

    content_buf[content_pos] = 0;

    // Determine if content is hex-encoded:
    // Check if all characters are valid hex digits
    int has_non_hex = 0;
    for (int i = 0; i < content_pos; i++) {
        char c = content_buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            has_non_hex = 1;
            break;
        }
    }

    if (!has_non_hex && content_pos > 0 && (content_pos & 1) == 0) {
        // Looks like hex data — decode it
        int decoded = pkg_hex_decode(content_buf, (unsigned char*)out_buf, out_max);
        if (decoded >= 0) {
            out_pos = decoded;
        } else {
            // Hex decode failed — use raw content
            out_pos = content_pos > out_max ? out_max : content_pos;
            memcpy(out_buf, content_buf, out_pos);
        }
    } else {
        // Plain text content
        out_pos = content_pos > out_max ? out_max : content_pos;
        memcpy(out_buf, content_buf, out_pos);
    }

    kfree(content_buf);
    return out_pos;
}

// =========================================================================
// Bundle Directory Creation
// =========================================================================

// Create the standard .app bundle directory skeleton:
//   /path/AppName.app/
//   /path/AppName.app/Contents/
//   /path/AppName.app/Contents/MacOS/
//   /path/AppName.app/Resources/
//   /path/AppName.app/Frameworks/
static int pkg_create_bundle_dirs(const char* bundle_path) {
    char path_buf[256];

    // .app root directory
    if (!sys_fs_exists(bundle_path)) {
        if (sys_fs_create(bundle_path, 1) < 0) {
            char msg[256];
            sprintf(msg, "Failed to create bundle dir: %s", bundle_path);
            pkg_set_error(msg);
            s_printf("[PKG] Failed to create bundle dir: ");
            s_printf(bundle_path);
            s_printf("\n");
            return PKG_ERR_IO;
        }
    }

    // Contents/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Contents");
    if (!sys_fs_exists(path_buf)) sys_fs_create(path_buf, 1);

    // Contents/MacOS/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Contents/MacOS");
    if (!sys_fs_exists(path_buf)) sys_fs_create(path_buf, 1);

    // Resources/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Resources");
    if (!sys_fs_exists(path_buf)) sys_fs_create(path_buf, 1);

    // Frameworks/
    strcpy(path_buf, bundle_path);
    strcat(path_buf, "/Frameworks");
    if (!sys_fs_exists(path_buf)) sys_fs_create(path_buf, 1);

    s_printf("[PKG] Bundle skeleton created: ");
    s_printf(bundle_path);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Info.plist Generation
// =========================================================================

// Write an Info.plist inside the bundle from .cpkg metadata
static int pkg_write_plist(const char* bundle_path, const cpkg_metadata_t* meta) {
    char plist_path[256];
    char plist_buf[1024];
    int plist_len;

    strcpy(plist_path, bundle_path);
    strcat(plist_path, "/Info.plist");

    // Build Info.plist content in key=value format
    plist_len = sprintf(plist_buf,
        "CFBundleName=%s\n"
        "CFBundleIdentifier=%s\n"
        "CFBundleExecutable=%s\n"
        "CFBundleVersion=%s\n"
        "CFBundleType=%s\n",
        meta->name, meta->identifier, meta->executable,
        meta->version, meta->type);

    // Append optional keys
    if (meta->icon[0]) {
        char icon_line[128];
        int ilen = sprintf(icon_line, "CFBundleIconFile=%s\n", meta->icon);
        if (plist_len + ilen < (int)sizeof(plist_buf)) {
            memcpy(plist_buf + plist_len, icon_line, ilen);
            plist_len += ilen;
        }
    }
    if (meta->min_os[0]) {
        char os_line[64];
        int olen = sprintf(os_line, "CFBundleMinOSVersion=%s\n", meta->min_os);
        if (plist_len + olen < (int)sizeof(plist_buf)) {
            memcpy(plist_buf + plist_len, os_line, olen);
            plist_len += olen;
        }
    }

    // Create the file and write
    if (!sys_fs_exists(plist_path)) {
        sys_fs_create(plist_path, 0);
    }
    int written = sys_fs_write(plist_path, plist_buf, plist_len);
    if (written < 0) {
        char msg[256];
        sprintf(msg, "Failed to write Info.plist: %s", plist_path);
        pkg_set_error(msg);
        s_printf("[PKG] Failed to write Info.plist\n");
        return PKG_ERR_IO;
    }

    s_printf("[PKG] Wrote Info.plist: ");
    s_printf(plist_path);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Package Registration
// =========================================================================

// Register a newly installed package in the in-memory database
static int pkg_register(const cpkg_metadata_t* meta, const char* install_path,
                         const char* source) {
    if (pkg_count >= PKG_MAX_INSTALLED) {
        pkg_set_error("Package database is full");
        s_printf("[PKG] Package database full, cannot register\n");
        return PKG_ERR_FULL;
    }

    pkg_entry_t* entry = &pkg_db[pkg_count];
    memset(entry, 0, sizeof(pkg_entry_t));

    // Copy metadata into AppBundleInfo
    strncpy(entry->info.name, meta->name, BUNDLE_NAME_MAX - 1);
    strncpy(entry->info.identifier, meta->identifier, BUNDLE_ID_MAX - 1);
    strncpy(entry->info.executable, meta->executable, BUNDLE_NAME_MAX - 1);
    strncpy(entry->info.version, meta->version, 15);
    strncpy(entry->info.type, meta->type, 15);
    if (meta->icon[0]) {
        strncpy(entry->info.icon_file, meta->icon, 63);
    }
    if (meta->min_os[0]) {
        strncpy(entry->info.min_os_version, meta->min_os, 15);
    }

    // Copy install details
    strncpy(entry->install_path, install_path, sizeof(entry->install_path) - 1);
    strncpy(entry->source, source, sizeof(entry->source) - 1);

    // Record install date from system clock
    int year, month, day;
    sys_get_date(&year, &month, &day);
    char ybuf[8], mbuf[4], dbuf[4];
    int_to_str(year, ybuf);
    int_to_str(month, mbuf);
    int_to_str(day, dbuf);
    sprintf(entry->install_date, "%s-%s-%s", ybuf, mbuf, dbuf);

    entry->verified = 0;

    pkg_count++;

    char msg[256];
    sprintf(msg, "[PKG] Registered: %s v%s (%s)", meta->name, meta->version, source);
    s_printf(msg);
    s_printf("\n");

    return PKG_OK;
}

// Unregister a package entry from the in-memory database
static int pkg_unregister(int entry_idx) {
    if (entry_idx < 0 || entry_idx >= pkg_count) return PKG_ERR_INVALID;

    // Shift entries down
    for (int i = entry_idx; i < pkg_count - 1; i++) {
        memcpy(&pkg_db[i], &pkg_db[i + 1], sizeof(pkg_entry_t));
    }

    // Zero the last slot
    memset(&pkg_db[pkg_count - 1], 0, sizeof(pkg_entry_t));
    pkg_count--;

    return PKG_OK;
}

// Find a package entry index by name or identifier
static int pkg_find_entry(const char* name_or_id) {
    if (!name_or_id) return -1;

    for (int i = 0; i < pkg_count; i++) {
        if (strcmp(pkg_db[i].info.name, name_or_id) == 0 ||
            strcmp(pkg_db[i].info.identifier, name_or_id) == 0) {
            return i;
        }
    }
    return -1;
}

// =========================================================================
// Database Persistence
// =========================================================================

// Package database format — simple text file at PKG_DB_PATH
// Each package is a block of key=value lines, separated by blank lines:
//
// name=AppName
// version=1.0.0
// identifier=com.camelos.appname
// type=elf
// executable=AppName
// install_path=/Applications/AppName.app
// install_date=2025-1-15
// source=cpkg
// verified=0
//
// name=AnotherApp
// ...

// Ensure database directory exists
static void pkg_ensure_dirs(void) {
    // Ensure /Library exists
    if (!sys_fs_exists("/Library")) {
        sys_fs_create("/Library", 1);
    }
    // Ensure /Library/PackageManager exists
    if (!sys_fs_exists(PKG_LIB_DIR)) {
        sys_fs_create(PKG_LIB_DIR, 1);
        s_printf("[PKG] Created database directory: ");
        s_printf(PKG_LIB_DIR);
        s_printf("\n");
    }
    // Ensure /Applications exists
    if (!sys_fs_exists(PKG_APPS_DIR)) {
        sys_fs_create(PKG_APPS_DIR, 1);
        s_printf("[PKG] Created applications directory: ");
        s_printf(PKG_APPS_DIR);
        s_printf("\n");
    }
}

// Load the package database from disk into memory
static int pkg_load_database(void) {
    char buffer[8192];
    int result;

    pkg_count = 0;

    if (!sys_fs_exists(PKG_DB_PATH)) {
        s_printf("[PKG] No package database found, starting fresh\n");
        return PKG_OK;
    }

    result = sys_fs_read(PKG_DB_PATH, buffer, sizeof(buffer) - 1);
    if (result <= 0) {
        s_printf("[PKG] Failed to read package database\n");
        return PKG_ERR_IO;
    }

    buffer[result] = 0;

    // Parse the database file
    char* line = buffer;
    pkg_entry_t* current = NULL;

    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;

        // Trim trailing CR
        int ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') line[ll - 1] = 0;

        // Blank line — finalize current entry
        if (line[0] == 0) {
            if (current && current->info.name[0]) {
                // Entry is already at pkg_db[pkg_count-1], nothing to do
            }
            current = NULL;
            line = next;
            continue;
        }

        // Parse key=value
        char* eq = strchr(line, '=');
        if (!eq) { line = next; continue; }

        *eq = 0;
        char* key = line;
        char* value = eq + 1;

        // If no current entry, start one
        if (!current) {
            if (pkg_count >= PKG_MAX_INSTALLED) {
                s_printf("[PKG] Package database full during load\n");
                break;
            }
            current = &pkg_db[pkg_count];
            memset(current, 0, sizeof(pkg_entry_t));
            pkg_count++;
        }

        if (strcmp(key, "name") == 0) {
            strncpy(current->info.name, value, BUNDLE_NAME_MAX - 1);
        } else if (strcmp(key, "version") == 0) {
            strncpy(current->info.version, value, 15);
        } else if (strcmp(key, "identifier") == 0) {
            strncpy(current->info.identifier, value, BUNDLE_ID_MAX - 1);
        } else if (strcmp(key, "type") == 0) {
            strncpy(current->info.type, value, 15);
        } else if (strcmp(key, "executable") == 0) {
            strncpy(current->info.executable, value, BUNDLE_NAME_MAX - 1);
        } else if (strcmp(key, "install_path") == 0) {
            strncpy(current->install_path, value, sizeof(current->install_path) - 1);
        } else if (strcmp(key, "install_date") == 0) {
            strncpy(current->install_date, value, sizeof(current->install_date) - 1);
        } else if (strcmp(key, "source") == 0) {
            strncpy(current->source, value, sizeof(current->source) - 1);
        } else if (strcmp(key, "verified") == 0) {
            current->verified = (value[0] == '1') ? 1 : 0;
        } else if (strcmp(key, "icon_file") == 0) {
            strncpy(current->info.icon_file, value, 63);
        } else if (strcmp(key, "min_os_version") == 0) {
            strncpy(current->info.min_os_version, value, 15);
        }

        line = next;
    }

    char msg[64];
    sprintf(msg, "[PKG] Database loaded: %d packages", pkg_count);
    s_printf(msg);
    s_printf("\n");

    return PKG_OK;
}

// Save the in-memory package database to disk
static int pkg_save_database(void) {
    char buffer[8192];
    int pos = 0;

    for (int i = 0; i < pkg_count; i++) {
        pkg_entry_t* e = &pkg_db[i];

        int written = sprintf(buffer + pos,
            "name=%s\n"
            "version=%s\n"
            "identifier=%s\n"
            "type=%s\n"
            "executable=%s\n"
            "install_path=%s\n"
            "install_date=%s\n"
            "source=%s\n"
            "verified=%d\n",
            e->info.name,
            e->info.version,
            e->info.identifier,
            e->info.type,
            e->info.executable,
            e->install_path,
            e->install_date,
            e->source,
            e->verified);

        pos += written;

        // Add optional fields if present
        if (e->info.icon_file[0]) {
            written = sprintf(buffer + pos, "icon_file=%s\n", e->info.icon_file);
            pos += written;
        }
        if (e->info.min_os_version[0]) {
            written = sprintf(buffer + pos, "min_os_version=%s\n", e->info.min_os_version);
            pos += written;
        }

        // Blank line separator
        buffer[pos++] = '\n';

        // Safety check
        if (pos >= (int)sizeof(buffer) - 256) {
            s_printf("[PKG] Warning: database buffer near capacity\n");
            break;
        }
    }

    // Create or overwrite the database file
    if (!sys_fs_exists(PKG_DB_PATH)) {
        sys_fs_create(PKG_DB_PATH, 0);
    }

    int result = sys_fs_write(PKG_DB_PATH, buffer, pos);
    if (result < 0) {
        s_printf("[PKG] Failed to write package database\n");
        return PKG_ERR_IO;
    }

    char msg[64];
    sprintf(msg, "[PKG] Database saved: %d packages", pkg_count);
    s_printf(msg);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Public API — Initialization
// =========================================================================

void pkg_init(void) {
    if (pkg_initialized) return;

    memset(pkg_db, 0, sizeof(pkg_db));
    pkg_count = 0;
    pkg_last_error[0] = 0;

    s_printf("[PKG] Initializing package manager\n");

    // Ensure required directories exist
    pkg_ensure_dirs();

    // Load existing database
    pkg_load_database();

    pkg_initialized = 1;

    s_printf("[PKG] Package manager initialized\n");
}

// =========================================================================
// Public API — Package Verification
// =========================================================================

int pkg_verify(const char* cpkg_path) {
    if (!cpkg_path) {
        pkg_set_error("NULL path provided");
        return PKG_ERR_INVALID;
    }

    // Check the file exists
    if (!sys_fs_exists(cpkg_path)) {
        char msg[256];
        sprintf(msg, "File not found: %s", cpkg_path);
        pkg_set_error(msg);
        return PKG_ERR_NOT_FOUND;
    }

    // Read the file
    char* buffer = (char*)kmalloc(65536);
    if (!buffer) {
        pkg_set_error("Out of memory reading package file");
        return PKG_ERR_NO_MEM;
    }

    int size = sys_fs_read(cpkg_path, buffer, 65535);
    if (size <= 0) {
        pkg_set_error("Failed to read package file");
        kfree(buffer);
        return PKG_ERR_IO;
    }
    buffer[size] = 0;

    // Check for [CPKG] header on the first line
    char* first_newline = strchr(buffer, '\n');
    int first_line_len = first_newline ? (int)(first_newline - buffer) : size;

    if (first_line_len != 6 || strncmp(buffer, CPKG_HEADER_START, 6) != 0) {
        pkg_set_error("Invalid .cpkg: missing [CPKG] header");
        kfree(buffer);
        return PKG_ERR_PARSE;
    }

    // Check for [/CPKG] footer
    int found_footer = 0;
    const char* p = buffer;
    const char* end = buffer + size;
    while (p < end - 6) {
        if (p[0] == '[' && p[1] == '/' && p[2] == 'C' &&
            p[3] == 'P' && p[4] == 'K' && p[5] == 'G' && p[6] == ']') {
            found_footer = 1;
            break;
        }
        p++;
    }

    if (!found_footer) {
        pkg_set_error("Invalid .cpkg: missing [/CPKG] footer");
        kfree(buffer);
        return PKG_ERR_PARSE;
    }

    // Parse metadata and check required fields
    cpkg_metadata_t meta;
    int meta_result = pkg_parse_cpkg_meta(buffer, size, &meta);
    if (meta_result != PKG_OK) {
        kfree(buffer);
        return meta_result;
    }

    kfree(buffer);

    s_printf("[PKG] Package verified: ");
    s_printf(cpkg_path);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Public API — Install from .cpkg
// =========================================================================

int pkg_install(const char* cpkg_path) {
    if (!pkg_initialized) {
        pkg_set_error("Package manager not initialized");
        return PKG_ERR_INTERNAL;
    }

    if (!cpkg_path) {
        pkg_set_error("NULL path provided");
        return PKG_ERR_INVALID;
    }

    s_printf("[PKG] Installing package: ");
    s_printf(cpkg_path);
    s_printf("\n");

    // Verify the package first
    int verify_result = pkg_verify(cpkg_path);
    if (verify_result != PKG_OK) {
        return verify_result;
    }

    // Read the entire .cpkg file
    char* buffer = (char*)kmalloc(65536);
    if (!buffer) {
        pkg_set_error("Out of memory reading package file");
        return PKG_ERR_NO_MEM;
    }

    int size = sys_fs_read(cpkg_path, buffer, 65535);
    if (size <= 0) {
        pkg_set_error("Failed to read package file");
        kfree(buffer);
        return PKG_ERR_IO;
    }
    buffer[size] = 0;

    // Parse metadata
    cpkg_metadata_t meta;
    int meta_result = pkg_parse_cpkg_meta(buffer, size, &meta);
    if (meta_result != PKG_OK) {
        kfree(buffer);
        return meta_result;
    }

    s_printf("[PKG] Package: ");
    s_printf(meta.name);
    s_printf(" v");
    s_printf(meta.version);
    s_printf(" (");
    s_printf(meta.type);
    s_printf(")\n");

    // Handle builtin-type packages — just register in DB, no file creation
    if (strcmp(meta.type, APP_TYPE_BUILTIN) == 0) {
        char bundle_path[256];
        sprintf(bundle_path, "%s/%s.app", PKG_APPS_DIR, meta.name);

        // Check if already installed
        int existing = pkg_find_entry(meta.name);
        if (existing >= 0) {
            s_printf("[PKG] Builtin package already registered: ");
            s_printf(meta.name);
            s_printf("\n");
            kfree(buffer);
            return PKG_ERR_EXISTS;
        }

        // Register the builtin package
        int reg_result = pkg_register(&meta, bundle_path, PKG_SOURCE_BUILTIN);
        if (reg_result != PKG_OK) {
            kfree(buffer);
            return reg_result;
        }

        // Save updated database
        pkg_save_database();
        kfree(buffer);

        s_printf("[PKG] Builtin package registered: ");
        s_printf(meta.name);
        s_printf("\n");

        return PKG_OK;
    }

    // Build the .app bundle path
    char bundle_path[256];
    sprintf(bundle_path, "%s/%s.app", PKG_APPS_DIR, meta.name);

    // Check if already installed
    if (sys_fs_exists(bundle_path)) {
        s_printf("[PKG] App already installed, replacing: ");
        s_printf(bundle_path);
        s_printf("\n");
        sys_fs_delete_recursive(bundle_path);

        // Remove old database entry
        int old_idx = pkg_find_entry(meta.name);
        if (old_idx >= 0) {
            pkg_unregister(old_idx);
        }
    }

    // Also check by identifier
    int id_idx = pkg_find_entry(meta.identifier);
    if (id_idx >= 0) {
        pkg_unregister(id_idx);
    }

    // Create the .app bundle directory structure
    int dir_result = pkg_create_bundle_dirs(bundle_path);
    if (dir_result != PKG_OK) {
        kfree(buffer);
        return dir_result;
    }

    // Parse the [FILES] section to get the list of files to extract
    // Find the [FILES] section
    char* files_section = NULL;
    char* data_section = NULL;
    char* cpkg_end = NULL;

    {
        char* p = buffer;
        char* end = buffer + size;

        while (p < end) {
            char* line_start = p;
            while (p < end && *p != '\n') p++;
            int line_len = (int)(p - line_start);
            if (p < end) p++;

            if (line_len == 7 && strncmp(line_start, CPKG_FILES_SECTION, 7) == 0) {
                files_section = line_start + line_len + 1; // past the newline
            } else if (line_len == 6 && strncmp(line_start, CPKG_DATA_SECTION, 6) == 0) {
                data_section = line_start;
            } else if (line_len == 7 && strncmp(line_start, CPKG_FOOTER, 7) == 0) {
                cpkg_end = line_start;
            }
        }
    }

    // Extract file list from [FILES] section
    if (files_section && data_section) {
        // Parse file names from [FILES] until [DATA]
        char file_list[32][256];
        int file_count = 0;

        char* fp = files_section;
        char* files_end = data_section;

        while (fp < files_end && file_count < 32) {
            char* line_start = fp;
            while (fp < files_end && *fp != '\n') fp++;
            int line_len = (int)(fp - line_start);
            if (fp < files_end) fp++; // skip newline

            // Trim CR
            if (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;

            // Skip empty lines
            if (line_len == 0) continue;

            // Copy filename
            if (line_len < 256) {
                memcpy(file_list[file_count], line_start, line_len);
                file_list[file_count][line_len] = 0;
                file_count++;
            }
        }

        // Now extract each file from the [DATA] section and write it
        char* file_buf = (char*)kmalloc(65536);
        if (!file_buf) {
            pkg_set_error("Out of memory for file extraction");
            kfree(buffer);
            return PKG_ERR_NO_MEM;
        }

        for (int fi = 0; fi < file_count; fi++) {
            char* fname = file_list[fi];

            s_printf("[PKG] Extracting: ");
            s_printf(fname);
            s_printf("\n");

            // Extract file content from [DATA] section
            int content_size = pkg_extract_cpkg_file(buffer, size, fname,
                                                      file_buf, 65535);

            if (content_size < 0) {
                char msg[256];
                sprintf(msg, "Failed to extract file: %s", fname);
                pkg_set_error(msg);
                s_printf("[PKG] Extract failed for: ");
                s_printf(fname);
                s_printf("\n");
                continue;
            }

            if (content_size == 0) {
                s_printf("[PKG] Warning: empty content for: ");
                s_printf(fname);
                s_printf("\n");
                // Still create the file (it might be an empty file)
            }

            // Determine the full path on disk
            // If the filename starts with '/', it's relative to the bundle root
            // If it's just a filename like "Info.plist", it goes to bundle root
            char dest_path[512];
            if (fname[0] == '/') {
                // Absolute path within bundle — place inside the .app directory
                sprintf(dest_path, "%s%s", bundle_path, fname);
            } else {
                // Relative filename — place in bundle root
                sprintf(dest_path, "%s/%s", bundle_path, fname);
            }

            // Ensure parent directory exists
            // Walk the path and create intermediate directories
            {
                char dir_path[512];
                strcpy(dir_path, dest_path);
                // Find last '/'
                char* last_slash = dir_path;
                char* scan = dir_path;
                while (*scan) {
                    if (*scan == '/') last_slash = scan;
                    scan++;
                }
                // Temporarily truncate at last slash
                *last_slash = 0;

                // Create directory chain if needed
                // We create each segment that doesn't exist
                char* seg = dir_path + 1; // skip leading '/'
                while (seg && *seg) {
                    char* next_slash = strchr(seg, '/');
                    if (next_slash) *next_slash = 0;

                    char check_path[512];
                    // Reconstruct path up to this segment
                    char* rebuild = dir_path;
                    int partial_len = (int)(seg - dir_path) + strlen(seg);
                    memcpy(check_path, rebuild, partial_len);
                    check_path[partial_len] = 0;

                    if (!sys_fs_exists(check_path)) {
                        sys_fs_create(check_path, 1);
                    }

                    if (next_slash) {
                        *next_slash = '/';
                        seg = next_slash + 1;
                    } else {
                        seg = NULL;
                    }
                }

                // Restore last slash character (not needed anymore)
            }

            // Create the destination file and write content
            if (!sys_fs_exists(dest_path)) {
                sys_fs_create(dest_path, 0);
            }

            if (content_size > 0) {
                int written = sys_fs_write(dest_path, file_buf, content_size);
                if (written < 0) {
                    char msg[256];
                    sprintf(msg, "Failed to write file: %s", dest_path);
                    pkg_set_error(msg);
                    s_printf("[PKG] Write failed for: ");
                    s_printf(dest_path);
                    s_printf("\n");
                } else {
                    char msg[256];
                    sprintf(msg, "[PKG] Wrote %d bytes: %s", content_size, dest_path);
                    s_printf(msg);
                    s_printf("\n");
                }
            }
        }

        kfree(file_buf);
    }

    // Write the Info.plist if it wasn't already in the [FILES] section
    // (always ensure we have a proper Info.plist)
    {
        char plist_path[256];
        sprintf(plist_path, "%s/Info.plist", bundle_path);

        // Only write if Info.plist doesn't exist (wasn't in the package)
        if (!sys_fs_exists(plist_path)) {
            pkg_write_plist(bundle_path, &meta);
        }
    }

    // Register the package in the database
    int reg_result = pkg_register(&meta, bundle_path, PKG_SOURCE_CPKG);
    if (reg_result != PKG_OK) {
        kfree(buffer);
        return reg_result;
    }

    // Save the database
    pkg_save_database();

    kfree(buffer);

    // Notify system of filesystem change
    sys_notify_fs_change();

    s_printf("[PKG] Installation complete: ");
    s_printf(meta.name);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Public API — Install from .dmg
// =========================================================================

int pkg_install_dmg(const char* dmg_path) {
    if (!pkg_initialized) {
        pkg_set_error("Package manager not initialized");
        return PKG_ERR_INTERNAL;
    }

    if (!dmg_path) {
        pkg_set_error("NULL path provided");
        return PKG_ERR_INVALID;
    }

    if (!app_installer_is_dmg(dmg_path)) {
        pkg_set_error("Not a .dmg file");
        return PKG_ERR_INVALID;
    }

    s_printf("[PKG] Installing from DMG: ");
    s_printf(dmg_path);
    s_printf("\n");

    // Delegate to the DMG installer
    install_result_t result = app_installer_quick_install(dmg_path);
    if (!result.success) {
        char msg[256];
        sprintf(msg, "DMG install failed: %s", result.error_msg);
        pkg_set_error(msg);
        s_printf("[PKG] DMG install failed: ");
        s_printf(result.error_msg);
        s_printf("\n");
        return PKG_ERR_IO;
    }

    // Parse the installed bundle's Info.plist to register it
    char bundle_path[256];
    strcpy(bundle_path, result.app_path);

    AppBundleInfo info;
    memset(&info, 0, sizeof(info));

    if (app_bundle_parse_plist(bundle_path, &info) == 0 && info.name[0]) {
        // Check if already registered
        int existing = pkg_find_entry(info.name);
        if (existing < 0) {
            existing = pkg_find_entry(info.identifier);
        }

        if (existing >= 0) {
            // Update existing entry
            memcpy(&pkg_db[existing].info, &info, sizeof(AppBundleInfo));
            strncpy(pkg_db[existing].install_path, bundle_path,
                    sizeof(pkg_db[existing].install_path) - 1);
            strncpy(pkg_db[existing].source, PKG_SOURCE_DMG,
                    sizeof(pkg_db[existing].source) - 1);
        } else {
            // Register as new entry
            cpkg_metadata_t meta;
            memset(&meta, 0, sizeof(meta));
            strncpy(meta.name, info.name, sizeof(meta.name) - 1);
            strncpy(meta.version, info.version, sizeof(meta.version) - 1);
            strncpy(meta.identifier, info.identifier, sizeof(meta.identifier) - 1);
            strncpy(meta.type, info.type, sizeof(meta.type) - 1);
            strncpy(meta.executable, info.executable, sizeof(meta.executable) - 1);
            strncpy(meta.icon, info.icon_file, sizeof(meta.icon) - 1);
            strncpy(meta.min_os, info.min_os_version, sizeof(meta.min_os) - 1);

            pkg_register(&meta, bundle_path, PKG_SOURCE_DMG);
        }

        pkg_save_database();
    }

    // Notify system
    sys_notify_fs_change();

    s_printf("[PKG] DMG installation complete: ");
    s_printf(result.app_name);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Public API — Remove Package
// =========================================================================

int pkg_remove(const char* name_or_id) {
    if (!pkg_initialized) {
        pkg_set_error("Package manager not initialized");
        return PKG_ERR_INTERNAL;
    }

    if (!name_or_id) {
        pkg_set_error("NULL name/identifier provided");
        return PKG_ERR_INVALID;
    }

    s_printf("[PKG] Removing package: ");
    s_printf(name_or_id);
    s_printf("\n");

    // Find the package entry
    int idx = pkg_find_entry(name_or_id);
    if (idx < 0) {
        char msg[256];
        sprintf(msg, "Package not found: %s", name_or_id);
        pkg_set_error(msg);
        s_printf("[PKG] Package not found: ");
        s_printf(name_or_id);
        s_printf("\n");
        return PKG_ERR_NOT_FOUND;
    }

    pkg_entry_t* entry = &pkg_db[idx];

    // Delete the .app bundle directory if it exists
    if (entry->install_path[0] && sys_fs_exists(entry->install_path)) {
        // Don't delete builtin app directories
        if (strcmp(entry->info.type, APP_TYPE_BUILTIN) != 0) {
            s_printf("[PKG] Deleting bundle: ");
            s_printf(entry->install_path);
            s_printf("\n");

            sys_fs_delete_recursive(entry->install_path);
        } else {
            s_printf("[PKG] Builtin package — skipping file deletion\n");
        }
    }

    // Remove from database
    char removed_name[64];
    strcpy(removed_name, entry->info.name);
    pkg_unregister(idx);

    // Save updated database
    pkg_save_database();

    // Notify system of filesystem change
    sys_notify_fs_change();

    s_printf("[PKG] Package removed: ");
    s_printf(removed_name);
    s_printf("\n");

    return PKG_OK;
}

// =========================================================================
// Public API — List Installed Packages
// =========================================================================

int pkg_list_installed(AppBundleInfo* out_list, int max_count) {
    if (!out_list || max_count <= 0) return 0;

    int count = 0;

    // Method 1: Use in-memory database if available
    if (pkg_count > 0) {
        for (int i = 0; i < pkg_count && count < max_count; i++) {
            memcpy(&out_list[count], &pkg_db[i].info, sizeof(AppBundleInfo));
            count++;
        }
        return count;
    }

    // Method 2: Fall back to scanning /Applications
    char dir_buf[4096];
    int result = sys_fs_list_dir(PKG_APPS_DIR, dir_buf, sizeof(dir_buf));

    if (result > 0) {
        char* entry = dir_buf;
        while (entry && *entry && count < max_count) {
            char* next = strchr(entry, '\n');
            if (next) *next++ = 0;

            int elen = strlen(entry);
            if (elen > 4 && strcmp(entry + elen - 4, ".app") == 0) {
                char full_path[256];
                sprintf(full_path, "%s/%s", PKG_APPS_DIR, entry);
                app_bundle_parse_plist(full_path, &out_list[count]);
                if (out_list[count].name[0] == 0) {
                    // Use directory name (without .app) as fallback
                    strncpy(out_list[count].name, entry, elen - 4);
                }
                count++;
            }

            entry = next;
        }
    }

    return count;
}

// =========================================================================
// Public API — Get Package Info
// =========================================================================

int pkg_get_info(const char* name, AppBundleInfo* info) {
    if (!name || !info) return PKG_ERR_INVALID;

    // Search in-memory database
    int idx = pkg_find_entry(name);
    if (idx >= 0) {
        memcpy(info, &pkg_db[idx].info, sizeof(AppBundleInfo));
        return PKG_OK;
    }

    // Try scanning /Applications directly
    char dir_buf[4096];
    int result = sys_fs_list_dir(PKG_APPS_DIR, dir_buf, sizeof(dir_buf));

    if (result > 0) {
        char* entry = dir_buf;
        while (entry && *entry) {
            char* next = strchr(entry, '\n');
            if (next) *next++ = 0;

            int elen = strlen(entry);
            if (elen > 4 && strcmp(entry + elen - 4, ".app") == 0) {
                // Check if this matches the name
                char app_name[64];
                int name_len = elen - 4;
                if (name_len >= 64) name_len = 63;
                memcpy(app_name, entry, name_len);
                app_name[name_len] = 0;

                if (strcmp(app_name, name) == 0) {
                    char full_path[256];
                    sprintf(full_path, "%s/%s", PKG_APPS_DIR, entry);
                    if (app_bundle_parse_plist(full_path, info) == 0) {
                        return PKG_OK;
                    }
                }
            }

            entry = next;
        }
    }

    pkg_set_error("Package not found");
    return PKG_ERR_NOT_FOUND;
}

// =========================================================================
// Public API — Get Installed Count
// =========================================================================

int pkg_get_count(void) {
    return pkg_count;
}

// =========================================================================
// Public API — Rebuild Database
// =========================================================================

int pkg_rebuild_database(void) {
    if (!pkg_initialized) {
        pkg_set_error("Package manager not initialized");
        return PKG_ERR_INTERNAL;
    }

    s_printf("[PKG] Rebuilding package database...\n");

    // Clear the in-memory database
    pkg_count = 0;
    memset(pkg_db, 0, sizeof(pkg_db));

    // Ensure directories exist
    pkg_ensure_dirs();

    // Scan /Applications for .app bundles
    char dir_buf[4096];
    int result = sys_fs_list_dir(PKG_APPS_DIR, dir_buf, sizeof(dir_buf));

    int found = 0;

    if (result > 0) {
        char* entry = dir_buf;
        while (entry && *entry && pkg_count < PKG_MAX_INSTALLED) {
            char* next = strchr(entry, '\n');
            if (next) *next++ = 0;

            int elen = strlen(entry);
            if (elen > 4 && strcmp(entry + elen - 4, ".app") == 0) {
                char full_path[256];
                sprintf(full_path, "%s/%s", PKG_APPS_DIR, entry);

                // Parse the bundle's Info.plist
                AppBundleInfo info;
                memset(&info, 0, sizeof(info));

                if (app_bundle_parse_plist(full_path, &info) == 0 || info.name[0]) {
                    // Create a database entry
                    pkg_entry_t* db_entry = &pkg_db[pkg_count];
                    memset(db_entry, 0, sizeof(pkg_entry_t));

                    // Copy bundle info
                    memcpy(&db_entry->info, &info, sizeof(AppBundleInfo));

                    // If name wasn't in plist, derive from directory name
                    if (db_entry->info.name[0] == 0) {
                        int name_len = elen - 4;
                        if (name_len >= BUNDLE_NAME_MAX) name_len = BUNDLE_NAME_MAX - 1;
                        memcpy(db_entry->info.name, entry, name_len);
                        db_entry->info.name[name_len] = 0;
                    }

                    // If identifier wasn't in plist, generate one
                    if (db_entry->info.identifier[0] == 0) {
                        sprintf(db_entry->info.identifier, "com.camelos.%s",
                                db_entry->info.name);
                    }

                    // Default type if not specified
                    if (db_entry->info.type[0] == 0) {
                        strcpy(db_entry->info.type, APP_TYPE_ELF);
                    }

                    // Default version if not specified
                    if (db_entry->info.version[0] == 0) {
                        strcpy(db_entry->info.version, "1.0.0");
                    }

                    // Default executable if not specified
                    if (db_entry->info.executable[0] == 0) {
                        strncpy(db_entry->info.executable, db_entry->info.name,
                                BUNDLE_NAME_MAX - 1);
                    }

                    // Set install path
                    strncpy(db_entry->install_path, full_path,
                            sizeof(db_entry->install_path) - 1);

                    // Set source (we don't know, so mark as "unknown")
                    strcpy(db_entry->source, "rebuild");

                    // Set install date to current date
                    int year, month, day;
                    sys_get_date(&year, &month, &day);
                    char ybuf[8], mbuf[4], dbuf[4];
                    int_to_str(year, ybuf);
                    int_to_str(month, mbuf);
                    int_to_str(day, dbuf);
                    sprintf(db_entry->install_date, "%s-%s-%s", ybuf, mbuf, dbuf);

                    db_entry->verified = 0;

                    pkg_count++;
                    found++;

                    s_printf("[PKG] Found: ");
                    s_printf(db_entry->info.name);
                    s_printf(" (");
                    s_printf(db_entry->info.type);
                    s_printf(")\n");
                }
            }

            entry = next;
        }
    }

    // Save the rebuilt database
    pkg_save_database();

    // Notify system
    sys_notify_fs_change();

    char msg[64];
    sprintf(msg, "[PKG] Database rebuilt: %d packages found", found);
    s_printf(msg);
    s_printf("\n");

    return found;
}

// =========================================================================
// Public API — Check for Updates (Placeholder)
// =========================================================================

int pkg_check_updates(void) {
    if (!pkg_initialized) {
        pkg_set_error("Package manager not initialized");
        return PKG_ERR_INTERNAL;
    }

    s_printf("[PKG] Checking for updates...\n");

    // Placeholder: In a real implementation, this would contact an
    // update server via the networking stack and compare installed
    // package versions against available versions.
    //
    // For now, we just report no updates available.

    s_printf("[PKG] No update server configured\n");

    return 0;
}

// =========================================================================
// Public API — Search Packages (Placeholder)
// =========================================================================

int pkg_search(const char* query, char* results, int max_len) {
    if (!pkg_initialized) {
        pkg_set_error("Package manager not initialized");
        return PKG_ERR_INTERNAL;
    }

    if (!query || !results || max_len <= 0) {
        pkg_set_error("Invalid search parameters");
        return PKG_ERR_INVALID;
    }

    s_printf("[PKG] Searching for: ");
    s_printf(query);
    s_printf("\n");

    // Placeholder: In a real implementation, this would query a
    // package repository via the networking stack.
    //
    // For now, we search the local installed packages by name
    // and return matching results.

    results[0] = 0;
    int pos = 0;
    int match_count = 0;

    for (int i = 0; i < pkg_count; i++) {
        // Simple substring match on name and identifier
        int name_match = 0;
        const char* name = pkg_db[i].info.name;
        const char* id = pkg_db[i].info.identifier;

        // Check if query appears in name
        if (name[0]) {
            int nlen = strlen(name);
            int qlen = strlen(query);
            for (int j = 0; j <= nlen - qlen; j++) {
                if (strncmp(name + j, query, qlen) == 0) {
                    name_match = 1;
                    break;
                }
            }
        }

        // Check if query appears in identifier
        if (!name_match && id[0]) {
            int ilen = strlen(id);
            int qlen = strlen(query);
            for (int j = 0; j <= ilen - qlen; j++) {
                if (strncmp(id + j, query, qlen) == 0) {
                    name_match = 1;
                    break;
                }
            }
        }

        if (name_match) {
            int entry_len = strlen(name);
            if (pos + entry_len + 2 < max_len) {
                memcpy(results + pos, name, entry_len);
                pos += entry_len;
                results[pos++] = '\n';
                results[pos] = 0;
                match_count++;
            }
        }
    }

    char msg[64];
    sprintf(msg, "[PKG] Search found %d local matches", match_count);
    s_printf(msg);
    s_printf("\n");

    return match_count;
}
