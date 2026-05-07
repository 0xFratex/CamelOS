/**
 * app_bootstrap.c - Bootstrap built-in apps into the filesystem
 *
 * On first boot, creates .app bundle directories under /Applications
 * for every built-in app compiled into the kernel. Each bundle gets
 * a proper Info.plist so that the app registry, dock, and spotlight
 * can discover and display them.
 *
 * Also creates essential system configuration files at boot:
 *   /etc/hostname, /etc/hosts, /etc/resolv.conf, /etc/motd
 *   /Library/Preferences/system.pref
 *   /Users/root/.profile
 *   /var/log/boot.log
 *
 * All operations are idempotent — safe to call on every boot.
 */

#include "app_bootstrap.h"
#include "string.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

/* ------------------------------------------------------------------ */
/*  Built-in app descriptor                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;           /* Display name, e.g. "Files"           */
    const char* identifier;     /* Bundle ID, e.g. "com.camelos.files"  */
    const char* executable;     /* Binary name (matches kernel dispatch)*/
    const char* icon;           /* Icon asset name for dock/switcher    */
    const char* category;       /* UTI category string                  */
    const char* version;        /* Version string                       */
} builtin_app_desc_t;

static const builtin_app_desc_t builtin_apps[] = {
    { "Files",            "com.camelos.files",           "files",           "files",           "public.app-category.utilities",     "1.0" },
    { "Terminal",         "com.camelos.terminal",        "terminal",        "terminal",        "public.app-category.utilities",     "1.0" },
    { "TextEdit",         "com.camelos.textedit",        "textedit",        "textedit",        "public.app-category.productivity",  "1.0" },
    { "Browser",          "com.camelos.browser",         "browser",         "browser",         "public.app-category.internet",      "1.0" },
    { "Settings",         "com.camelos.settings",        "settings",        "settings",        "public.app-category.system",        "1.0" },
    { "Calculator",       "com.camelos.calculator",      "calculator",      "calculator",      "public.app-category.utilities",     "1.0" },
    { "Console",          "com.camelos.console",         "console",         "console",         "public.app-category.developer",     "1.0" },
    { "Disk Utility",     "com.camelos.diskutility",     "diskutility",     "diskutility",     "public.app-category.utilities",     "1.0" },
    { "Process Monitor",  "com.camelos.processmonitor",  "processmonitor",  "activitymonitor", "public.app-category.utilities",     "1.0" },
    { "Image Viewer",     "com.camelos.imageviewer",     "imageviewer",     "imageviewer",     "public.app-category.media",         "1.0" },
    { NULL, NULL, NULL, NULL, NULL, NULL } /* sentinel */
};

/* ------------------------------------------------------------------ */
/*  Helper: write a small text file if it doesn't already exist        */
/* ------------------------------------------------------------------ */

static void write_file_if_missing(const char* path, const char* content)
{
    if (sys_fs_exists(path)) return;

    int len = (int)strlen(content);
    int rc = sys_fs_write(path, (char*)content, len);
    if (rc >= 0) {
        s_printf("[bootstrap] Created "); s_printf(path); s_printf("\n");
    } else {
        s_printf("[bootstrap] ERROR writing "); s_printf(path); s_printf("\n");
    }
}

/* ------------------------------------------------------------------ */
/*  Helper: ensure a directory exists                                  */
/* ------------------------------------------------------------------ */

static void ensure_dir(const char* path)
{
    if (sys_fs_exists(path)) return;
    /* Parent must already exist — sys_dirs_init() creates /Applications */
    int rc = sys_fs_create(path, 1);
    if (rc == 0) {
        s_printf("[bootstrap] Created directory "); s_printf(path); s_printf("\n");
    } else {
        s_printf("[bootstrap] ERROR creating directory "); s_printf(path); s_printf("\n");
    }
}

/* ------------------------------------------------------------------ */
/*  Helper: build an Info.plist for a given app                        */
/* ------------------------------------------------------------------ */

static void write_info_plist(const builtin_app_desc_t* app, const char* bundle_path)
{
    /* Build the plist content in a static buffer.
     * A typical Info.plist is ~400 bytes; we allow 1024. */
    char plist[1024];
    int pos = 0;

    #define APPEND(...) do { \
        pos += sprintf(plist + pos, __VA_ARGS__); \
        if (pos >= (int)sizeof(plist) - 1) { pos = (int)sizeof(plist) - 2; break; } \
    } while(0)

    APPEND("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    APPEND("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
    APPEND("<plist version=\"1.0\">\n<dict>\n");
    APPEND("  <key>CFBundleName</key>\n  <string>%s</string>\n", app->name);
    APPEND("  <key>CFBundleIdentifier</key>\n  <string>%s</string>\n", app->identifier);
    APPEND("  <key>CFBundleExecutable</key>\n  <string>%s</string>\n", app->executable);
    APPEND("  <key>CFBundleIconFile</key>\n  <string>%s</string>\n", app->icon);
    APPEND("  <key>CFBundleVersion</key>\n  <string>%s</string>\n", app->version);
    APPEND("  <key>CFBundlePackageType</key>\n  <string>APPL</string>\n");
    APPEND("  <key>LSApplicationCategoryType</key>\n  <string>%s</string>\n", app->category);
    APPEND("  <key>CFBundleInfoDictionaryVersion</key>\n  <string>6.0</string>\n");
    APPEND("  <key>CamelBuiltin</key>\n  <true/>\n");
    APPEND("</dict>\n</plist>\n");

    #undef APPEND

    plist[pos] = '\0';

    char plist_path[256];
    sprintf(plist_path, "%s/Info.plist", bundle_path);

    write_file_if_missing(plist_path, plist);
}

