// usr/welcome_setup.c - Camel OS Welcome Setup Implementation
// Enhanced with encrypted password, macOS-like directory structure, and screenlock integration

#include "welcome_setup.h"
#include "screenlock.h"
#include "lib/camel_framework.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "../core/sha256.h"
#include "../common/time.h"
#include "../sys/api.h"
#include "../fs/pfs32.h"
#include "../fs/disk.h"
#include "../hal/drivers/serial.h"
#include "../hal/drivers/keyboard.h"
#include "../core/theme.h"

// External API
extern kernel_api_t* sys;
extern int screen_w;
extern int screen_h;
extern int mouse_scroll_delta;

// Global setup state
static WelcomeSetup g_setup;

// Design constants - Warm macOS X style
#define C_BG_TOP          0xFFF5F5F7
#define C_BG_BOTTOM       0xFFE8E8ED
#define C_ACCENT          0xFF007AFF
#define C_ACCENT_HOVER    0xFF0051D5
#define C_TEXT_DARK       0xFF1C1C1E
#define C_TEXT_MUTED      0xFF8E8E93
#define C_TEXT_LIGHT      0xFFFFFFFF
#define C_CARD_BG         0xFFFFFFFF
#define C_INPUT_BG        0xFFF2F2F7
#define C_BORDER          0xFFC6C6C8
#define C_SUCCESS         0xFF34C759
#define C_ERROR           0xFFFF3B30
#define C_LOCK_ICON       0xFF8E8E93

// Timezone data (comprehensive worldwide coverage)
static TimeZone timezones[] = {
    // UTC offsets (standard reference)
    {"UTC-12",   "UTC-12:00 (Baker Island)", -720},
    {"UTC-11",   "UTC-11:00 (Samoa)", -660},
    {"UTC-10",   "UTC-10:00 (Hawaii)", -600},
    {"UTC-9",    "UTC-09:00 (Alaska)", -540},
    {"UTC-8",    "UTC-08:00 (Pacific)", -480},
    {"UTC-7",    "UTC-07:00 (Mountain)", -420},
    {"UTC-6",    "UTC-06:00 (Central)", -360},
    {"UTC-5",    "UTC-05:00 (Eastern)", -300},
    {"UTC-4",    "UTC-04:00 (Atlantic)", -240},
    {"UTC-3",    "UTC-03:00 (Brasilia)", -180},
    {"UTC-2",    "UTC-02:00 (Mid-Atlantic)", -120},
    {"UTC-1",    "UTC-01:00 (Azores)", -60},
    {"UTC",      "UTC (Coordinated Universal Time)", 0},
    {"UTC+1",    "UTC+01:00 (Central Europe)", 60},
    {"UTC+2",    "UTC+02:00 (Eastern Europe)", 120},
    {"UTC+3",    "UTC+03:00 (Moscow)", 180},
    {"UTC+3:30", "UTC+03:30 (Tehran)", 210},
    {"UTC+4",    "UTC+04:00 (Gulf)", 240},
    {"UTC+4:30", "UTC+04:30 (Kabul)", 270},
    {"UTC+5",    "UTC+05:00 (Pakistan)", 300},
    {"UTC+5:30", "UTC+05:30 (India)", 330},
    {"UTC+5:45", "UTC+05:45 (Nepal)", 345},
    {"UTC+6",    "UTC+06:00 (Bangladesh)", 360},
    {"UTC+6:30", "UTC+06:30 (Myanmar)", 390},
    {"UTC+7",    "UTC+07:00 (Indochina)", 420},
    {"UTC+8",    "UTC+08:00 (China)", 480},
    {"UTC+9",    "UTC+09:00 (Japan/Korea)", 540},
    {"UTC+9:30", "UTC+09:30 (Central Australia)", 570},
    {"UTC+10",   "UTC+10:00 (Eastern Australia)", 600},
    {"UTC+11",   "UTC+11:00 (Solomon Islands)", 660},
    {"UTC+12",   "UTC+12:00 (New Zealand)", 720},
    {"UTC+13",   "UTC+13:00 (Tonga)", 780},
    {"UTC+14",   "UTC+14:00 (Line Islands)", 840},
    // Americas - detailed cities
    {"NST",      "Newfoundland (NST)", -210},
    {"AST",      "Halifax (AST)", -240},
    {"EST",      "New York (EST)", -300},
    {"CST",      "Chicago (CST)", -360},
    {"MST",      "Denver (MST)", -420},
    {"PST",      "Los Angeles (PST)", -480},
    {"AKST",     "Anchorage (AKST)", -540},
    {"HST",      "Honolulu (HST)", -600},
    {"BRT",      "Sao Paulo (BRT)", -180},
    {"ART",      "Buenos Aires (ART)", -180},
    {"COT",      "Bogota (COT)", -300},
    {"PET",      "Lima (PET)", -300},
    {"CST_MX",   "Mexico City (CST)", -360},
    {"VET",      "Caracas (VET)", -270},
    {"CLT",      "Santiago (CLT)", -240},
    {"UYT",      "Montevideo (UYT)", -180},
    {"GYT",      "Georgetown (GYT)", -240},
    {"SRT",      "Paramaribo (SRT)", -180},
    // Europe - detailed cities
    {"GMT",      "GMT (Greenwich Mean Time)", 0},
    {"WET",      "Lisbon (WET)", 0},
    {"IST_EURO", "Dublin (IST)", 60},
    {"CET",      "Paris (CET)", 60},
    {"CET_ROM",  "Rome (CET)", 60},
    {"CET_MAD",  "Madrid (CET)", 60},
    {"CET_AMS",  "Amsterdam (CET)", 60},
    {"CET_ZUR",  "Zurich (CET)", 60},
    {"CET_WAR",  "Warsaw (CET)", 60},
    {"CET_STO",  "Stockholm (CET)", 60},
    {"CET_VIE",  "Vienna (CET)", 60},
    {"CET_BER",  "Berlin (CET)", 60},
    {"CET_BEL",  "Belgrade (CET)", 60},
    {"CET_PRA",  "Prague (CET)", 60},
    {"CET_BUD",  "Budapest (CET)", 60},
    {"EET",      "Athens (EET)", 120},
    {"EET_BUC",  "Bucharest (EET)", 120},
    {"EET_HEL",  "Helsinki (EET)", 120},
    {"EET_SOF",  "Sofia (EET)", 120},
    {"EET_TAL",  "Tallinn (EET)", 120},
    {"EET_RIG",  "Riga (EET)", 120},
    {"EET_VIL",  "Vilnius (EET)", 120},
    {"EET_KIE",  "Kyiv (EET)", 120},
    {"MSK",      "Moscow (MSK)", 180},
    {"SAMT",     "Samara (SAMT)", 240},
    // Asia - detailed cities
    {"TRT",      "Istanbul (TRT)", 180},
    {"GST",      "Dubai (GST)", 240},
    {"AZT",      "Baku (AZT)", 240},
    {"GET",      "Tbilisi (GET)", 240},
    {"IST",      "Mumbai (IST)", 330},
    {"NPT",      "Kathmandu (NPT)", 345},
    {"BST",      "Dhaka (BST)", 360},
    {"ICT",      "Bangkok (ICT)", 420},
    {"WIB",      "Jakarta (WIB)", 420},
    {"SGT",      "Singapore (SGT)", 480},
    {"CST_ASIA", "Beijing/Shanghai (CST)", 480},
    {"HKT",      "Hong Kong (HKT)", 480},
    {"PHT",      "Manila (PHT)", 480},
    {"MYT",      "Kuala Lumpur (MYT)", 480},
    {"TWT",      "Taipei (TWT)", 480},
    {"KST",      "Seoul (KST)", 540},
    {"JST",      "Tokyo (JST)", 540},
    {"IRKT",     "Irkutsk (IRKT)", 480},
    {"YAKT",     "Yakutsk (YAKT)", 540},
    {"VLAT",     "Vladivostok (VLAT)", 600},
    // Oceania
    {"AWST",     "Perth (AWST)", 480},
    {"ACST",     "Darwin (ACST)", 570},
    {"AEST",     "Sydney (AEST)", 600},
    {"NZST",     "Auckland (NZST)", 720},
    {"CHAST",    "Chatham Islands (CHAST)", 765},
    // Africa
    {"WAT",      "Lagos (WAT)", 60},
    {"CAT",      "Johannesburg (CAT)", 120},
    {"EAT",      "Nairobi (EAT)", 180},
    {"MUT",      "Mauritius (MUT)", 240}
};
#define TIMEZONE_COUNT (sizeof(timezones) / sizeof(TimeZone))

// Theme definitions - simplified to Dark/Light mode only
typedef struct {
    const char* name;
    uint32_t primary;
    uint32_t secondary;
    uint32_t accent;
} ThemeDef;

