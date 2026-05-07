// core/theme.c - CamelOS Theme System (Dark + Light modes)
// macOS-inspired theme switching with persistent preference

#include "theme.h"
#include "memory.h"
#include "../include/string.h"
#include "../hal/drivers/serial.h"

// ── Preference file path ──
#define THEME_PREF_PATH "/Library/Preferences/theme.pref"

// ── Current theme state ──
static int g_theme_id = THEME_LIGHT;
static theme_t g_theme;

// ══════════════════════════════════════════════════════════════════
// Light Theme — White backgrounds, light grays, dark text
// (macOS Aqua-inspired, matches the existing default)
// ══════════════════════════════════════════════════════════════════

static const theme_t theme_light = {
    .desktop_bg              = 0xFF3b80c6,   // Blue gradient base
    .menubar_bg              = 0xFFF5F5F7,   // Light grey
    .menubar_text            = 0xFF1C1C1E,   // Near-black
    .window_titlebar         = 0xFFF0F0F0,   // Light header
    .window_titlebar_unfocused = 0xFFE8E8E8,
    .window_title_text       = 0xFF333333,   // Dark grey
    .window_title_text_unfocused = 0xFF999999,
    .window_body             = 0xFFF6F6F6,   // Off-white
    .window_border           = 0xFFB8B8B8,   // Medium grey
    .window_border_unfocused = 0xFFD0D0D0,   // Light grey
    .dock_bg                 = 0x50F0F0F0,   // Translucent light
    .dock_border             = 0x40FFFFFF,   // Subtle white
    .dock_icon_bg            = 0xFFFFFFFF,
    .dock_text               = 0xFF1C1C1E,
    .text_primary            = 0xFF1C1C1E,   // Near-black
    .text_secondary          = 0xFF8E8E93,   // Muted grey
    .accent_color            = 0xFF007AFF,   // macOS blue
    .separator               = 0xFFC6C6C8,   // Light separator
    .spotlight_bg            = 0xFFE8E8ED,   // Light search bg
    .notification_bg         = 0xFFF2F2F7,   // Light notif bg
    .notification_text       = 0xFF1C1C1E,
    .notification_border     = 0xFFC7C7CC,
};

// ══════════════════════════════════════════════════════════════════
// Dark Theme — Dark backgrounds, light text, translucent effects
// (macOS Dark Mode-inspired)
// ══════════════════════════════════════════════════════════════════

static const theme_t theme_dark = {
    .desktop_bg              = 0xFF1C1C1E,   // Deep dark
    .menubar_bg              = 0xFF2C2C2E,   // Dark menu bar
    .menubar_text            = 0xFFF5F5F7,   // Light text
    .window_titlebar         = 0xFF2C2C2E,   // Dark header
    .window_titlebar_unfocused = 0xFF242426,
    .window_title_text       = 0xFFE5E5E7,   // Light grey text
    .window_title_text_unfocused = 0xFF8E8E93,
    .window_body             = 0xFF1C1C1E,   // Dark body
    .window_border           = 0xFF48484A,   // Dark border
    .window_border_unfocused = 0xFF3A3A3C,
    .dock_bg                 = 0x502C2C2E,   // Translucent dark
    .dock_border             = 0x4048484A,
    .dock_icon_bg            = 0xFF3A3A3C,
    .dock_text               = 0xFFF5F5F7,
    .text_primary            = 0xFFF5F5F7,   // Light text
    .text_secondary          = 0xFF8E8E93,   // Muted grey
    .accent_color            = 0xFF0A84FF,   // macOS blue (dark mode)
    .separator               = 0xFF38383A,   // Dark separator
    .spotlight_bg            = 0xFF2C2C2E,   // Dark search bg
    .notification_bg         = 0xFF2C2C2E,   // Dark notif bg
    .notification_text       = 0xFFF5F5F7,
    .notification_border     = 0xFF48484A,
};

// ══════════════════════════════════════════════════════════════════
// API Implementation
// ══════════════════════════════════════════════════════════════════

void theme_init(void) {
    // Load saved preference, default to light
    theme_load();

    // Apply the theme
    if (g_theme_id == THEME_DARK) {
        memcpy(&g_theme, &theme_dark, sizeof(theme_t));
    } else {
        memcpy(&g_theme, &theme_light, sizeof(theme_t));
    }

    s_printf("[THEME] Initialized (mode=");
    s_printf(g_theme_id == THEME_DARK ? "dark" : "light");
    s_printf(")\n");
}

const theme_t* theme_get_current(void) {
    return &g_theme;
}

void theme_set(int theme_id) {
    if (theme_id < THEME_LIGHT || theme_id > THEME_DARK) return;

    g_theme_id = theme_id;

    if (g_theme_id == THEME_DARK) {
        memcpy(&g_theme, &theme_dark, sizeof(theme_t));
    } else {
        memcpy(&g_theme, &theme_light, sizeof(theme_t));
    }

    // Invalidate the desktop wallpaper cache so it gets regenerated
    // with the new theme colors
    extern int wallpaper_cache_w;
    wallpaper_cache_w = 0;

    theme_save();

    s_printf("[THEME] Switched to ");
    s_printf(g_theme_id == THEME_DARK ? "dark" : "light");
    s_printf(" mode\n");
}

void theme_toggle(void) {
    if (g_theme_id == THEME_LIGHT) {
        theme_set(THEME_DARK);
    } else {
        theme_set(THEME_LIGHT);
    }
}

int theme_get_id(void) {
    return g_theme_id;
}

void theme_save(void) {
    char buf[4];
    buf[0] = '0' + g_theme_id;
    buf[1] = '\n';
    buf[2] = '\0';

    // Ensure the directory exists
    extern int sys_fs_create(const char*, int);
    sys_fs_create("/Library", 1);
    sys_fs_create("/Library/Preferences", 1);

    extern int sys_fs_write(const char*, const char*, int);
    sys_fs_write(THEME_PREF_PATH, buf, 2);
}

void theme_load(void) {
    char buf[16];
    extern int sys_fs_read(const char*, char*, int);
    int len = sys_fs_read(THEME_PREF_PATH, buf, sizeof(buf) - 1);

    if (len > 0) {
        buf[len] = 0;
        // Parse the theme ID (0=light, 1=dark)
        if (buf[0] == '1') {
            g_theme_id = THEME_DARK;
        } else {
            g_theme_id = THEME_LIGHT;
        }
    } else {
        g_theme_id = THEME_LIGHT;  // Default
    }
}
