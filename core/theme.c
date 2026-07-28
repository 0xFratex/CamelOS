// core/theme.c - CamelOS Theme System (Dark + Light modes)
// Classic Aqua refresh — extends the original 23-field theme with traffic lights,
// status colors, glass/gloss tokens, pinstripes, Aqua-sphere facets, brushed metal,
// shadow constants, and lock-screen tokens. All surfaces (system + installer) now
// share this single source of truth.

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
// Light Theme — Classic Mac OS X Aqua (10.4-10.6 inspired)
// White surfaces, glossy candy accents, pinstriped title bars,
// sky-blue desktop, brushed-metal utility chrome.
// ══════════════════════════════════════════════════════════════════

static const theme_t theme_light = {
    /* ── original 23 fields ── */
    .desktop_bg                = 0xFF5A8FD9,   // Classic Aqua sky blue (a touch brighter)
    .menubar_bg                = 0xE6F5F5F7,   // Translucent light grey
    .menubar_text              = 0xFF1C1C1E,
    .window_titlebar           = 0xFFF7F7FA,   // Pinstriped titlebar base (light)
    .window_titlebar_unfocused = 0xFFE8E8EC,
    .window_title_text         = 0xFF1D1D1F,
    .window_title_text_unfocused = 0xFF8E8E93,
    .window_body               = 0xFFFAFAFC,
    .window_border             = 0xFFA8A8AE,   // Slightly darker for Aqua definition
    .window_border_unfocused   = 0xFFC8C8CC,
    .dock_bg                   = 0x99FFFFFF,   // More opaque than before — Aqua dock was solid glass
    .dock_border               = 0x66FFFFFF,
    .dock_icon_bg              = 0xFFFFFFFF,
    .dock_text                 = 0xFF1C1C1E,
    .text_primary              = 0xFF1C1C1E,
    .text_secondary            = 0xFF8E8E93,
    .accent_color              = 0xFF007AFF,   // macOS blue
    .separator                 = 0xFFD1D1D6,
    .spotlight_bg              = 0xF0E8E8ED,
    .notification_bg           = 0xF5F2F2F7,
    .notification_text         = 0xFF1C1C1E,
    .notification_border       = 0xFFC7C7CC,

    /* ── new tokens ── */
    .accent_hover              = 0xFF0066CC,   // 12% darker than 0xFF007AFF
    .accent_pressed            = 0xFF0051D5,   // 20% darker

    // Traffic lights — Aqua 10.4-10.6 candy style (saturated, glossy)
    .tl_close                  = 0xFFFF5F57,
    .tl_close_idle             = 0xFFFF3B30,
    .tl_min                    = 0xFFFFBD2E,
    .tl_min_idle               = 0xFFFFAB00,
    .tl_max                    = 0xFF28C940,
    .tl_max_idle               = 0xFF1AAA2E,
    .tl_glyph                  = 0xFF4D0000,   // dark red X (recolors per-button at draw time)
    .tl_border                 = 0xFFC8C8CC,

    .success_color             = 0xFF34C759,
    .warning_color             = 0xFFFF9500,
    .error_color               = 0xFFFF3B30,
    .danger_color              = 0xFFFF375F,

    .card_bg                   = 0xFFFFFFFF,
    .card_border               = 0xFFC6C6C8,
    .input_bg                  = 0xFFF2F2F7,
    .input_border              = 0xFFC6C6C8,
    .sidebar_bg                = 0xFFECECEC,   // Aqua pinstriped sidebar
    .modal_dim                 = 0x80000000,
    .page_bg                   = 0xFFF2F2F7,
    .page_bg_bottom            = 0xFFE8E8ED,

    .glass_tint                = 0x66FFFFFF,
    .gloss_highlight           = AQUA_GLOSS_LIGHT,   // 0x66FFFFFF
    .pinstripe                 = AQUA_PINSTRIPE_LIGHT,
    .aqua_sphere_top           = 0xCCF0F4FA,   // light facet of welcome bg sphere
    .aqua_sphere_bot           = 0xAEA8B8C8,   // darker facet

    .shadow_soft               = AQUA_SHADOW_SOFT,
    .shadow_medium             = AQUA_SHADOW_MEDIUM,
    .shadow_strong             = AQUA_SHADOW_STRONG,
    .shadow_title              = AQUA_SHADOW_TITLE,

    .metal_light               = 0xFFE8E8EC,   // brushed metal top
    .metal_dark                = 0xFFC8C8CE,   // brushed metal bottom

    .lock_bg_top               = 0xFF1A3A6C,   // Classic Aqua deep blue (matches desktop)
    .lock_bg_bottom            = 0xFF0E2147,
    .lock_input_bg             = 0x40FFFFFF,
    .lock_avatar_bg            = 0xFF4A5568,
};