static ThemeDef themes[THEME_COUNT] = {
    {"Light", 0xFF007AFF, 0xFF5AC8FA, 0xFF007AFF},  // Light mode (macOS Aqua blue)
    {"Dark",  0xFF0A84FF, 0xFF5AC8FA, 0xFF0A84FF}   // Dark mode (macOS Dark blue)
};

// Keyboard layout display names (matching keyboard.h KBD_LAYOUT_COUNT = 32)
static const char* kbd_layout_display_names[] = {
    "US QWERTY", "UK QWERTY", "German QWERTZ", "French AZERTY",
    "Spanish QWERTY", "Italian QWERTY", "Brazilian ABNT2", "Dvorak",
    "Japanese", "Korean", "Chinese Pinyin", "Swiss",
    "Swedish", "Norwegian", "Danish", "Finnish",
    "Polish", "Czech QWERTZ", "Hungarian", "Romanian",
    "Turkish Q", "Turkish F", "Russian", "Arabic",
    "Hebrew", "Thai", "Vietnamese", "Greek",
    "Croatian", "Portuguese PT", "Canadian",
    "Brazilian ABNT1"
};
#define KBD_LAYOUT_DISPLAY_COUNT (sizeof(kbd_layout_display_names) / sizeof(char*))

// --- Initialization ---

void welcome_setup_init(void) {
    memset(&g_setup, 0, sizeof(g_setup));
    g_setup.state = SETUP_STATE_WELCOME;
    g_setup.current_step = 0;
    g_setup.total_steps = 6;  // 6 steps: Welcome, Keyboard, User, Password, Timezone, Theme
    g_setup.tz_scroll = 0;    // Timezone scroll offset
    g_setup.selected_tz_idx = 0;
    g_setup.selected_theme_idx = 0;
    g_setup.selected_kbd_idx = 0;
    g_setup.kbd_scroll = 0;
    g_setup.input_buffer[0] = 0;
    g_setup.input_cursor = 0;
    g_setup.anim_progress = 0.0f;
    g_setup.password_buffer[0] = 0;
    g_setup.password_confirm[0] = 0;
    g_setup.password_cursor = 0;
    g_setup.confirm_cursor = 0;
    g_setup.password_active = 1;
    g_setup.password_match_error = 0;
    g_setup.password_step = 0;
    
    welcome_setup_set_defaults();
    welcome_setup_load_config();
}

void welcome_setup_set_defaults(void) {
    g_setup.config.username[0] = 0;  // Empty - show placeholder "Enter name..."
    strcpy(g_setup.config.computer_name, "CamelOS");
    g_setup.config.password_hash[0] = 0;
    memcpy(&g_setup.config.timezone, &timezones[0], sizeof(TimeZone));
    g_setup.config.theme = THEME_LIGHT;
    g_setup.config.is_configured = 0;
    g_setup.config.auto_lock = 1;
    g_setup.config.lock_timeout = 10;
    g_setup.config.kbd_layout = 0;  // Default US QWERTY
    g_setup.config.config_version = 2;  // Bumped for encrypted password support
}

// --- Persistence ---

void welcome_setup_load_config(void) {
    // Try to load config from /Library/Preferences/system.conf (macOS-like path)
    // Fall back to /etc/system.conf for backward compatibility
    char buffer[1024];
    int result = sys_fs_read("/Library/Preferences/system.conf", buffer, sizeof(buffer) - 1);
    
    if (result <= 0) {
        // Try legacy path
        result = sys_fs_read("/etc/system.conf", buffer, sizeof(buffer) - 1);
    }
    
    if (result > 0) {
        buffer[result] = 0;
        
        // Parse config format
        char* line = buffer;
        while (line && *line) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            
            // Trim trailing CR if present (in case of CRLF line endings)
            int llen = strlen(line);
            while (llen > 0 && (line[llen-1] == '\r' || line[llen-1] == ' ')) {
                line[--llen] = 0;
            }
            
            // Skip comments and empty lines
            if (line[0] == '#' || line[0] == 0) { line = next; continue; }
            
            if (strncmp(line, "username=", 9) == 0) {
                strncpy(g_setup.config.username, line + 9, SETUP_USERNAME_MAX - 1);
                g_setup.config.username[SETUP_USERNAME_MAX - 1] = 0;
            } else if (strncmp(line, "password_hash=", 14) == 0) {
                strncpy(g_setup.config.password_hash, line + 14, 64);
                g_setup.config.password_hash[64] = 0;
            } else if (strncmp(line, "timezone=", 9) == 0) {
                for (int i = 0; i < (int)TIMEZONE_COUNT; i++) {
                    if (strcmp(line + 9, timezones[i].name) == 0) {
                        memcpy(&g_setup.config.timezone, &timezones[i], sizeof(TimeZone));
                        g_setup.selected_tz_idx = i;
                        break;
                    }
                }
            } else if (strncmp(line, "theme=", 6) == 0) {
                int theme_idx = line[6] - '0';
                if (theme_idx >= 0 && theme_idx < THEME_COUNT) {
                    g_setup.config.theme = theme_idx;
                    g_setup.selected_theme_idx = theme_idx;
                }
            } else if (strncmp(line, "configured=", 11) == 0) {
                g_setup.config.is_configured = (line[11] == '1');
            } else if (strncmp(line, "auto_lock=", 10) == 0) {
                g_setup.config.auto_lock = (line[10] == '1');
            } else if (strncmp(line, "lock_timeout=", 13) == 0) {
                g_setup.config.lock_timeout = 0;
                const char* p = line + 13;
                while (*p >= '0' && *p <= '9') {
                    g_setup.config.lock_timeout = g_setup.config.lock_timeout * 10 + (*p - '0');
                    p++;
                }
            } else if (strncmp(line, "kbd_layout=", 11) == 0) {
                g_setup.config.kbd_layout = 0;
                const char* p = line + 11;
                while (*p >= '0' && *p <= '9') {
                    g_setup.config.kbd_layout = g_setup.config.kbd_layout * 10 + (*p - '0');
                    p++;
                }
            }
            
            line = next;
        }
    }
}

