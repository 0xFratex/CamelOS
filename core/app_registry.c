// core/app_registry.c - CamelOS Application Registry Implementation
// System-wide database of all installed and discovered applications.
// Manages app discovery, registration, querying, and persistence.

#include "app_registry.h"
#include "string.h"
#include "memory.h"
#include "app_bundle.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

// ---------------------------------------------------------------------------
// Static registry storage
// ---------------------------------------------------------------------------
static app_registry_entry_t registry[REG_MAX_APPS];

// ---------------------------------------------------------------------------
// Helper: character lower-case
// ---------------------------------------------------------------------------
static char char_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

// ---------------------------------------------------------------------------
// Helper: case-insensitive string equality
// ---------------------------------------------------------------------------
static int streq_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (char_lower(*a) != char_lower(*b)) return 0;
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

// ---------------------------------------------------------------------------
// Helper: case-insensitive substring search
// Returns 1 if needle is found inside haystack, 0 otherwise
// ---------------------------------------------------------------------------
static int str_contains_ci(const char* haystack, const char* needle) {
    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(haystack);
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (char_lower(haystack[i + j]) != char_lower(needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Helper: find first free slot in registry, or -1
// ---------------------------------------------------------------------------
static int find_free_slot(void) {
    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] == '\0') return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Helper: convert uint32_t to decimal string
// ---------------------------------------------------------------------------
static void uint_to_str(uint32_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// ---------------------------------------------------------------------------
// Helper: parse a decimal integer string to int
// ---------------------------------------------------------------------------
static int atoi_simple(const char* s) {
    if (!s) return 0;
    int result = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

// ---------------------------------------------------------------------------
// Helper: parse a decimal string to uint32_t
// ---------------------------------------------------------------------------
static uint32_t atou_simple(const char* s) {
    if (!s) return 0;
    uint32_t result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Helper: parse a "key=value" line.
// Splits at first '=', trims trailing \r/\n from value.
// Returns 0 on success, -1 if no '=' found.
// ---------------------------------------------------------------------------
static int parse_kv_line(const char* line, char* key, int key_max,
                          char* value, int val_max) {
    const char* eq = strchr(line, '=');
    if (!eq) return -1;

    int klen = (int)(eq - line);
    if (klen >= key_max) klen = key_max - 1;
    memcpy(key, line, klen);
    key[klen] = '\0';

    const char* vstart = eq + 1;
    int vlen = (int)strlen(vstart);
    // trim trailing \r or \n
    while (vlen > 0 && (vstart[vlen - 1] == '\n' || vstart[vlen - 1] == '\r'))
        vlen--;
    if (vlen >= val_max) vlen = val_max - 1;
    memcpy(value, vstart, vlen);
    value[vlen] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// Helper: scan a single directory for .app bundles and register them
// ---------------------------------------------------------------------------
static int scan_directory(const char* dir_path) {
    char dir_buf[8192];
    int result = sys_fs_list_dir(dir_path, dir_buf, sizeof(dir_buf) - 1);
    if (result <= 0) return 0;

    dir_buf[result] = '\0';
    int registered = 0;

    char* entry = dir_buf;
    while (entry && *entry) {
        // Find end of this entry (newline-separated)
        char* next = strchr(entry, '\n');
        if (next) *next++ = '\0';

        // Skip dot entries
        if (entry[0] == '.' && (entry[1] == '\0' ||
                                 (entry[1] == '.' && entry[2] == '\0'))) {
            entry = next;
            continue;
        }

        int elen = (int)strlen(entry);

        // Only process .app directories
        if (elen > 4 && strcmp(entry + elen - 4, ".app") == 0) {
            char full_path[256];
            sprintf(full_path, "%s/%s", dir_path, entry);

            // Verify it's a directory
            if (!sys_fs_is_dir(full_path)) {
                entry = next;
                continue;
            }

            // Check if already registered by install path
            int already = 0;
            for (int i = 0; i < REG_MAX_APPS; i++) {
                if (registry[i].bundle_info.name[0] != '\0' &&
                    strcmp(registry[i].install_path, full_path) == 0) {
                    already = 1;
                    break;
                }
            }

            if (!already) {
                int reg_result = app_registry_register_app(full_path, "scan");
                if (reg_result >= 0) registered++;
            }
        }

        entry = next;
    }

    return registered;
}

// ---------------------------------------------------------------------------
// Helper: apply a loaded DB entry to the registry
// If the app exists (by identifier), update tracking data.
// If not, add it as a new entry.
// ---------------------------------------------------------------------------
static void merge_loaded_entry(const app_registry_entry_t* loaded) {
    if (!loaded || loaded->bundle_info.identifier[0] == '\0') return;

    // Try to find by identifier first
    int idx = app_registry_find_by_id(loaded->bundle_info.identifier);
    if (idx >= 0) {
        // App already in registry — update tracking / preference data only
        registry[idx].launch_count      = loaded->launch_count;
        registry[idx].last_launch_time  = loaded->last_launch_time;
        registry[idx].is_visible        = loaded->is_visible;
        // Preserve category from DB if it's more specific than OTHER
        if (loaded->category != APP_CAT_OTHER && loaded->category != APP_CAT_ALL) {
            registry[idx].category = loaded->category;
        }
        return;
    }

    // Not in registry — try to find by name
    idx = app_registry_find(loaded->bundle_info.name);
    if (idx >= 0) {
        registry[idx].launch_count      = loaded->launch_count;
        registry[idx].last_launch_time  = loaded->last_launch_time;
        registry[idx].is_visible        = loaded->is_visible;
        if (loaded->category != APP_CAT_OTHER && loaded->category != APP_CAT_ALL) {
            registry[idx].category = loaded->category;
        }
        return;
    }

    // Completely new entry — add to a free slot
    int slot = find_free_slot();
    if (slot >= 0) {
        memcpy(&registry[slot], loaded, sizeof(app_registry_entry_t));
    }
}

// ===========================================================================
// Public API
// ===========================================================================

// ---------------------------------------------------------------------------
// app_registry_init - called once at boot
// ---------------------------------------------------------------------------
void app_registry_init(void) {
    // Zero the entire registry
    memset(registry, 0, sizeof(registry));

    s_printf("[REG] Initializing app registry...\n");

    // Register all built-in apps (compiled into the kernel)
    app_registry_register_builtin("Files",            "com.camelos.files",           "files",           "files",           APP_CAT_UTILITIES);
    app_registry_register_builtin("Finder",           "com.camelos.finder",          "finder",          "finder",          APP_CAT_UTILITIES);
    app_registry_register_builtin("Terminal",         "com.camelos.terminal",        "terminal",        "terminal",        APP_CAT_UTILITIES);
    app_registry_register_builtin("TextEdit",         "com.camelos.textedit",        "textedit",        "textedit",        APP_CAT_PRODUCTIVITY);
    app_registry_register_builtin("Browser",          "com.camelos.browser",         "browser",         "browser",         APP_CAT_INTERNET);
    app_registry_register_builtin("Settings",         "com.camelos.settings",        "settings",        "settings",        APP_CAT_SYSTEM);
    app_registry_register_builtin("Calculator",       "com.camelos.calculator",      "calculator",      "calculator",      APP_CAT_UTILITIES);
    app_registry_register_builtin("Console",          "com.camelos.console",         "console",         "console",         APP_CAT_DEVELOPER);
    app_registry_register_builtin("Disk Utility",     "com.camelos.diskutility",     "diskutility",     "diskutility",     APP_CAT_UTILITIES);
    app_registry_register_builtin("Activity Monitor", "com.camelos.activitymonitor", "activitymonitor", "activitymonitor", APP_CAT_UTILITIES);
    app_registry_register_builtin("Image Viewer",     "com.camelos.imageviewer",     "imageviewer",     "imageviewer",     APP_CAT_MEDIA);
    app_registry_register_builtin("MacTest",          "com.camelos.mactest",         "mactest",         "mactest",         APP_CAT_DEVELOPER);

    // Scan filesystem for .app bundles
    app_registry_scan_applications();

    // Load persisted data (launch counts, timestamps, visibility)
    app_registry_load();

    {
        char count_buf[16];
        int_to_str(app_registry_get_count(), count_buf);
        char msg[80];
        sprintf(msg, "[REG] Registry ready: %s apps registered\n", count_buf);
        s_printf(msg);
    }
}

// ---------------------------------------------------------------------------
// app_registry_scan_applications - discover .app bundles on disk
// ---------------------------------------------------------------------------
int app_registry_scan_applications(void) {
    int total = 0;

    s_printf("[REG] Scanning /Applications...\n");
    total += scan_directory("/Applications");

    s_printf("[REG] Scanning /System/Applications...\n");
    total += scan_directory("/System/Applications");

    {
        char buf[16];
        int_to_str(total, buf);
        char msg[64];
        sprintf(msg, "[REG] Discovered %s new .app bundles\n", buf);
        s_printf(msg);
    }

    return total;
}

// ---------------------------------------------------------------------------
// app_registry_register_builtin - register a kernel-compiled app
// ---------------------------------------------------------------------------
int app_registry_register_builtin(const char* name, const char* identifier,
                                   const char* executable, const char* icon,
                                   app_category_t category) {
    if (!name || !identifier) return -1;

    // Check for duplicate by identifier
    int existing = app_registry_find_by_id(identifier);
    if (existing >= 0) return existing;

    int slot = find_free_slot();
    if (slot < 0) {
        char msg[80];
        sprintf(msg, "[REG] Registry full, cannot register: %s\n", name);
        s_printf(msg);
        return -1;
    }

    app_registry_entry_t* e = &registry[slot];
    memset(e, 0, sizeof(app_registry_entry_t));

    // Fill bundle info
    strncpy(e->bundle_info.name,       name,       BUNDLE_NAME_MAX - 1);
    strncpy(e->bundle_info.identifier, identifier, BUNDLE_ID_MAX   - 1);
    strncpy(e->bundle_info.executable, executable, BUNDLE_NAME_MAX - 1);
    strncpy(e->bundle_info.icon_file,  icon,       63);
    strcpy(e->bundle_info.version,     "1.0");
    strcpy(e->bundle_info.type,        APP_TYPE_BUILTIN);

    // Install path convention for built-in apps
    sprintf(e->install_path, "/builtin/%s", name);

    e->category         = category;
    e->launch_count     = 0;
    e->last_launch_time = 0;
    e->is_builtin       = 1;
    e->is_visible       = 1;
    strncpy(e->source, "builtin", sizeof(e->source) - 1);

    return slot;
}

// ---------------------------------------------------------------------------
// app_registry_register_app - register an app from a .app bundle path
// ---------------------------------------------------------------------------
int app_registry_register_app(const char* app_path, const char* source) {
    if (!app_path) return -1;

    // Check if already registered by install path
    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] != '\0' &&
            strcmp(registry[i].install_path, app_path) == 0) {
            return i;  // Already registered
        }
    }

    int slot = find_free_slot();
    if (slot < 0) {
        char msg[80];
        sprintf(msg, "[REG] Registry full, cannot register: %s\n", app_path);
        s_printf(msg);
        return -1;
    }

    // Parse the bundle's Info.plist
    AppBundleInfo info;
    memset(&info, 0, sizeof(info));
    int parse_result = app_bundle_parse_plist(app_path, &info);

    app_registry_entry_t* e = &registry[slot];
    memset(e, 0, sizeof(app_registry_entry_t));

    if (parse_result == 0 && info.name[0] != '\0') {
        // Successful plist parse
        memcpy(&e->bundle_info, &info, sizeof(AppBundleInfo));
    } else {
        // No valid plist — derive name from directory name
        const char* name_start = app_path;
        const char* p = app_path;
        while (*p) { if (*p == '/') name_start = p + 1; p++; }
        int name_len = (int)strlen(name_start);
        // Strip ".app" suffix
        if (name_len > 4 && strcmp(name_start + name_len - 4, ".app") == 0) {
            name_len -= 4;
        }
        if (name_len > 0) {
            strncpy(e->bundle_info.name, name_start, name_len);
            e->bundle_info.name[name_len < BUNDLE_NAME_MAX ? name_len : BUNDLE_NAME_MAX - 1] = '\0';
        }
        strcpy(e->bundle_info.type, APP_TYPE_ELF);
    }

    // Check for duplicate by identifier (if we have one)
    if (e->bundle_info.identifier[0] != '\0') {
        int dup = app_registry_find_by_id(e->bundle_info.identifier);
        if (dup >= 0 && dup != slot) {
            // Another entry already has this identifier — don't add duplicate
            memset(e, 0, sizeof(app_registry_entry_t));
            return dup;
        }
    }

    // Fill remaining fields
    strncpy(e->install_path, app_path, sizeof(e->install_path) - 1);
    e->category         = APP_CAT_OTHER;
    e->launch_count     = 0;
    e->last_launch_time = 0;
    e->is_builtin       = 0;
    e->is_visible       = 1;
    if (source) {
        strncpy(e->source, source, sizeof(e->source) - 1);
    } else {
        strncpy(e->source, "manual", sizeof(e->source) - 1);
    }

    {
        char msg[256];
        sprintf(msg, "[REG] Registered app: %s (%s)\n",
                e->bundle_info.name, app_path);
        s_printf(msg);
    }

    return slot;
}

// ---------------------------------------------------------------------------
// app_registry_unregister - remove an app by name or identifier
// ---------------------------------------------------------------------------
int app_registry_unregister(const char* name_or_id) {
    if (!name_or_id) return -1;

    // Try by identifier first
    int idx = app_registry_find_by_id(name_or_id);
    // Then by name
    if (idx < 0) idx = app_registry_find(name_or_id);

    if (idx < 0) return -1;

    {
        char msg[80];
        sprintf(msg, "[REG] Unregistered: %s\n", registry[idx].bundle_info.name);
        s_printf(msg);
    }

    memset(&registry[idx], 0, sizeof(app_registry_entry_t));
    return 0;
}

// ---------------------------------------------------------------------------
// app_registry_find - find by name (exact, then case-insensitive partial)
// ---------------------------------------------------------------------------
int app_registry_find(const char* name) {
    if (!name) return -1;

    // Pass 1: exact name match
    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] != '\0' &&
            strcmp(registry[i].bundle_info.name, name) == 0) {
            return i;
        }
    }

    // Pass 2: case-insensitive partial match on name
    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] != '\0' &&
            str_contains_ci(registry[i].bundle_info.name, name)) {
            return i;
        }
    }

    return -1;
}

