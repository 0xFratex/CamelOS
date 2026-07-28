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
    .desktop_bg              = 0xFF4A90D9,   // Brighter sky-blue base for gradient
    .menubar_bg              = 0xE6F5F5F7,   // Slightly translucent light grey
    .menubar_text            = 0xFF1C1C1E,   // Near-black
    .window_titlebar         = 0xFFF8F8FA,   // Cleaner light header
    .window_titlebar_unfocused = 0xFFEDEDF0,
    .window_title_text       = 0xFF1D1D1F,   // Near-black title
    .window_title_text_unfocused = 0xFF8E8E93,
    .window_body             = 0xFFFAFAFC,   // Soft white body
    .window_border           = 0xFFC8C8CC,   // Subtle grey border
    .window_border_unfocused = 0xFFD8D8DC,
    .dock_bg                 = 0x66FFFFFF,   // Glassier light dock
    .dock_border             = 0x55FFFFFF,   // Soft white edge
    .dock_icon_bg            = 0xFFFFFFFF,
    .dock_text               = 0xFF1C1C1E,
    .text_primary            = 0xFF1C1C1E,   // Near-black
    .text_secondary          = 0xFF8E8E93,   // Muted grey
    .accent_color            = 0xFF007AFF,   // macOS blue
    .separator               = 0xFFD1D1D6,   // Light separator
    .spotlight_bg            = 0xF0E8E8ED,   // Light search bg
    .notification_bg         = 0xF5F2F2F7,   // Light notif bg
    .notification_text       = 0xFF1C1C1E,
    .notification_border     = 0xFFC7C7CC,
};

// ══════════════════════════════════════════════════════════════════
// Dark Theme — Dark backgrounds, light text, translucent effects
// (macOS Dark Mode-inspired)
// ══════════════════════════════════════════════════════════════════

static const theme_t theme_dark = {
    .desktop_bg              = 0xFF121428,   // Deep navy (pairs with gradient)
    .menubar_bg              = 0xE6282830,   // Translucent dark menu bar
    .menubar_text            = 0xFFF5F5F7,   // Light text
    .window_titlebar         = 0xFF2A2A32,   // Dark header
    .window_titlebar_unfocused = 0xFF222228,
    .window_title_text       = 0xFFE8E8ED,   // Light grey text
    .window_title_text_unfocused = 0xFF8E8E93,
    .window_body             = 0xFF1A1A22,   // Dark body
    .window_border           = 0xFF3E3E48,   // Dark border
    .window_border_unfocused = 0xFF323238,
    .dock_bg                 = 0x66202028,   // Glassier dark dock
    .dock_border             = 0x44505058,
    .dock_icon_bg            = 0xFF3A3A44,
    .dock_text               = 0xFFF5F5F7,
    .text_primary            = 0xFFF5F5F7,   // Light text
    .text_secondary          = 0xFF8E8E93,   // Muted grey
    .accent_color            = 0xFF0A84FF,   // macOS blue (dark mode)
    .separator               = 0xFF3A3A42,   // Dark separator
    .spotlight_bg            = 0xF0282830,   // Dark search bg
    .notification_bg         = 0xF02C2C34,   // Dark notif bg
    .notification_text       = 0xFFF5F5F7,
    .notification_border     = 0xFF484850,
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