int welcome_setup_save_config(void) {
    char buffer[1024];
    int pos = 0;
    
    pos += sprintf(buffer + pos, "# CamelOS System Configuration\n");
    pos += sprintf(buffer + pos, "# Version 2 - Encrypted password storage\n");
    pos += sprintf(buffer + pos, "username=%s\n", g_setup.config.username);
    pos += sprintf(buffer + pos, "computer=%s\n", g_setup.config.computer_name);
    pos += sprintf(buffer + pos, "password_hash=%s\n", g_setup.config.password_hash);
    pos += sprintf(buffer + pos, "timezone=%s\n", g_setup.config.timezone.name);
    pos += sprintf(buffer + pos, "theme=%d\n", g_setup.config.theme);
    pos += sprintf(buffer + pos, "auto_lock=%d\n", g_setup.config.auto_lock);
    pos += sprintf(buffer + pos, "lock_timeout=%d\n", g_setup.config.lock_timeout);
    pos += sprintf(buffer + pos, "kbd_layout=%d\n", g_setup.config.kbd_layout);
    pos += sprintf(buffer + pos, "configured=1\n");
    
    // Create macOS-like directory structure
    sys_fs_create("/Library", 1);
    sys_fs_create("/Library/Preferences", 1);
    
    // Write config file - pfs32_write_file handles creation and overwrite internally.
    // Do NOT delete+create separately as that can cause race conditions and
    // duplicate directory entries. Just write directly.
    int wres = sys_fs_write("/Library/Preferences/system.conf", buffer, pos);
    
    // Flush immediately after writing the config to ensure persistence
    pfs32_sync();
    disk_flush_cache();
    
    // Verify the write succeeded by reading back
    if (wres > 0) {
        char verify[64];
        int vres = sys_fs_read("/Library/Preferences/system.conf", verify, sizeof(verify) - 1);
        if (vres <= 0 || strncmp(verify, "# CamelOS", 9) != 0) {
            // Primary write failed verification - try deleting and rewriting
            s_printf("[SETUP] WARNING: Config verification failed, retrying...\n");
            sys_fs_delete("/Library/Preferences/system.conf");
            wres = sys_fs_write("/Library/Preferences/system.conf", buffer, pos);
            pfs32_sync();
            disk_flush_cache();
            if (wres <= 0) {
                s_printf("[SETUP] ERROR: Config rewrite also failed!\n");
            }
        }
    } else {
        s_printf("[SETUP] ERROR: Failed to write config file!\n");
    }
    
    // Also write to legacy path for backward compatibility during transition
    sys_fs_create("/etc", 1);
    sys_fs_write("/etc/system.conf", buffer, pos);
    
    // Create macOS-like user directory structure
    char home_path[128];
    
    // /Users/<name> (macOS-like, replaces /home/<name>)
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    sys_fs_create(home_path, 1);
    
    // Desktop
    strcat(home_path, "/Desktop");
    sys_fs_create(home_path, 1);
    
    // Documents
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Documents");
    sys_fs_create(home_path, 1);
    
    // Downloads
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Downloads");
    sys_fs_create(home_path, 1);
    
    // Pictures
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Pictures");
    sys_fs_create(home_path, 1);
    
    // Music
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Music");
    sys_fs_create(home_path, 1);
    
    // Movies
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Movies");
    sys_fs_create(home_path, 1);
    
    // Public
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Public");
    sys_fs_create(home_path, 1);
    
    // NOTE: Do NOT create /Users/Desktop or /home/desktop - it causes duplicate folder issues.
    // The desktop.c module reads username from config and uses /Users/<name>/Desktop.
    // Only the user-specific Desktop at /Users/<username>/Desktop should exist.
    
    // Create macOS system directories
    sys_fs_create("/Applications", 1);
    sys_fs_create("/System", 1);
    sys_fs_create("/System/Library", 1);
    sys_fs_create("/System/Library/Frameworks", 1);
    sys_fs_create("/System/Library/CoreServices", 1);
    sys_fs_create("/Library/Application Support", 1);
    sys_fs_create("/Library/Fonts", 1);
    sys_fs_create("/private", 1);
    sys_fs_create("/private/var", 1);
    sys_fs_create("/private/tmp", 1);
    sys_fs_create("/usr", 1);
    sys_fs_create("/usr/apps", 1);  // Legacy compatibility
    sys_fs_create("/usr/lib", 1);   // Legacy compatibility
    sys_fs_create("/bin", 1);
    sys_fs_create("/sbin", 1);
    sys_fs_create("/dev", 1);
    sys_fs_create("/Volumes", 1);

    // Create standard system files to make the OS feel complete
    char file_buf[512];
    int file_len;

    // /etc/hosts - Network hosts file
    file_len = sprintf(file_buf,
        "# CamelOS Hosts File\n"
        "127.0.0.1       localhost\n"
        "255.255.255.255 broadcasthost\n"
        "::1             localhost\n");
    sys_fs_create("/etc/hosts", 0);
    sys_fs_write("/etc/hosts", file_buf, file_len);

    // /etc/resolv.conf - DNS configuration
    file_len = sprintf(file_buf,
        "# CamelOS DNS Configuration\n"
        "nameserver 8.8.8.8\n"
        "nameserver 8.8.4.4\n");
    sys_fs_create("/etc/resolv.conf", 0);
    sys_fs_write("/etc/resolv.conf", file_buf, file_len);

    // /etc/hostname
    file_len = sprintf(file_buf, "camelos\n");
    sys_fs_create("/etc/hostname", 0);
    sys_fs_write("/etc/hostname", file_buf, file_len);

    // /etc/passwd - Minimal user database
    file_len = sprintf(file_buf,
        "# CamelOS User Database\n"
        "root:x:0:0:root:/root:/bin/sh\n");
    sys_fs_create("/etc/passwd", 0);
    sys_fs_write("/etc/passwd", file_buf, file_len);

    // /etc/motd - Message of the day
    file_len = sprintf(file_buf,
        "Welcome to CamelOS!\n"
        "Type 'help' for available commands.\n");
    sys_fs_create("/etc/motd", 0);
    sys_fs_write("/etc/motd", file_buf, file_len);

    // /etc/shells
    file_len = sprintf(file_buf, "/bin/sh\n/bin/csh\n");
    sys_fs_create("/etc/shells", 0);
    sys_fs_write("/etc/shells", file_buf, file_len);

    // /var/log/system.log placeholder
    sys_fs_create("/var", 1);
    sys_fs_create("/var/log", 1);
    file_len = sprintf(file_buf,
        "[%s] CamelOS system initialized\n"
        "[%s] Network stack ready\n"
        "[%s] Filesystem mounted\n",
        g_setup.config.username, g_setup.config.username, g_setup.config.username);
    sys_fs_create("/var/log/system.log", 0);
    sys_fs_write("/var/log/system.log", file_buf, file_len);

    // /tmp directory (used by browser downloads and app installer)
    sys_fs_create("/tmp", 1);

    // /home symlink equivalent - create /home directory for compatibility
    sys_fs_create("/home", 1);
    char home_compat[128];
    strcpy(home_compat, "/home/");
    strcat(home_compat, g_setup.config.username);
    sys_fs_create(home_compat, 1);

    // Readme on Desktop
    char readme_path[128];
    strcpy(readme_path, "/Users/");
    strcat(readme_path, g_setup.config.username);
    strcat(readme_path, "/Desktop/Welcome.txt");
    file_len = sprintf(file_buf,
        "Welcome to CamelOS!\n"
        "===================\n\n"
        "CamelOS is a modern operating system with a macOS-inspired interface.\n\n"
        "Getting Started:\n"
        "- Use the Dock at the bottom to launch applications\n"
        "- Click the Browser icon to browse the web\n"
        "- Use Terminal for command-line access\n"
        "- Try: curl http://example.com to download files\n"
        "- Try: open http://example.com to open URLs\n\n"
        "Tips:\n"
        "- Drag windows by their title bar\n"
        "- Resize windows from the bottom-right corner\n"
        "- Use Ctrl+Tab to switch between apps\n"
        "- Double-click desktop icons to open them\n\n"
        "Enjoy CamelOS!\n");
    sys_fs_create(readme_path, 0);
    sys_fs_write(readme_path, file_buf, file_len);

    // Sample Documents
    char doc_path[128];
    strcpy(doc_path, "/Users/");
    strcat(doc_path, g_setup.config.username);
    strcat(doc_path, "/Documents/Notes.txt");
    file_len = sprintf(file_buf,
        "My Notes\n"
        "========\n\n"
        "This is the Documents folder. Store your text files here.\n"
        "You can edit files using the TextEdit application.\n\n"
        "Quick Commands:\n"
        "- ls: list files\n"
        "- cd: change directory\n"
        "- cat: view file contents\n"
        "- curl: download files from the internet\n"
        "- ping: test network connectivity\n");
    sys_fs_create(doc_path, 0);
    sys_fs_write(doc_path, file_buf, file_len);

    // /usr/lib/README
    file_len = sprintf(file_buf,
        "CamelOS CDL Libraries\n"
        "====================\n\n"
        "This directory contains CDL (CamelOS Dynamic Library) modules.\n"
        "CDL files can be loaded at runtime using sys_load_library().\n\n"
        "Place .cdl files here to make them available system-wide.\n");
    sys_fs_create("/usr/lib/README", 0);
    sys_fs_write("/usr/lib/README", file_buf, file_len);

    // /Applications/README
    file_len = sprintf(file_buf,
        "CamelOS Applications\n"
        "===================\n\n"
        "Install applications by:\n"
        "1. Downloading .app bundles via the browser\n"
        "2. Opening .dmg files to mount and install\n"
        "3. Dragging apps to this folder\n\n"
        "Installed apps appear in the Dock automatically.\n");
    sys_fs_create("/Applications/README", 0);
    sys_fs_write("/Applications/README", file_buf, file_len);

    // /System/Library/CoreServices/SystemVersion.plist
    file_len = sprintf(file_buf,
        "CamelOS Version Info\n"
        "====================\n"
        "ProductName: CamelOS\n"
        "ProductVersion: 1.0\n"
        "BuildVersion: 2026.05\n");
    sys_fs_create("/System/Library/CoreServices/SystemVersion.plist", 0);
    sys_fs_write("/System/Library/CoreServices/SystemVersion.plist", file_buf, file_len);

    // /var/log/install.log
    file_len = sprintf(file_buf,
        "[%s] System installation completed\n"
        "[%s] Default applications registered\n"
        "[%s] Network stack initialized\n",
        g_setup.config.username, g_setup.config.username, g_setup.config.username);
    sys_fs_create("/var/log/install.log", 0);
    sys_fs_write("/var/log/install.log", file_buf, file_len);

    // /etc/fstab - Filesystem table
    file_len = sprintf(file_buf,
        "# CamelOS Filesystem Table\n"
        "# Device    Mount    Type    Options\n"
        "/dev/hda0   /        pfs32   defaults\n");
    sys_fs_create("/etc/fstab", 0);
    sys_fs_write("/etc/fstab", file_buf, file_len);

    // /etc/profile - Shell profile
    file_len = sprintf(file_buf,
        "# CamelOS Shell Profile\n"
        "export HOME=/Users/%s\n"
        "export PATH=/bin:/usr/apps\n"
        "export SHELL=/bin/sh\n",
        g_setup.config.username);
    sys_fs_create("/etc/profile", 0);
    sys_fs_write("/etc/profile", file_buf, file_len);

    // /Library/Preferences/com.apple.dock.plist equivalent
    file_len = sprintf(file_buf,
        "# Dock Configuration\n"
        "apps=Files,Terminal,TextEdit,Browser,Settings\n"
        "orientation=bottom\n"
        "autohide=0\n");
    sys_fs_create("/Library/Preferences/dock.conf", 0);
    sys_fs_write("/Library/Preferences/dock.conf", file_buf, file_len);

    // Sample file in Downloads
    char dl_path[128];
    strcpy(dl_path, "/Users/");
    strcat(dl_path, g_setup.config.username);
    strcat(dl_path, "/Downloads/README.txt");
    file_len = sprintf(file_buf,
        "Downloads Folder\n"
        "================\n\n"
        "Files downloaded from the internet appear here.\n"
        "Use the Browser to download files, or curl:\n"
        "  curl http://example.com/file.txt\n\n");
    sys_fs_create(dl_path, 0);
    sys_fs_write(dl_path, file_buf, file_len);

    // Sample file in Pictures
    char pic_path[128];
    strcpy(pic_path, "/Users/");
    strcat(pic_path, g_setup.config.username);
    strcat(pic_path, "/Pictures/README.txt");
    file_len = sprintf(file_buf,
        "Pictures Folder\n"
        "===============\n\n"
        "Store your images and screenshots here.\n");
    sys_fs_create(pic_path, 0);
    sys_fs_write(pic_path, file_buf, file_len);

    // Sample file in Music
    char mus_path[128];
    strcpy(mus_path, "/Users/");
    strcat(mus_path, g_setup.config.username);
    strcat(mus_path, "/Music/README.txt");
    file_len = sprintf(file_buf,
        "Music Folder\n"
        "============\n\n"
        "Store your audio files here.\n");
    sys_fs_create(mus_path, 0);
    sys_fs_write(mus_path, file_buf, file_len);

    // Sample file in Movies
    char mov_path[128];
    strcpy(mov_path, "/Users/");
    strcat(mov_path, g_setup.config.username);
    strcat(mov_path, "/Movies/README.txt");
    file_len = sprintf(file_buf,
        "Movies Folder\n"
        "=============\n\n"
        "Store your video files here.\n");
    sys_fs_create(mov_path, 0);
    sys_fs_write(mov_path, file_buf, file_len);

    // /bin directory content
    file_len = sprintf(file_buf,
        "CamelOS Binaries\n"
        "================\n"
        "This directory contains system executables.\n"
        "Available commands: ls, cd, pwd, cat, echo, curl, open, ping\n");
    sys_fs_create("/bin/README", 0);
    sys_fs_write("/bin/README", file_buf, file_len);

    // /sbin directory content
    file_len = sprintf(file_buf,
        "CamelOS System Binaries\n"
        "=======================\n"
        "System administration tools.\n");
    sys_fs_create("/sbin/README", 0);
    sys_fs_write("/sbin/README", file_buf, file_len);

    // CRITICAL: Flush all changes to disk so config persists across reboots
    // Double-flush to ensure data reaches persistent storage
    pfs32_sync();
    disk_flush_cache();
    pfs32_sync();
    disk_flush_cache();
    
    return (wres > 0) ? 0 : -1;
}