// ---------------------------------------------------------------------------
// app_registry_find_by_id - find by bundle identifier (exact match)
// ---------------------------------------------------------------------------
int app_registry_find_by_id(const char* identifier) {
    if (!identifier) return -1;

    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] != '\0' &&
            registry[i].bundle_info.identifier[0] != '\0' &&
            strcmp(registry[i].bundle_info.identifier, identifier) == 0) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// app_registry_get - get entry by index
// ---------------------------------------------------------------------------
const app_registry_entry_t* app_registry_get(int index) {
    if (index < 0 || index >= REG_MAX_APPS) return 0;
    if (registry[index].bundle_info.name[0] == '\0') return 0;
    return &registry[index];
}

// ---------------------------------------------------------------------------
// app_registry_get_by_name - get entry by name
// ---------------------------------------------------------------------------
const app_registry_entry_t* app_registry_get_by_name(const char* name) {
    int idx = app_registry_find(name);
    if (idx < 0) return 0;
    return &registry[idx];
}

// ---------------------------------------------------------------------------
// app_registry_list_by_category - fill output array with apps in a category
// ---------------------------------------------------------------------------
int app_registry_list_by_category(app_category_t category,
                                   app_registry_entry_t* out, int max) {
    if (!out || max <= 0) return 0;
    int count = 0;

    for (int i = 0; i < REG_MAX_APPS && count < max; i++) {
        if (registry[i].bundle_info.name[0] == '\0') continue;

        if (category == APP_CAT_ALL || registry[i].category == category) {
            memcpy(&out[count], &registry[i], sizeof(app_registry_entry_t));
            count++;
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// app_registry_search - partial case-insensitive name search
// ---------------------------------------------------------------------------
int app_registry_search(const char* query, app_registry_entry_t* out, int max) {
    if (!query || !out || max <= 0) return 0;
    int count = 0;

    for (int i = 0; i < REG_MAX_APPS && count < max; i++) {
        if (registry[i].bundle_info.name[0] == '\0') continue;

        if (str_contains_ci(registry[i].bundle_info.name, query)) {
            memcpy(&out[count], &registry[i], sizeof(app_registry_entry_t));
            count++;
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// app_registry_record_launch - increment launch count, update timestamp
// ---------------------------------------------------------------------------
void app_registry_record_launch(const char* name) {
    int idx = app_registry_find(name);
    if (idx < 0) return;

    registry[idx].launch_count++;

    // Get current time as a simple unix-ish timestamp
    int h, m, s;
    sys_get_time(&h, &m, &s);
    int year, month, day;
    sys_get_date(&year, &month, &day);

    // Encode as approximate seconds since 2000-01-01
    // Simple formula: fits in uint32_t for dates up to ~2054
    uint32_t days = (uint32_t)((year - 2000) * 365 + month * 30 + day);
    uint32_t ts = days * 86400 + (uint32_t)h * 3600 + (uint32_t)m * 60 + (uint32_t)s;
    registry[idx].last_launch_time = ts;
}

// ---------------------------------------------------------------------------
// app_registry_get_count - total registered apps
// ---------------------------------------------------------------------------
int app_registry_get_count(void) {
    int count = 0;
    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] != '\0') count++;
    }
    return count;
}

// ---------------------------------------------------------------------------
// app_registry_save - persist registry to disk as key=value text
// ---------------------------------------------------------------------------
int app_registry_save(void) {
    // Allocate a large buffer for the serialized database
    int buf_size = 65536;
    char* buf = (char*)kzalloc(buf_size);
    if (!buf) {
        s_printf("[REG] Save failed: out of memory\n");
        return -1;
    }

    int pos = 0;

    for (int i = 0; i < REG_MAX_APPS; i++) {
        if (registry[i].bundle_info.name[0] == '\0') continue;

        app_registry_entry_t* e = &registry[i];
        char tmp[32];

        // Each entry is a block of key=value lines followed by a blank line
        pos += sprintf(buf + pos, "name=%s\n",                  e->bundle_info.name);
        pos += sprintf(buf + pos, "identifier=%s\n",            e->bundle_info.identifier);
        pos += sprintf(buf + pos, "executable=%s\n",            e->bundle_info.executable);
        pos += sprintf(buf + pos, "version=%s\n",               e->bundle_info.version);
        pos += sprintf(buf + pos, "type=%s\n",                  e->bundle_info.type);
        pos += sprintf(buf + pos, "icon_file=%s\n",             e->bundle_info.icon_file);
        pos += sprintf(buf + pos, "min_os_version=%s\n",        e->bundle_info.min_os_version);
        pos += sprintf(buf + pos, "cdl_path=%s\n",              e->bundle_info.cdl_path);
        pos += sprintf(buf + pos, "install_path=%s\n",          e->install_path);

        int_to_str((int)e->category, tmp);
        pos += sprintf(buf + pos, "category=%s\n", tmp);

        int_to_str(e->launch_count, tmp);
        pos += sprintf(buf + pos, "launch_count=%s\n", tmp);

        uint_to_str(e->last_launch_time, tmp);
        pos += sprintf(buf + pos, "last_launch_time=%s\n", tmp);

        int_to_str(e->is_builtin, tmp);
        pos += sprintf(buf + pos, "is_builtin=%s\n", tmp);

        int_to_str(e->is_visible, tmp);
        pos += sprintf(buf + pos, "is_visible=%s\n", tmp);

        pos += sprintf(buf + pos, "source=%s\n", e->source);

        // Blank line separates entries
        pos += sprintf(buf + pos, "\n");

        // Guard against buffer overflow
        if (pos >= buf_size - 512) {
            s_printf("[REG] Save buffer near full, truncating\n");
            break;
        }
    }

    // Ensure the parent directory exists
    sys_fs_create("/Library", 1);
    sys_fs_create("/Library/PackageManager", 1);

    // Write the database file
    int result = sys_fs_write(REG_DB_PATH, buf, pos);

    kfree(buf);

    if (result < 0) {
        s_printf("[REG] Save failed: write error\n");
        return -1;
    }

    {
        char msg[64];
        int_to_str(app_registry_get_count(), msg);
        char full_msg[80];
        sprintf(full_msg, "[REG] Saved %s apps to disk\n", msg);
        s_printf(full_msg);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// app_registry_load - load persisted registry from disk and merge
// ---------------------------------------------------------------------------
int app_registry_load(void) {
    if (!sys_fs_exists(REG_DB_PATH)) {
        s_printf("[REG] No saved registry found, starting fresh\n");
        return -1;
    }

    int buf_size = 65536;
    char* buf = (char*)kzalloc(buf_size);
    if (!buf) {
        s_printf("[REG] Load failed: out of memory\n");
        return -1;
    }

    int size = sys_fs_read(REG_DB_PATH, buf, buf_size - 1);
    if (size <= 0) {
        kfree(buf);
        s_printf("[REG] Load failed: read error\n");
        return -1;
    }
    buf[size] = '\0';

    // Parse line by line, accumulating fields into a temporary entry
    app_registry_entry_t temp;
    memset(&temp, 0, sizeof(temp));
    int have_entry = 0;
    int merged = 0;

    char* line = buf;
    while (line && *line) {
        // Find end of line
        char* nl = strchr(line, '\n');
        if (nl) *nl++ = '\0';

        // Trim trailing \r
        int llen = (int)strlen(line);
        while (llen > 0 && line[llen - 1] == '\r') line[--llen] = '\0';

        if (line[0] == '\0') {
            // Blank line — finalize current entry
            if (have_entry) {
                merge_loaded_entry(&temp);
                merged++;
            }
            memset(&temp, 0, sizeof(temp));
            have_entry = 0;
        } else {
            // Parse key=value
            char key[64], value[256];
            if (parse_kv_line(line, key, sizeof(key), value, sizeof(value)) == 0) {
                have_entry = 1;

                if (strcmp(key, "name") == 0) {
                    strncpy(temp.bundle_info.name, value, BUNDLE_NAME_MAX - 1);
                } else if (strcmp(key, "identifier") == 0) {
                    strncpy(temp.bundle_info.identifier, value, BUNDLE_ID_MAX - 1);
                } else if (strcmp(key, "executable") == 0) {
                    strncpy(temp.bundle_info.executable, value, BUNDLE_NAME_MAX - 1);
                } else if (strcmp(key, "version") == 0) {
                    strncpy(temp.bundle_info.version, value, 15);
                } else if (strcmp(key, "type") == 0) {
                    strncpy(temp.bundle_info.type, value, 15);
                } else if (strcmp(key, "icon_file") == 0) {
                    strncpy(temp.bundle_info.icon_file, value, 63);
                } else if (strcmp(key, "min_os_version") == 0) {
                    strncpy(temp.bundle_info.min_os_version, value, 15);
                } else if (strcmp(key, "cdl_path") == 0) {
                    strncpy(temp.bundle_info.cdl_path, value, BUNDLE_PATH_MAX - 1);
                } else if (strcmp(key, "install_path") == 0) {
                    strncpy(temp.install_path, value, sizeof(temp.install_path) - 1);
                } else if (strcmp(key, "category") == 0) {
                    temp.category = (app_category_t)atoi_simple(value);
                } else if (strcmp(key, "launch_count") == 0) {
                    temp.launch_count = atoi_simple(value);
                } else if (strcmp(key, "last_launch_time") == 0) {
                    temp.last_launch_time = (uint32_t)atou_simple(value);
                } else if (strcmp(key, "is_builtin") == 0) {
                    temp.is_builtin = atoi_simple(value);
                } else if (strcmp(key, "is_visible") == 0) {
                    temp.is_visible = atoi_simple(value);
                } else if (strcmp(key, "source") == 0) {
                    strncpy(temp.source, value, sizeof(temp.source) - 1);
                }
            }
        }

        line = nl;
    }

    // Handle last entry (file may not end with blank line)
    if (have_entry) {
        merge_loaded_entry(&temp);
        merged++;
    }

    kfree(buf);

    {
        char msg_buf[16];
        int_to_str(merged, msg_buf);
        char msg[64];
        sprintf(msg, "[REG] Loaded %s entries from disk\n", msg_buf);
        s_printf(msg);
    }

    return 0;
}
