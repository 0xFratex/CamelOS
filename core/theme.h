#ifndef THEME_H
#define THEME_H

#include "../include/types.h"

// Theme types
#define THEME_LIGHT  0
#define THEME_DARK   1

typedef struct {
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

#endif