// --- State Management ---

int welcome_setup_needs_setup(void) {
    return !g_setup.config.is_configured;
}

void welcome_setup_start(void) {
    if (!g_setup.config.is_configured) {
        g_setup.state = SETUP_STATE_WELCOME;
        g_setup.current_step = 0;
        g_setup.anim_progress = 0.0f;
    }
}

int welcome_setup_is_active(void) {
    return g_setup.state != SETUP_STATE_COMPLETE || !g_setup.config.is_configured;
}

int welcome_setup_finish(void) {
    // Hash the password if one was set
    if (g_setup.password_buffer[0] != 0) {
        sha256_hash_password(g_setup.password_buffer, g_setup.config.password_hash);
    } else {
        // No password set - store empty hash
        g_setup.config.password_hash[0] = 0;
    }
    
    // Only mark as configured if the save succeeds
    g_setup.config.is_configured = 1;
    int save_result = welcome_setup_save_config();
    if (save_result < 0) {
        s_printf("[SETUP] ERROR: Config save failed! Not marking as configured.\n");
        g_setup.config.is_configured = 0;
        return -1;  // Don't proceed - user must retry
    }
    
    // Configure screenlock with user and hashed password
    screenlock_create_user(g_setup.config.username, g_setup.config.password_hash, g_setup.config.theme);
    
    // Apply the selected theme (Light/Dark) to the system theme engine
    extern void theme_set(int theme_id);
    if (g_setup.config.theme == THEME_DARK) {
        theme_set(1);  // THEME_DARK = 1 in core/theme.h
    } else {
        theme_set(0);  // THEME_LIGHT = 0 in core/theme.h
    }
    
    // Configure screenlock timeout
    screenlock_set_inactivity_timeout(g_setup.config.lock_timeout);
    screenlock_set_inactivity_enabled(g_setup.config.auto_lock);
    
    // Apply keyboard layout from selected index
    extern void kbd_set_layout(int);
    g_setup.config.kbd_layout = g_setup.selected_kbd_idx;
    kbd_set_layout(g_setup.config.kbd_layout);
    
    // Apply timezone offset to system clock
    extern void sys_set_tz_offset(int);
    if (g_setup.selected_tz_idx >= 0 && g_setup.selected_tz_idx < (int)TIMEZONE_COUNT) {
        sys_set_tz_offset(timezones[g_setup.selected_tz_idx].offset_minutes);
    }
    
    g_setup.state = SETUP_STATE_COMPLETE;
    return 0;
}

SystemConfig* welcome_setup_get_config(void) {
    return &g_setup.config;
}

// --- Rendering Helpers ---

static void draw_gradient_bg(int w, int h) {
    for (int y = 0; y < h; y++) {
        uint8_t blend = (y * 255) / h;
        uint8_t r1 = (C_BG_TOP >> 16) & 0xFF;
        uint8_t g1 = (C_BG_TOP >> 8) & 0xFF;
        uint8_t b1 = C_BG_TOP & 0xFF;
        uint8_t r2 = (C_BG_BOTTOM >> 16) & 0xFF;
        uint8_t g2 = (C_BG_BOTTOM >> 8) & 0xFF;
        uint8_t b2 = C_BG_BOTTOM & 0xFF;
        
        uint8_t r = r1 + ((r2 - r1) * blend) / 255;
        uint8_t g = g1 + ((g2 - g1) * blend) / 255;
        uint8_t b = b1 + ((b2 - b1) * blend) / 255;
        
        gfx_fill_rect(0, y, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }
}

static void draw_progress_dots(int cx, int y, int current, int total) {
    int dot_size = 8;
    int spacing = 16;
    int total_w = total * spacing;
    int start_x = cx - total_w / 2;
    
    for (int i = 0; i < total; i++) {
        int dot_x = start_x + i * spacing + spacing / 2;
        uint32_t color = (i == current) ? C_ACCENT : C_BORDER;
        gfx_fill_rounded_rect(dot_x - dot_size/2, y - dot_size/2, 
                              dot_size, dot_size, color, dot_size/2);
    }
}

static void draw_card(int x, int y, int w, int h) {
    // Shadow
    gfx_fill_rounded_rect(x + 4, y + 6, w, h, 0x20000000, 16);
    // Background
    gfx_fill_rounded_rect(x, y, w, h, C_CARD_BG, 16);
    // Border
    gfx_draw_rect(x, y, w, h, C_BORDER);
}

static int draw_button(int x, int y, int w, int h, const char* label, int primary, int mx, int my, int click) {
    int hover = (mx >= x && mx <= x + w && my >= y && my <= y + h);
    
    uint32_t bg = primary ? (hover ? C_ACCENT_HOVER : C_ACCENT) : 
                            (hover ? C_INPUT_BG : C_CARD_BG);
    uint32_t text = primary ? C_TEXT_LIGHT : C_TEXT_DARK;
    
    // Shadow for primary
    if (primary) {
        gfx_fill_rounded_rect(x + 2, y + 3, w, h, 0x20000000, 10);
    }
    
    gfx_fill_rounded_rect(x, y, w, h, bg, 10);
    if (!primary) {
        gfx_draw_rect(x, y, w, h, C_BORDER);
    }
    
    int text_x = x + (w - strlen(label) * 8) / 2;
    int text_y = y + (h - 16) / 2;
    gfx_draw_string(text_x, text_y, label, text);
    
    return hover && click;
}

static void draw_text_field(int x, int y, int w, int h, const char* value, int active, int is_password, int mx, int my, int click, int cursor_pos) {
    int hover = (mx >= x && mx <= x + w && my >= y && my <= y + h);
    
    uint32_t bg = active ? C_TEXT_LIGHT : (hover ? C_INPUT_BG : C_CARD_BG);
    uint32_t border = active ? C_ACCENT : (hover ? C_TEXT_MUTED : C_BORDER);
    
    gfx_fill_rounded_rect(x, y, w, h, bg, 8);
    gfx_draw_rect(x, y, w, h, border);
    
    // Text is left-aligned with padding for cursor accuracy
    int pad = 16;  // Left padding
    int text_y = y + (h - 16) / 2;
    int text_x = x + pad;
    
    if (is_password) {
        int len = cursor_pos;
        // Use 10px spacing for password dots (more compact, matches count accurately)
        int dot_spacing = 10;
        int dot_size = 6;
        
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                gfx_fill_rounded_rect(text_x + i * dot_spacing + 2, text_y + 5, dot_size, dot_size, C_TEXT_DARK, 3);
            }
        } else {
            // Placeholder
            const char* ph = "Enter password...";
            gfx_draw_string(text_x, text_y, ph, C_TEXT_MUTED);
        }
    } else {
        if (value && value[0]) {
            gfx_draw_string(text_x, text_y, value, C_TEXT_DARK);
        } else {
            // Placeholder
            const char* ph = "Enter name...";
            gfx_draw_string(text_x, text_y, ph, C_TEXT_MUTED);
        }
    }
    
    // Cursor - positioned at cursor_pos (character index)
    if (active) {
        static int blink = 0;
        blink++;
        if ((blink / 30) % 2 == 0) {
            int cursor_x_pos;
            if (is_password) {
                int dot_spacing = 10;
                cursor_x_pos = text_x + cursor_pos * dot_spacing + 2;
            } else {
                // Use cursor_pos (character index) instead of strlen(value)
                // to correctly position cursor within the text
                cursor_x_pos = text_x + cursor_pos * 8;
            }
            gfx_fill_rect(cursor_x_pos, text_y, 2, 16, C_ACCENT);
        }
    }
}