// ══════════════════════════════════════════════════════════════════
// Dark Theme — Modern dark with Aqua accents preserved
// ══════════════════════════════════════════════════════════════════

static const theme_t theme_dark = {
    /* ── original 23 fields ── */
    .desktop_bg                = 0xFF121428,
    .menubar_bg                = 0xE6282830,
    .menubar_text              = 0xFFF5F5F7,
    .window_titlebar           = 0xFF2A2A32,
    .window_titlebar_unfocused = 0xFF222228,
    .window_title_text         = 0xFFE8E8ED,
    .window_title_text_unfocused = 0xFF8E8E93,
    .window_body               = 0xFF1A1A22,
    .window_border             = 0xFF3E3E48,
    .window_border_unfocused   = 0xFF323238,
    .dock_bg                   = 0x99202028,
    .dock_border               = 0x44505058,
    .dock_icon_bg              = 0xFF3A3A44,
    .dock_text                 = 0xFFF5F5F7,
    .text_primary              = 0xFFF5F5F7,
    .text_secondary            = 0xFF8E8E93,
    .accent_color              = 0xFF0A84FF,
    .separator                 = 0xFF3A3A42,
    .spotlight_bg              = 0xF0282830,
    .notification_bg           = 0xF02C2C34,
    .notification_text         = 0xFFF5F5F7,
    .notification_border       = 0xFF484850,

    /* ── new tokens ── */
    .accent_hover              = 0xFF0A78E0,
    .accent_pressed            = 0xFF0866C0,

    .tl_close                  = 0xFFFF5F57,
    .tl_close_idle             = 0xFFCC453D,
    .tl_min                    = 0xFFFFBD2E,
    .tl_min_idle               = 0xFFCC9724,
    .tl_max                    = 0xFF28C940,
    .tl_max_idle               = 0xFF1F9A30,
    .tl_glyph                  = 0xFF4D0000,
    .tl_border                 = 0xFF484850,

    .success_color             = 0xFF30D158,
    .warning_color             = 0xFFFF9F0A,
    .error_color               = 0xFFFF453A,
    .danger_color              = 0xFFFF375F,

    .card_bg                   = 0xFF2A2A32,
    .card_border               = 0xFF3A3A42,
    .input_bg                  = 0xFF1C1C24,
    .input_border              = 0xFF3A3A42,
    .sidebar_bg                = 0xFF1C1C24,
    .modal_dim                 = 0x80000000,
    .page_bg                   = 0xFF1C1C24,
    .page_bg_bottom            = 0xFF121218,

    .glass_tint                = 0x66202028,
    .gloss_highlight           = AQUA_GLOSS_DARK,    // 0x33FFFFFF
    .pinstripe                 = AQUA_PINSTRIPE_DARK,
    .aqua_sphere_top           = 0xCC2A2A3A,
    .aqua_sphere_bot           = 0xAE181820,

    .shadow_soft               = 0x10000000,
    .shadow_medium             = 0x20000000,
    .shadow_strong             = 0x35000000,
    .shadow_title              = 0x40000000,

    .metal_light               = 0xFF2A2A32,
    .metal_dark                = 0xFF1C1C24,

    .lock_bg_top               = 0xFF0E0E1A,
    .lock_bg_bottom            = 0xFF06060C,
    .lock_input_bg             = 0x40FFFFFF,
    .lock_avatar_bg            = 0xFF2A2A32,
};

// ══════════════════════════════════════════════════════════════════
// Color helpers — usable on any ARGB value (not just theme colors)
// ══════════════════════════════════════════════════════════════════

uint32_t theme_darken(uint32_t color, int amount) {
    if (amount < 0) amount = 0;
    if (amount > 255) amount = 255;
    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    r = (r > amount) ? (r - amount) : 0;
    g = (g > amount) ? (g - amount) : 0;
    b = (b > amount) ? (b - amount) : 0;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t theme_lighten(uint32_t color, int amount) {
    if (amount < 0) amount = 0;
    if (amount > 255) amount = 255;
    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    r = (r + amount > 255) ? 255 : (r + amount);
    g = (g + amount > 255) ? 255 : (g + amount);
    b = (b + amount > 255) ? 255 : (b + amount);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t theme_alpha(uint32_t color, uint8_t a) {
    return ((uint32_t)a << 24) | (color & 0x00FFFFFF);
}

int theme_is_light(uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    // Perceptual luminance (Rec. 709 weights)
    uint32_t lum = (r * 54 + g * 183 + b * 19) / 256;
    return lum > 128;
}

// ══════════════════════════════════════════════════════════════════
// API Implementation
// ══════════════════════════════════════════════════════════════════

void theme_init(void) {
    theme_load();

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
        if (buf[0] == '1') {
            g_theme_id = THEME_DARK;
        } else {
            g_theme_id = THEME_LIGHT;
        }
    } else {
        g_theme_id = THEME_LIGHT;
    }
}
