#ifndef THEME_H
#define THEME_H

#include "../include/types.h"

// Theme types
#define THEME_LIGHT  0
#define THEME_DARK   1

// ─────────────────────────────────────────────────────────────────────────
// Style tokens — shared across all surfaces (system + installer)
// Classic Aqua (10.4-10.6) inspired radii/shadows/gloss
// ─────────────────────────────────────────────────────────────────────────
#define AQUA_RADIUS_WINDOW     10
#define AQUA_RADIUS_CARD       12
#define AQUA_RADIUS_BUTTON     6
#define AQUA_RADIUS_PILL       999    /* sentinel: clamped to h/2 */
#define AQUA_RADIUS_MENU       8
#define AQUA_RADIUS_DOCK       22

#define AQUA_TITLEBAR_HEIGHT   38     /* must match compositor.c + bubbleview.c HEADER_HEIGHT + window_server.h TITLE_BAR_HEIGHT */
#define AQUA_MENUBAR_HEIGHT    28

#define AQUA_SHADOW_SOFT       0x15000000
#define AQUA_SHADOW_MEDIUM     0x28000000
#define AQUA_SHADOW_STRONG     0x40000000
#define AQUA_SHADOW_TITLE      0x30000000   /* title text shadow */

/* Pinstripe overlay alpha for Aqua title bars (very subtle horizontal lines) */
#define AQUA_PINSTRIPE_LIGHT   0x08FFFFFF
#define AQUA_PINSTRIPE_DARK    0x08000000

/* Gloss highlight alpha for Aqua candy buttons (top half sheen) */
#define AQUA_GLOSS_LIGHT       0x66FFFFFF
#define AQUA_GLOSS_DARK        0x33FFFFFF

typedef struct {
    // ── Original 23 fields (kept for binary compat with existing callers) ──
    uint32_t desktop_bg;          // Desktop background
    uint32_t menubar_bg;          // Menu bar background
    uint32_t menubar_text;        // Menu bar text color
    uint32_t window_titlebar;     // Window title bar
    uint32_t window_titlebar_unfocused;
    uint32_t window_title_text;
    uint32_t window_title_text_unfocused;
    uint32_t window_body;         // Window content background
    uint32_t window_border;
    uint32_t window_border_unfocused;
    uint32_t dock_bg;            // Dock background
    uint32_t dock_border;
    uint32_t dock_icon_bg;
    uint32_t dock_text;
    uint32_t text_primary;        // Primary text color
    uint32_t text_secondary;      // Secondary/muted text
    uint32_t accent_color;        // Accent/highlight color
    uint32_t separator;           // Separator/divider lines
    uint32_t spotlight_bg;        // Spotlight search bg
    uint32_t notification_bg;     // Notification background
    uint32_t notification_text;
    uint32_t notification_border;

    // ── New tokens (Aqua refresh + cleanup) ──
    uint32_t accent_hover;        // Hovered-accent (darken 12%)
    uint32_t accent_pressed;      // Pressed-accent (darken 20%)

    // Traffic lights (Aqua glossy pills, 12px rounded squares)
    uint32_t tl_close;            // 0xFFFF5F57 light / 0xFFFF453A dark
    uint32_t tl_close_idle;       // dimmer when not hovered
    uint32_t tl_min;
    uint32_t tl_min_idle;
    uint32_t tl_max;
    uint32_t tl_max_idle;
    uint32_t tl_glyph;            // inner X/-/+ icon color
    uint32_t tl_border;           // 1px ring around each pill

    // Status colors
    uint32_t success_color;       // 0xFF34C759
    uint32_t warning_color;       // 0xFFFF9500
    uint32_t error_color;         // 0xFFFF3B30
    uint32_t danger_color;        // 0xFFFF375F (CTA-destructive)

    // Surface tones
    uint32_t card_bg;             // white card on light / dark grey card on dark
    uint32_t card_border;
    uint32_t input_bg;            // text-field fill
    uint32_t input_border;
    uint32_t sidebar_bg;          // installer/util sidebar
    uint32_t modal_dim;           // overlay dim (0x80000000)
    uint32_t page_bg;             // page background (slightly off from window_body)
    uint32_t page_bg_bottom;      // gradient bottom

    // Glass / Aqua specifics
    uint32_t glass_tint;          // 40% white over blur (light) / 40% dark (dark)
    uint32_t gloss_highlight;     // top-half sheen on candy buttons
    uint32_t pinstripe;           // pinstripe overlay alpha (very subtle)
    uint32_t aqua_sphere_top;     // installer welcome "Aqua sphere" light facet
    uint32_t aqua_sphere_bot;     // darker facet

    // Shadows (themeable so dark mode can lighten them)
    uint32_t shadow_soft;
    uint32_t shadow_medium;
    uint32_t shadow_strong;
    uint32_t shadow_title;        // title text shadow

    // Brushed metal (Aqua 10.4 era) — used for installer utility chrome
    uint32_t metal_light;
    uint32_t metal_dark;

    // Lock screen specific
    uint32_t lock_bg_top;
    uint32_t lock_bg_bottom;
    uint32_t lock_input_bg;       // 0x40FFFFFF translucent
    uint32_t lock_avatar_bg;
} theme_t;

// Initialize theme subsystem
void theme_init(void);

// Get current theme
const theme_t* theme_get_current(void);

// Set theme
void theme_set(int theme_id);

// Toggle between dark/light
void theme_toggle(void);

// Get current theme ID
int theme_get_id(void);

// Persist theme preference
void theme_save(void);
void theme_load(void);

// ── Convenience color helpers (work on any ARGB) ──
uint32_t theme_darken(uint32_t color, int amount);   // amount 0..255
uint32_t theme_lighten(uint32_t color, int amount);
uint32_t theme_alpha(uint32_t color, uint8_t a);     // replace alpha byte
int      theme_is_light(uint32_t color);             // perceptual luminance check

#endif