// --- State Renderers ---

static void render_welcome(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Logo / Title - shifted up to make room for features and button
    int title_y = cy - 140;
    gfx_draw_string_centered(cx, title_y, "Camel", C_ACCENT, 4);
    gfx_draw_string_centered(cx, title_y + 60, "OS", C_TEXT_DARK, 4);
    
    // Subtitle
    char* subtitle = "Welcome to your new operating system";
    gfx_draw_string(cx - strlen(subtitle) * 4, title_y + 120, subtitle, C_TEXT_MUTED);
    
    // Feature list
    int feat_y = title_y + 160;
    char* features[] = {
        "Fast and lightweight performance",
        "Beautiful macOS-inspired interface",
        "Encrypted user authentication",
        "Built-in apps and utilities",
        "Secure and private by design"
    };
    
    for (int i = 0; i < 5; i++) {
        gfx_draw_string(cx - 160, feat_y + i * 22, ">", C_ACCENT);
        gfx_draw_string(cx - 145, feat_y + i * 22, features[i], C_TEXT_DARK);
    }
    
    // Continue button - placed below feature list with proper spacing
    // Feature list ends at feat_y + 4*22 = feat_y + 88
    int btn_y = feat_y + 88 + 30;  // 30px gap after last feature
    if (draw_button(cx - 80, btn_y, 160, 48, "Continue", 1, mx, my, click)) {
        g_setup.state = SETUP_STATE_KEYBOARD;
        g_setup.current_step = 1;
        g_setup.input_buffer[0] = 0;
        g_setup.input_cursor = 0;
        g_setup.input_active = 1;
        // Don't pre-fill - show placeholder text instead
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 0, g_setup.total_steps);
}

static void render_keyboard_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 480;
    int card_h = 400;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2 - 20;

    draw_card(card_x, card_y, card_w, card_h);

    // Title
    char* title = "Select Your Keyboard Layout";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 16) / 2,
                          card_y + 24, title, C_TEXT_DARK, 2);

    // Subtitle
    char* subtitle = "Choose the layout that matches your keyboard";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 8) / 2,
                   card_y + 60, subtitle, C_TEXT_MUTED);

    // Keyboard layout list (scrollable region)
    int list_x = card_x + 30;
    int list_y = card_y + 100;
    int list_w = card_w - 60;
    int item_h = 32;
    int visible_items = 7;

    // Handle mouse scroll wheel
    if (mouse_scroll_delta != 0) {
        if (mx >= list_x && mx <= list_x + list_w &&
            my >= list_y && my < list_y + visible_items * item_h) {
            g_setup.kbd_scroll -= mouse_scroll_delta;
            int max_scroll = (int)KBD_LAYOUT_DISPLAY_COUNT - visible_items;
            if (max_scroll < 0) max_scroll = 0;
            if (g_setup.kbd_scroll > max_scroll) g_setup.kbd_scroll = max_scroll;
            if (g_setup.kbd_scroll < 0) g_setup.kbd_scroll = 0;
            // Adjust selected index if it's out of view
            if (g_setup.selected_kbd_idx < g_setup.kbd_scroll) {
                g_setup.selected_kbd_idx = g_setup.kbd_scroll;
            }
            if (g_setup.selected_kbd_idx >= g_setup.kbd_scroll + visible_items) {
                g_setup.selected_kbd_idx = g_setup.kbd_scroll + visible_items - 1;
            }
        }
        mouse_scroll_delta = 0;
    }

    // Clamp scroll so selected item is always visible
    if (g_setup.selected_kbd_idx < g_setup.kbd_scroll) {
        g_setup.kbd_scroll = g_setup.selected_kbd_idx;
    }
    if (g_setup.selected_kbd_idx >= g_setup.kbd_scroll + visible_items) {
        g_setup.kbd_scroll = g_setup.selected_kbd_idx - visible_items + 1;
    }
    // Clamp scroll bounds
    int max_scroll = (int)KBD_LAYOUT_DISPLAY_COUNT - visible_items;
    if (max_scroll < 0) max_scroll = 0;
    if (g_setup.kbd_scroll > max_scroll) g_setup.kbd_scroll = max_scroll;
    if (g_setup.kbd_scroll < 0) g_setup.kbd_scroll = 0;

    // List background
    gfx_fill_rounded_rect(list_x, list_y, list_w, visible_items * item_h, C_INPUT_BG, 8);

    for (int vi = 0; vi < visible_items; vi++) {
        int i = g_setup.kbd_scroll + vi;
        if (i >= (int)KBD_LAYOUT_DISPLAY_COUNT) break;

        int item_y = list_y + vi * item_h;
        int is_selected = (i == g_setup.selected_kbd_idx);
        int hover = (mx >= list_x && mx <= list_x + list_w &&
                    my >= item_y && my < item_y + item_h);

        if (is_selected || hover) {
            uint32_t bg = is_selected ? C_ACCENT : (hover ? C_BG_TOP : C_INPUT_BG);
            gfx_fill_rounded_rect(list_x + 4, item_y + 2, list_w - 8, item_h - 4, bg, 6);
        }

        uint32_t text_color = is_selected ? C_TEXT_LIGHT : C_TEXT_DARK;
        gfx_draw_string(list_x + 16, item_y + 8, kbd_layout_display_names[i], text_color);
    }

    // Scroll indicator (only when there are more items than visible)
    if ((int)KBD_LAYOUT_DISPLAY_COUNT > visible_items) {
        int total_h = visible_items * item_h;
        int scroll_h = (visible_items * total_h) / (int)KBD_LAYOUT_DISPLAY_COUNT;
        if (scroll_h < 20) scroll_h = 20;
        int max_scroll_kbd = (int)KBD_LAYOUT_DISPLAY_COUNT - visible_items;
        if (max_scroll_kbd < 0) max_scroll_kbd = 0;
        int scroll_y = list_y;
        if (max_scroll_kbd > 0) {
            scroll_y = list_y + (g_setup.kbd_scroll * (total_h - scroll_h)) / max_scroll_kbd;
        }
        if (scroll_y < list_y) scroll_y = list_y;
        if (scroll_y + scroll_h > list_y + total_h) scroll_y = list_y + total_h - scroll_h;
        gfx_fill_rounded_rect(list_x + list_w - 12, scroll_y, 8, scroll_h, C_TEXT_MUTED, 4);
    }

    // Current selection preview
    char preview[64];
    strcpy(preview, "Selected: ");
    strcat(preview, kbd_layout_display_names[g_setup.selected_kbd_idx]);

    gfx_draw_string(card_x + (card_w - strlen(preview) * 8) / 2,
                   card_y + card_h - 100, preview, C_TEXT_MUTED);

    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_WELCOME;
        g_setup.current_step = 0;
    }

    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
        g_setup.config.kbd_layout = g_setup.selected_kbd_idx;
        g_setup.state = SETUP_STATE_USER;
        g_setup.current_step = 2;
        g_setup.input_buffer[0] = 0;
        g_setup.input_cursor = 0;
        g_setup.input_active = 1;
    }

    // Progress dots
    draw_progress_dots(cx, h - 80, 1, g_setup.total_steps);
}