/* ------------------------------------------------------------------ */
/*  Bootstrap each built-in app                                        */
/* ------------------------------------------------------------------ */

static void bootstrap_builtin_apps(void)
{
    s_printf("[bootstrap] Creating .app bundles for built-in apps...\n");

    int created = 0;
    int existing = 0;

    for (int i = 0; builtin_apps[i].name != NULL; i++) {
        const builtin_app_desc_t* app = &builtin_apps[i];

        /* Build the bundle path: /Applications/AppName.app */
        char bundle_path[256];
        sprintf(bundle_path, "/Applications/%s.app", app->name);

        if (sys_fs_exists(bundle_path)) {
            existing++;
            continue;
        }

        /* Create the bundle directory structure */
        ensure_dir(bundle_path);

        /* Create Contents subdirectory (macOS convention) */
        char contents_path[256];
        sprintf(contents_path, "%s/Contents", bundle_path);
        ensure_dir(contents_path);

        /* Create MacOS subdirectory (binary location) */
        char macos_path[256];
        sprintf(macos_path, "%s/Contents/MacOS", bundle_path);
        ensure_dir(macos_path);

        /* Create Resources subdirectory (icons, etc.) */
        char resources_path[256];
        sprintf(resources_path, "%s/Contents/Resources", bundle_path);
        ensure_dir(resources_path);

        /* Write Info.plist into the bundle */
        write_info_plist(app, bundle_path);

        created++;
    }

    {
        char buf[16];
        int_to_str(created, buf);
        s_printf("[bootstrap] Apps: ");
        s_printf(buf);
        s_printf(" created, ");
        int_to_str(existing, buf);
        s_printf(buf);
        s_printf(" already existed\n");
    }
}

/* ------------------------------------------------------------------ */
/*  Create essential system files                                      */
/* ------------------------------------------------------------------ */

static void bootstrap_system_files(void)
{
    s_printf("[bootstrap] Creating system configuration files...\n");

    /* /etc/hostname */
    write_file_if_missing("/etc/hostname", "camelos\n");

    /* /etc/hosts */
    write_file_if_missing("/etc/hosts",
        "127.0.0.1   localhost camelos\n"
        "::1         localhost\n"
        "10.0.2.2    gateway\n"
        "10.0.2.3    dns\n"
    );

    /* /etc/resolv.conf */
    write_file_if_missing("/etc/resolv.conf",
        "nameserver 10.0.2.3\n"
        "nameserver 8.8.8.8\n"
        "domain camelos.local\n"
    );

    /* /etc/motd - Welcome message */
    write_file_if_missing("/etc/motd",
        "Welcome to CamelOS!\n"
        "\n"
        "Type 'help' for available commands.\n"
        "\n"
    );

    /* /Library/Preferences/system.pref */
    write_file_if_missing("/Library/Preferences/system.pref",
        "hostname=camelos\n"
        "theme=light\n"
        "volume=80\n"
        "network=dhcp\n"
        "screensaver=off\n"
    );

    /* /Users/root/.profile */
    write_file_if_missing("/Users/root/.profile",
        "# CamelOS root profile\n"
        "export HOME=/Users/root\n"
        "export PATH=/usr/bin:/usr/local/bin\n"
        "export SHELL=/bin/sh\n"
        "export HOSTNAME=camelos\n"
        "PS1='camelos# '\n"
    );

    /* /var/log/boot.log placeholder */
    {
        /* Generate a timestamp for the boot log */
        int h, m, s;
        sys_get_time(&h, &m, &s);
        int year, month, day;
        sys_get_date(&year, &month, &day);

        char boot_log[256];
        sprintf(boot_log,
            "[BOOT] CamelOS started %04d-%02d-%02d %02d:%02d:%02d\n"
            "[BOOT] System initialized successfully.\n",
            year, month, day, h, m, s
        );
        write_file_if_missing("/var/log/boot.log", boot_log);
    }
}

/* ------------------------------------------------------------------ */
/*  int_to_str helper (local, avoids dependency on kernel global)      */
/* ------------------------------------------------------------------ */

static void int_to_str_local(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int i = 0;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* We need int_to_str for s_printf in bootstrap_builtin_apps.
 * It's declared externally in the kernel, so we can use it. */
extern void int_to_str(int, char*);

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void app_bootstrap_init(void)
{
    s_printf("[bootstrap] Initializing app bootstrap...\n");

    /* Step 1: Create .app bundles for built-in apps */
    bootstrap_builtin_apps();

    /* Step 2: Create system configuration files */
    bootstrap_system_files();

    s_printf("[bootstrap] Bootstrap complete.\n");
}
