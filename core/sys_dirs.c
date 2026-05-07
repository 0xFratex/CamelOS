/**
 * sys_dirs.c - System Directory Initializer Implementation for CamelOS
 *
 * Creates and verifies the FHS-like directory layout used by CamelOS.
 * All directories are created idempotently — safe to call on every boot.
 * Parent directories are ensured before their children.
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#include "sys_dirs.h"
#include "string.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

/* ------------------------------------------------------------------ */
/*  Directory table — order matters (parents before children)          */
/* ------------------------------------------------------------------ */

static const char* sys_dir_paths[] = {
    /* /Applications */
    "/Applications",

    /* /System hierarchy */
    "/System",
    "/System/Applications",
    "/System/Library",
    "/System/Library/Frameworks",

    /* /Library hierarchy */
    "/Library",
    "/Library/PackageManager",
    "/Library/Preferences",
    "/Library/Logs",

    /* /usr hierarchy */
    "/usr",
    "/usr/bin",
    "/usr/lib",
    "/usr/local",
    "/usr/local/bin",
    "/usr/local/lib",
    "/usr/apps",

    /* /etc */
    "/etc",

    /* /var hierarchy */
    "/var",
    "/var/log",
    "/var/tmp",
    "/var/db",

    /* /tmp */
    "/tmp",

    /* Virtual filesystem mount points */
    "/dev",
    "/proc",

    /* /Users hierarchy */
    "/Users",
    "/Users/Shared",
    "/Users/root",

    NULL  /* sentinel */
};

#define SYS_DIR_COUNT (sizeof(sys_dir_paths) / sizeof(sys_dir_paths[0]) - 1)

/* ------------------------------------------------------------------ */
/*  Name-to-path mapping for sys_dirs_get_path()                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;
    const char* path;
} dir_name_entry_t;

static const dir_name_entry_t dir_name_map[] = {
    { "applications",          "/Applications" },
    { "system",                "/System" },
    { "system_applications",   "/System/Applications" },
    { "system_library",        "/System/Library" },
    { "frameworks",            "/System/Library/Frameworks" },
    { "library",               "/Library" },
    { "package_manager",       "/Library/PackageManager" },
    { "preferences",           "/Library/Preferences" },
    { "logs",                  "/Library/Logs" },
    { "usr",                   "/usr" },
    { "usr_bin",               "/usr/bin" },
    { "usr_lib",               "/usr/lib" },
    { "usr_local",             "/usr/local" },
    { "usr_local_bin",         "/usr/local/bin" },
    { "usr_local_lib",         "/usr/local/lib" },
    { "usr_apps",              "/usr/apps" },
    { "etc",                   "/etc" },
    { "var",                   "/var" },
    { "var_log",               "/var/log" },
    { "var_tmp",               "/var/tmp" },
    { "var_db",                "/var/db" },
    { "tmp",                   "/tmp" },
    { "dev",                   "/dev" },
    { "proc",                  "/proc" },
    { "users",                 "/Users" },
    { "users_shared",          "/Users/Shared" },
    { "root",                  "/Users/root" },

    { NULL, NULL }  /* sentinel */
};

/* ------------------------------------------------------------------ */
/*  Key directories checked by sys_dirs_is_initialized()               */
/* ------------------------------------------------------------------ */

static const char* key_dirs[] = {
    "/Applications",
    "/System",
    "/usr",
    "/etc",
    "/tmp",
    NULL
};

/* ================================================================== */
/*  Helper: ensure a single directory, creating parents as needed      */
/* ================================================================== */

int sys_dirs_ensure(const char* path)
{
    char buf[256];
    int len;

    if (path == NULL) {
        return -1;
    }

    len = strlen(path);
    if (len == 0 || len >= (int)sizeof(buf)) {
        return -1;
    }

    /* Already exists — nothing to do */
    if (sys_fs_exists(path)) {
        return 0;
    }

    /* Walk the path, creating each missing parent directory.
     * For "/System/Library/Frameworks" we would try:
     *   "/System"  (at the first '/' after index 0)
     *   "/System/Library"
     *   "/System/Library/Frameworks"
     */
    strcpy(buf, path);

    for (int i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';

            if (!sys_fs_exists(buf)) {
                int rc = sys_fs_create(buf, 1);
                if (rc != 0) {
                    char msg[256];
                    sprintf(msg, "[sys_dirs] Failed to create: %s\n", buf);
                    s_printf(msg);
                    return -1;
                }
            }

            buf[i] = saved;

            /* If we just processed the terminating nul, we're done */
            if (saved == '\0') {
                break;
            }
        }
    }

    return 0;
}

/* ================================================================== */
/*  sys_dirs_init — create every directory in the standard layout      */
/* ================================================================== */

void sys_dirs_init(void)
{
    int created = 0;
    int skipped = 0;
    int i;
    char msg[256];

    s_printf("[sys_dirs] Initializing directory layout...\n");

    for (i = 0; sys_dir_paths[i] != NULL; i++) {
        const char* dir = sys_dir_paths[i];

        if (sys_fs_exists(dir)) {
            skipped++;
        } else {
            int rc = sys_dirs_ensure(dir);
            if (rc != 0) {
                sprintf(msg, "[sys_dirs] ERROR: could not create %s\n", dir);
                s_printf(msg);
            } else {
                created++;
                sprintf(msg, "[sys_dirs] Created %s\n", dir);
                s_printf(msg);
            }
        }
    }

    sprintf(msg, "[sys_dirs] Done: %d created, %d already existed\n",
            created, skipped);
    s_printf(msg);
}

/* ================================================================== */
/*  sys_dirs_get_path — look up a known directory by logical name      */
/* ================================================================== */

const char* sys_dirs_get_path(const char* name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; dir_name_map[i].name != NULL; i++) {
        if (strcmp(dir_name_map[i].name, name) == 0) {
            return dir_name_map[i].path;
        }
    }

    return NULL;
}

/* ================================================================== */
/*  sys_dirs_is_initialized — verify key directories exist             */
/* ================================================================== */

int sys_dirs_is_initialized(void)
{
    int i;

    for (i = 0; key_dirs[i] != NULL; i++) {
        if (!sys_fs_exists(key_dirs[i])) {
            return 0;
        }
    }

    return 1;
}