static void render_user_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 480;
    int card_h = 340;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Title
    char* title = "Create Your Account";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 16) / 2, 
                          card_y + 24, title, C_TEXT_DARK, 2);
    
    // Subtitle
    char* subtitle = "This will be your user account name";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 8) / 2, 
                   card_y + 60, subtitle, C_TEXT_MUTED);
    
    // Avatar preview
    int avatar_x = cx;
    int avatar_y = card_y + 120;
    uint32_t avatar_color = themes[g_setup.selected_theme_idx].primary;
    gfx_fill_rounded_rect(avatar_x - 40, avatar_y - 40, 80, 80, avatar_color, 40);
    
    // User icon on avatar
    gfx_fill_rounded_rect(avatar_x - 10, avatar_y - 20, 20, 20, C_TEXT_LIGHT, 10);
    gfx_fill_rounded_rect(avatar_x - 20, avatar_y + 5, 40, 30, C_TEXT_LIGHT, 8);
    
    // Username field
    draw_text_field(cx - 150, card_y + 210, 300, 44, 
                   g_setup.input_buffer, g_setup.input_active, 0, mx, my, click,
                   g_setup.input_cursor);
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_KEYBOARD;
        g_setup.current_step = 1;
    }
    
    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
        if (g_setup.input_buffer[0]) {
            strcpy(g_setup.config.username, g_setup.input_buffer);
            g_setup.state = SETUP_STATE_PASSWORD;
            g_setup.current_step = 3;
            g_setup.password_buffer[0] = 0;
            g_setup.password_confirm[0] = 0;
            g_setup.password_cursor = 0;
            g_setup.confirm_cursor = 0;
            g_setup.password_active = 1;
            g_setup.password_step = 0;
            g_setup.password_match_error = 0;
        }
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 2, g_setup.total_steps);
}

static void render_password_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 480;
    int card_h = 400;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Lock icon at top
    int icon_y = card_y + 20;
    gfx_fill_rounded_rect(cx - 15, icon_y, 30, 24, C_LOCK_ICON, 4);
    gfx_fill_rounded_rect(cx - 10, icon_y - 10, 20, 16, 0x00000000, 8); // Arch
    gfx_fill_rounded_rect(cx - 8, icon_y + 4, 16, 12, C_TEXT_LIGHT, 3);  // Keyhole
    
    // Title
    if (g_setup.password_step == 0) {
        char* title = "Set Your Password";
        gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 16) / 2, 
                              card_y + 50, title, C_TEXT_DARK, 2);
        char* subtitle = "This protects your account and locks your screen";
        gfx_draw_string(card_x + (card_w - strlen(subtitle) * 8) / 2, 
                       card_y + 85, subtitle, C_TEXT_MUTED);
    } else {
        char* title = "Confirm Password";
        gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 16) / 2, 
                              card_y + 50, title, C_TEXT_DARK, 2);
        char* subtitle = "Enter your password again to confirm";
        gfx_draw_string(card_x + (card_w - strlen(subtitle) * 8) / 2, 
                       card_y + 85, subtitle, C_TEXT_MUTED);
    }
    
    // Password field
    if (g_setup.password_step == 0) {
        draw_text_field(cx - 150, card_y + 130, 300, 44,
                       g_setup.password_buffer, g_setup.password_active == 1, 1, mx, my, click,
                       g_setup.password_cursor);
    } else {
        // Show first password as confirmed dots
        gfx_draw_string(cx - 140, card_y + 120, "Password set", C_SUCCESS);
        
        // Confirm field
        draw_text_field(cx - 150, card_y + 150, 300, 44,
                       g_setup.password_confirm, g_setup.password_active == 2, 1, mx, my, click,
                       g_setup.confirm_cursor);
    }
    
    // Optional hint
    char* hint = "Leave blank to skip password protection";
    gfx_draw_string(cx - strlen(hint) * 4, card_y + 220, hint, C_TEXT_MUTED);
    
    // Error message
    if (g_setup.password_match_error) {
        char* error = "Passwords do not match. Try again.";
        gfx_draw_string(cx - strlen(error) * 4, card_y + 250, error, C_ERROR);
    }
    
    // Security note
    char* security = "Your password is encrypted with SHA-256";
    gfx_draw_string(cx - strlen(security) * 4, card_y + 280, security, C_TEXT_MUTED);
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_USER;
        g_setup.current_step = 2;
        g_setup.password_match_error = 0;
    }
    
    if (g_setup.password_step == 0) {
        if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Next", 1, mx, my, click)) {
            if (g_setup.password_buffer[0] == 0) {
                // No password - skip confirmation, go to timezone
                g_setup.state = SETUP_STATE_TIMEZONE;
                g_setup.current_step = 4;
            } else {
                // Move to confirm step
                g_setup.password_step = 1;
                g_setup.password_active = 2;
                g_setup.password_confirm[0] = 0;
                g_setup.confirm_cursor = 0;
                g_setup.password_match_error = 0;
            }
        }
    } else {
        if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
            // Verify passwords match
            if (strcmp(g_setup.password_buffer, g_setup.password_confirm) == 0) {
                g_setup.state = SETUP_STATE_TIMEZONE;
                g_setup.current_step = 4;
                g_setup.password_match_error = 0;
            } else {
                g_setup.password_match_error = 1;
                g_setup.password_confirm[0] = 0;
                g_setup.confirm_cursor = 0;
            }
        }
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 3, g_setup.total_steps);
}

static void render_timezone_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 520;
    int card_h = 400;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2 - 20;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Title
    char* title = "Select Your Time Zone";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 16) / 2, 
                          card_y + 24, title, C_TEXT_DARK, 2);
    
    // Subtitle
    char* subtitle = "This helps show the correct time on your system";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 8) / 2, 
                   card_y + 60, subtitle, C_TEXT_MUTED);
    
    // Timezone list (scrollable region)
    int list_x = card_x + 30;
    int list_y = card_y + 100;
    int list_w = card_w - 60;
    int item_h = 32;
    int visible_items = 7;
    
    // Handle mouse scroll wheel (inline like keyboard page)
    if (mouse_scroll_delta != 0) {
        if (mx >= list_x && mx <= list_x + list_w &&
            my >= list_y && my < list_y + visible_items * item_h) {
            g_setup.tz_scroll -= mouse_scroll_delta;
            int max_scroll_tz = (int)TIMEZONE_COUNT - visible_items;
            if (max_scroll_tz < 0) max_scroll_tz = 0;
            if (g_setup.tz_scroll > max_scroll_tz) g_setup.tz_scroll = max_scroll_tz;
            if (g_setup.tz_scroll < 0) g_setup.tz_scroll = 0;
            // Adjust selected index if it's out of view
            if (g_setup.selected_tz_idx < g_setup.tz_scroll) {
                g_setup.selected_tz_idx = g_setup.tz_scroll;
            }
            if (g_setup.selected_tz_idx >= g_setup.tz_scroll + visible_items) {
                g_setup.selected_tz_idx = g_setup.tz_scroll + visible_items - 1;
            }
        }
        mouse_scroll_delta = 0;
    }
    
    // Clamp scroll so selected item is always visible
    if (g_setup.selected_tz_idx < g_setup.tz_scroll) {
        g_setup.tz_scroll = g_setup.selected_tz_idx;
    }
    if (g_setup.selected_tz_idx >= g_setup.tz_scroll + visible_items) {
        g_setup.tz_scroll = g_setup.selected_tz_idx - visible_items + 1;
    }
    // Clamp scroll bounds
    int max_scroll = (int)TIMEZONE_COUNT - visible_items;
    if (max_scroll < 0) max_scroll = 0;
    if (g_setup.tz_scroll > max_scroll) g_setup.tz_scroll = max_scroll;
    if (g_setup.tz_scroll < 0) g_setup.tz_scroll = 0;
    
    // List background
    gfx_fill_rounded_rect(list_x, list_y, list_w, visible_items * item_h, C_INPUT_BG, 8);
    
    for (int vi = 0; vi < visible_items; vi++) {
        int i = g_setup.tz_scroll + vi;
        if (i >= (int)TIMEZONE_COUNT) break;
        
        int item_y = list_y + vi * item_h;
        int is_selected = (i == g_setup.selected_tz_idx);
        int hover = (mx >= list_x && mx <= list_x + list_w && 
                    my >= item_y && my < item_y + item_h);
        
        if (is_selected || hover) {
            uint32_t bg = is_selected ? C_ACCENT : (hover ? C_BG_TOP : C_INPUT_BG);
            gfx_fill_rounded_rect(list_x + 4, item_y + 2, list_w - 8, item_h - 4, bg, 6);
        }
        
        uint32_t text_color = is_selected ? C_TEXT_LIGHT : C_TEXT_DARK;
        TimeZone* tz = &timezones[i];
        
        gfx_draw_string(list_x + 16, item_y + 8, tz->display, text_color);
    }
    
    // Scroll indicator (only when there are more items than visible)
    if ((int)TIMEZONE_COUNT > visible_items) {
        int total_h = visible_items * item_h;
        int scroll_h = (visible_items * total_h) / (int)TIMEZONE_COUNT;
        if (scroll_h < 20) scroll_h = 20;
        int scroll_y = list_y + (g_setup.tz_scroll * (total_h - scroll_h)) / max_scroll;
        if (scroll_y < list_y) scroll_y = list_y;
        if (scroll_y + scroll_h > list_y + total_h) scroll_y = list_y + total_h - scroll_h;
        gfx_fill_rounded_rect(list_x + list_w - 12, scroll_y, 8, scroll_h, C_TEXT_MUTED, 4);
    }
    
    // Current selection preview
    TimeZone* selected = &timezones[g_setup.selected_tz_idx];
    char preview[64];
    strcpy(preview, "Selected: ");
    strcat(preview, selected->name);
    strcat(preview, " (UTC");
    if (selected->offset_minutes >= 0) strcat(preview, "+");
    char offset_str[8];
    int hours = selected->offset_minutes / 60;
    int_to_str(hours, offset_str);
    strcat(preview, offset_str);
    strcat(preview, ")");
    
    gfx_draw_string(card_x + (card_w - strlen(preview) * 8) / 2, 
                   card_y + card_h - 100, preview, C_TEXT_MUTED);
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_PASSWORD;
        g_setup.current_step = 3;
    }
    
    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
        memcpy(&g_setup.config.timezone, &timezones[g_setup.selected_tz_idx], sizeof(TimeZone));
        g_setup.state = SETUP_STATE_THEME;
        g_setup.current_step = 5;
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 4, g_setup.total_steps);
}

static void render_theme_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 540;
    int card_h = 380;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Title
    char* title = "Choose Your Theme";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 16) / 2, 
                          card_y + 24, title, C_TEXT_DARK, 2);
    
    // Subtitle
    char* subtitle = "Personalize your CamelOS experience";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 8) / 2, 
                   card_y + 60, subtitle, C_TEXT_MUTED);
    
    // Theme options
    int theme_start_x = card_x + 40;
    int theme_y = card_y + 110;
    int theme_w = 90;
    int theme_h = 140;
    int theme_spacing = 100;
    
    for (int i = 0; i < THEME_COUNT; i++) {
        int theme_x = theme_start_x + i * theme_spacing;
        int is_selected = (i == g_setup.selected_theme_idx);
        int hover = (mx >= theme_x && mx <= theme_x + theme_w && 
                    my >= theme_y && my <= theme_y + theme_h);
        
        // Theme preview card
        uint32_t border = is_selected ? C_ACCENT : (hover ? C_TEXT_MUTED : C_BORDER);
        
        // Shadow
        if (is_selected) {
            gfx_fill_rounded_rect(theme_x + 2, theme_y + 3, theme_w, theme_h, 0x20000000, 12);
        }
        
        // Background
        gfx_fill_rounded_rect(theme_x, theme_y, theme_w, theme_h, C_CARD_BG, 12);
        gfx_draw_rect(theme_x, theme_y, theme_w, theme_h, border);
        
        // Color preview (mini window)
        int preview_x = theme_x + 10;
        int preview_y = theme_y + 10;
        int preview_w = theme_w - 20;
        int preview_h = 60;
        
        // Title bar
        gfx_fill_rounded_rect(preview_x, preview_y, preview_w, 20, 
                             themes[i].primary, 6);
        // Traffic lights
        gfx_fill_rounded_rect(preview_x + 6, preview_y + 6, 8, 8, 0xFFFF5F57, 4);
        gfx_fill_rounded_rect(preview_x + 18, preview_y + 6, 8, 8, 0xFFFFBD2E, 4);
        gfx_fill_rounded_rect(preview_x + 30, preview_y + 6, 8, 8, 0xFF28C940, 4);
        
        // Content area
        gfx_fill_rect(preview_x, preview_y + 20, preview_w, preview_h - 20, C_INPUT_BG);
        
        // Sidebar
        gfx_fill_rect(preview_x, preview_y + 20, 20, preview_h - 20, themes[i].secondary);
        
        // Theme name
        gfx_draw_string(theme_x + (theme_w - strlen(themes[i].name) * 8) / 2, 
                       theme_y + theme_h - 30, themes[i].name, 
                       is_selected ? C_ACCENT : C_TEXT_DARK);
        
        // Selection checkmark
        if (is_selected) {
            gfx_draw_string(theme_x + theme_w - 20, theme_y + 5, "OK", C_ACCENT);
        }
    }
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_TIMEZONE;
        g_setup.current_step = 4;
    }
    
    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Get Started", 1, mx, my, click)) {
        g_setup.config.theme = g_setup.selected_theme_idx;
        int save_ok = welcome_setup_finish();
        // Only set COMPLETE if save actually succeeded.
        // If save fails, finish() returns -1 and keeps is_configured=0,
        // so the setup wizard stays active for the user to retry.
        if (save_ok == 0) {
            g_setup.state = SETUP_STATE_COMPLETE;
        } else {
            s_printf("[SETUP] Save failed - staying on theme page for retry.\n");
        }
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 5, g_setup.total_steps);
}

// --- Main Render ---

void welcome_setup_render(uint32_t* buffer, int w, int h, int mx, int my) {
    (void)buffer;
    
    if (g_setup.state == SETUP_STATE_COMPLETE && g_setup.config.is_configured) {
        return;
    }
    
    int cx = w / 2;
    int cy = h / 2;
    
    // Background
    draw_gradient_bg(w, h);
    
    // Animated entrance
    g_setup.anim_progress += 0.05f;
    if (g_setup.anim_progress > 1.0f) g_setup.anim_progress = 1.0f;
    
    // Render based on state
    switch (g_setup.state) {
        case SETUP_STATE_WELCOME:
            render_welcome(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_KEYBOARD:
            render_keyboard_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_USER:
            render_user_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_PASSWORD:
            render_password_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_TIMEZONE:
            render_timezone_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_THEME:
            render_theme_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_COMPLETE:
            // Show completion briefly
            gfx_draw_string_centered(cx, cy - 20, "All Done!", C_SUCCESS, 3);
            break;
    }
}

// --- Input Handling ---

int welcome_setup_handle_key(int key) {
    if (g_setup.state == SETUP_STATE_COMPLETE) {
        return 0;
    }
    
    // Keyboard layout list navigation with arrow keys
    if (g_setup.state == SETUP_STATE_KEYBOARD) {
        if (key == 128 + 2) { // KEY_UP
            if (g_setup.selected_kbd_idx > 0) {
                g_setup.selected_kbd_idx--;
                extern void kbd_set_layout(int);
                kbd_set_layout(g_setup.selected_kbd_idx);
            }
            return 1;
        } else if (key == 128 + 3) { // KEY_DOWN
            if (g_setup.selected_kbd_idx < (int)KBD_LAYOUT_DISPLAY_COUNT - 1) {
                g_setup.selected_kbd_idx++;
                extern void kbd_set_layout(int);
                kbd_set_layout(g_setup.selected_kbd_idx);
            }
            return 1;
        }
    }
    
    // Timezone list navigation with arrow keys
    if (g_setup.state == SETUP_STATE_TIMEZONE) {
        if (key == 128 + 2) { // KEY_UP
            if (g_setup.selected_tz_idx > 0) g_setup.selected_tz_idx--;
            return 1;
        } else if (key == 128 + 3) { // KEY_DOWN
            if (g_setup.selected_tz_idx < (int)TIMEZONE_COUNT - 1) g_setup.selected_tz_idx++;
            return 1;
        }
    }
    
    if (g_setup.state == SETUP_STATE_USER && g_setup.input_active) {
        if (key == 0x08 || key == 0x7F) {
            // Backspace
            if (g_setup.input_cursor > 0) {
                g_setup.input_cursor--;
                g_setup.input_buffer[g_setup.input_cursor] = 0;
            }
        } else if (key == 0x0D || key == '\n') {
            // Enter - accept and continue
            if (g_setup.input_buffer[0]) {
                strcpy(g_setup.config.username, g_setup.input_buffer);
                g_setup.state = SETUP_STATE_PASSWORD;
                g_setup.current_step = 3;
                g_setup.password_active = 1;
                g_setup.password_step = 0;
            }
        } else if (key >= 0x20 && key != 0x7F && (key <= 0x7E || key >= 0xA0) && g_setup.input_cursor < SETUP_USERNAME_MAX - 1) {
            g_setup.input_buffer[g_setup.input_cursor] = (char)key;
            g_setup.input_cursor++;
            g_setup.input_buffer[g_setup.input_cursor] = 0;
        }
        return 1;
    }
    
    // Password step input
    if (g_setup.state == SETUP_STATE_PASSWORD) {
        if (key == 0x08 || key == 0x7F) {
            // Backspace
            if (g_setup.password_step == 0 && g_setup.password_cursor > 0) {
                g_setup.password_cursor--;
                g_setup.password_buffer[g_setup.password_cursor] = 0;
            } else if (g_setup.password_step == 1 && g_setup.confirm_cursor > 0) {
                g_setup.confirm_cursor--;
                g_setup.password_confirm[g_setup.confirm_cursor] = 0;
            }
            g_setup.password_match_error = 0;
        } else if (key == 0x0D || key == '\n') {
            // Enter
            if (g_setup.password_step == 0) {
                if (g_setup.password_buffer[0] == 0) {
                    // No password - skip to timezone
                    g_setup.state = SETUP_STATE_TIMEZONE;
                    g_setup.current_step = 4;
                } else {
                    // Move to confirm
                    g_setup.password_step = 1;
                    g_setup.password_active = 2;
                    g_setup.password_confirm[0] = 0;
                    g_setup.confirm_cursor = 0;
                }
            } else {
                // Verify passwords match
                if (strcmp(g_setup.password_buffer, g_setup.password_confirm) == 0) {
                    g_setup.state = SETUP_STATE_TIMEZONE;
                    g_setup.current_step = 4;
                    g_setup.password_match_error = 0;
                } else {
                    g_setup.password_match_error = 1;
                    g_setup.password_confirm[0] = 0;
                    g_setup.confirm_cursor = 0;
                }
            }
        } else if (key >= 0x20 && key != 0x7F && (key <= 0x7E || key >= 0xA0)) {
            if (g_setup.password_step == 0 && g_setup.password_cursor < SETUP_PASSWORD_MAX - 1) {
                g_setup.password_buffer[g_setup.password_cursor] = (char)key;
                g_setup.password_cursor++;
                g_setup.password_buffer[g_setup.password_cursor] = 0;
            } else if (g_setup.password_step == 1 && g_setup.confirm_cursor < SETUP_PASSWORD_MAX - 1) {
                g_setup.password_confirm[g_setup.confirm_cursor] = (char)key;
                g_setup.confirm_cursor++;
                g_setup.password_confirm[g_setup.confirm_cursor] = 0;
            }
            g_setup.password_match_error = 0;
        }
        return 1;
    }
    
    return 0;
}

int welcome_setup_handle_click(int mx, int my, int click) {
    if (g_setup.state == SETUP_STATE_COMPLETE) {
        return 0;
    }
    
    int w = screen_w ? screen_w : 1024;
    int h = screen_h ? screen_h : 768;
    int cx = w / 2;
    int cy = h / 2;
    
    switch (g_setup.state) {
        case SETUP_STATE_WELCOME:
            render_welcome(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_KEYBOARD:
            render_keyboard_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_USER:
            render_user_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_PASSWORD:
            render_password_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_TIMEZONE:
            render_timezone_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_THEME:
            render_theme_setup(cx, cy, w, h, mx, my, click);
            break;
        default:
            break;
    }
    
    return 1;
}

int welcome_setup_handle_mouse(int mx, int my, int click, int pressed) {
    (void)pressed;
    
    // Handle keyboard layout list scrolling and selection
    if (g_setup.state == SETUP_STATE_KEYBOARD && click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_w = 480;
        int card_h = 400;
        int card_x = cx - card_w / 2;
        int card_y = cy - card_h / 2 - 20;
        
        int list_x = card_x + 30;
        int list_y = card_y + 100;
        int list_w = card_w - 60;
        int item_h = 32;
        int visible_items = 7;
        
        if (mx >= list_x && mx <= list_x + list_w) {
            int rel_y = my - list_y;
            if (rel_y >= 0 && rel_y < visible_items * item_h) {
                int vi = rel_y / item_h;
                int idx = g_setup.kbd_scroll + vi;
                if (idx >= 0 && idx < (int)KBD_LAYOUT_DISPLAY_COUNT) {
                    g_setup.selected_kbd_idx = idx;
                    // Apply keyboard layout immediately so user can test it
                    extern void kbd_set_layout(int);
                    kbd_set_layout(idx);
                }
            }
        }
    }
    
    // Handle keyboard layout list scroll wheel
    if (g_setup.state == SETUP_STATE_KEYBOARD && mouse_scroll_delta != 0) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_w = 480;
        int card_h = 400;
        int card_x = cx - card_w / 2;
        int card_y = cy - card_h / 2 - 20;
        
        int list_x = card_x + 30;
        int list_y = card_y + 100;
        int list_w = card_w - 60;
        int item_h = 32;
        int visible_items = 7;
        
        if (mx >= list_x && mx <= list_x + list_w &&
            my >= list_y && my < list_y + visible_items * item_h) {
            g_setup.kbd_scroll -= mouse_scroll_delta;
            int max_scroll = (int)KBD_LAYOUT_DISPLAY_COUNT - visible_items;
            if (max_scroll < 0) max_scroll = 0;
            if (g_setup.kbd_scroll > max_scroll) g_setup.kbd_scroll = max_scroll;
            if (g_setup.kbd_scroll < 0) g_setup.kbd_scroll = 0;
        }
        mouse_scroll_delta = 0;
    }
    
    // Handle timezone list scrolling and selection
    if (g_setup.state == SETUP_STATE_TIMEZONE && click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_w = 520;
        int card_h = 400;
        int card_x = cx - card_w / 2;
        int card_y = cy - card_h / 2 - 20;
        
        int list_x = card_x + 30;
        int list_y = card_y + 100;
        int list_w = card_w - 60;
        int item_h = 32;
        int visible_items = 7;
        
        if (mx >= list_x && mx <= list_x + list_w) {
            int rel_y = my - list_y;
            if (rel_y >= 0 && rel_y < visible_items * item_h) {
                int vi = rel_y / item_h;
                int idx = g_setup.tz_scroll + vi;
                if (idx >= 0 && idx < (int)TIMEZONE_COUNT) {
                    g_setup.selected_tz_idx = idx;
                }
            }
        }
    }
    
    // Handle timezone list scroll wheel
    if (g_setup.state == SETUP_STATE_TIMEZONE && mouse_scroll_delta != 0) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_w = 520;
        int card_h = 400;
        int card_x = cx - card_w / 2;
        int card_y = cy - card_h / 2 - 20;
        
        int list_x = card_x + 30;
        int list_y = card_y + 100;
        int list_w = card_w - 60;
        int item_h = 32;
        int visible_items = 7;
        
        if (mx >= list_x && mx <= list_x + list_w &&
            my >= list_y && my < list_y + visible_items * item_h) {
            g_setup.tz_scroll -= mouse_scroll_delta;
            int max_scroll = (int)TIMEZONE_COUNT - visible_items;
            if (max_scroll < 0) max_scroll = 0;
            if (g_setup.tz_scroll > max_scroll) g_setup.tz_scroll = max_scroll;
            if (g_setup.tz_scroll < 0) g_setup.tz_scroll = 0;
        }
        mouse_scroll_delta = 0;
    }
    
    // Handle theme selection
    if (g_setup.state == SETUP_STATE_THEME && click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_x = cx - 270;
        int card_y = cy - 190;
        int theme_start_x = card_x + 40;
        int theme_y = card_y + 110;
        int theme_w = 90;
        int theme_h = 140;
        int theme_spacing = 100;
        
        for (int i = 0; i < THEME_COUNT; i++) {
            int theme_x = theme_start_x + i * theme_spacing;
            if (mx >= theme_x && mx <= theme_x + theme_w && 
                my >= theme_y && my <= theme_y + theme_h) {
                g_setup.selected_theme_idx = i;
            }
        }
    }
    
    // Handle text field focus on click (click on input box activates it)
    if (click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        if (g_setup.state == SETUP_STATE_USER) {
            int card_y = cy - 170;
            int field_x = cx - 150, field_y = card_y + 210;
            if (mx >= field_x && mx <= field_x + 300 && my >= field_y && my <= field_y + 44) {
                g_setup.input_active = 1;
            }
        }
        
        if (g_setup.state == SETUP_STATE_PASSWORD) {
            int card_y = cy - 200;
            if (g_setup.password_step == 0) {
                int field_x = cx - 150, field_y = card_y + 130;
                if (mx >= field_x && mx <= field_x + 300 && my >= field_y && my <= field_y + 44) {
                    g_setup.password_active = 1;
                }
            } else {
                int field_x = cx - 150, field_y = card_y + 150;
                if (mx >= field_x && mx <= field_x + 300 && my >= field_y && my <= field_y + 44) {
                    g_setup.password_active = 2;
                }
            }
        }
    }
    
    // Only process button clicks when there's an actual click event
    if (click) {
        return welcome_setup_handle_click(mx, my, click);
    }
    return 1;
}

void welcome_setup_update(float dt) {
    (void)dt;
    // Animation updates handled in render
}
