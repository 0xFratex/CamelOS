// usr/dock.c
#include "dock.h"
#include "lib/camel_framework.h"
#include "../hal/video/gfx_hal.h"
#include "../core/window_server.h"
#include "../core/string.h"
#include "../core/theme.h"

// Externs
extern void execute_program(const char* path);
extern window_t* active_win;

// --- Visual Configuration (Big Sur Style) ---
// Note: DOCK_BG_COLOR and DOCK_SHINE now come from theme->dock_bg
#define DOCK_INDICATOR   0xFF404040 // Dark Grey Dot for active apps
#define DOCK_BASE_SIZE   54
#define DOCK_MAX_SIZE    90
#define DOCK_RANGE       150
#define DOCK_SPACING     12

// Dock State
DockIcon dock_icons[MAX_DOCK_APPS];
int dock_count = 0;

// Forward declarations
void dock_add_app(const char* label, const char* path, const char* icon_res);

void dock_init() {
    dock_count = 0;
    // Register Default Apps - Using /Applications/ path (macOS-like)
    // These .app bundles now have proper Info.plist and bundle structures
    // that the app_bundle system can resolve to CDL executables or built-in apps
    dock_add_app("Finder",    "/Applications/Files.app",      "folder");
    dock_add_app("Terminal",  "/Applications/Terminal.app",   "terminal");
    dock_add_app("Monitor",   "/Applications/Monitor.app",    "waterhole");
    dock_add_app("NetDiag",   "/Applications/NetDiag.app",    "networking");
    dock_add_app("TextEdit",  "/Applications/TextEdit.app",   "file");
    dock_add_app("Browser",   "/Applications/Browser.app",    "browser");
    dock_add_app("Calculator","/Applications/Calculator.app", "calculator");
    dock_add_app("Settings",  "/Applications/Settings.app",   "settings");
    dock_add_app("MacTest",   "/Applications/MacTest.app",    "mactest");
}

void dock_add_app(const char* label, const char* path, const char* icon_res) {
    if (dock_count >= MAX_DOCK_APPS) return;
    strcpy(dock_icons[dock_count].label, label);
    strcpy(dock_icons[dock_count].exec_path, path);
    strncpy(dock_icons[dock_count].icon_res, icon_res ? icon_res : "hdd_icon", 15);
    dock_icons[dock_count].icon_res[15] = 0;
    dock_icons[dock_count].window_ref = 0;
    dock_count++;
}

// Deprecated API stubs
void dock_bind_window(window_t* win) {}
void dock_register(const char* label, int color, Window* win) {
    // Dynamically register an app in the dock if not already present
    if (!label) return;
    // Check if already registered by label
    for (int i = 0; i < dock_count; i++) {
        if (strcmp(dock_icons[i].label, label) == 0) return;
    }
    // Add to dock with a default path and try to match a known icon
    if (dock_count < MAX_DOCK_APPS) {
        char path[128];
        strcpy(path, "/Applications/");
        strcat(path, label);
        strcat(path, ".app");
        // Pick an icon that matches the app name
        const char* icon = "terminal";  // generic app icon
        if (strcmp(label, "Calculator") == 0) icon = "calculator";
        else if (strcmp(label, "MacTest") == 0) icon = "mactest";
        else if (strcmp(label, "About") == 0) icon = "about";
        else if (strcmp(label, "Settings") == 0) icon = "settings";
        else if (strcmp(label, "Browser") == 0) icon = "browser";
        else if (strcmp(label, "Terminal") == 0) icon = "terminal";
        else if (strcmp(label, "TextEdit") == 0) icon = "file";
        else if (strcmp(label, "Finder") == 0 || strcmp(label, "Files") == 0) icon = "folder";
        else if (strcmp(label, "Monitor") == 0) icon = "waterhole";
        else if (strcmp(label, "NetDiag") == 0) icon = "networking";
        dock_add_app(label, path, icon);
    }
}

// Helper: Find window by title match
window_t* find_app_window(const char* label_fragment) {
    for(int i = ws_get_count() - 1; i >= 0; i--) {
        window_t* w = ws_get_window_at_index(i);
        if (w && strstr(w->title, label_fragment)) return w;
    }
    return 0;
}

// Check if app is running for indicator dot
int is_app_running(const char* label_fragment) {
    for(int i = 0; i < ws_get_count(); i++) {
        window_t* w = ws_get_window_at_index(i);
        if (w && strstr(w->title, label_fragment)) return 1;
    }
    return 0;
}

// Calculate Layout (Magnification)
void get_dock_layout(int screen_w, int mx, int my, int* x_positions, int* sizes, int* total_w) {
    *total_w = 0;
    int dock_bottom_area = 768 - 100;

    for(int i=0; i<dock_count; i++) {
        sizes[i] = DOCK_BASE_SIZE;
        
        // Magnification Logic
        if (my > dock_bottom_area) {
            int group_w = dock_count * (DOCK_BASE_SIZE + DOCK_SPACING);
            int start_x_est = (screen_w - group_w) / 2;
            int icon_center_x = start_x_est + (i * (DOCK_BASE_SIZE + DOCK_SPACING)) + (DOCK_BASE_SIZE/2);
            
            int dist = mx - icon_center_x;
            if(dist < 0) dist = -dist;
            
            if (dist < DOCK_RANGE) {
                // Linear interpolation for speed
                sizes[i] += (DOCK_MAX_SIZE - DOCK_BASE_SIZE) * (DOCK_RANGE - dist) / DOCK_RANGE;
            }
        }
        *total_w += sizes[i] + DOCK_SPACING;
    }
    
    if (dock_count > 0) *total_w -= DOCK_SPACING;

    int start_x = (screen_w - *total_w) / 2;
    for(int i=0; i<dock_count; i++) {
        x_positions[i] = start_x;
        start_x += sizes[i] + DOCK_SPACING;
    }
}

// Genie Effect Coordinate Helper
void dock_get_window_rect(window_t* win, int* out_x, int* out_y, int* out_w, int* out_h) {
    *out_x = 512; *out_y = 768; *out_w = 10; *out_h = 10; // Defaults

    int w = 1024; // Should ideally be dynamic
    int x_pos[MAX_DOCK_APPS], sizes[MAX_DOCK_APPS], total_w;
    get_dock_layout(w, -1000, -1000, x_pos, sizes, &total_w);

    int shelf_h = 74;
    int shelf_y = 768 - shelf_h - 12;

    for(int i=0; i<dock_count; i++) {
        const char* lbl = dock_icons[i].label;
        const char* match = lbl;
        if(strcmp(lbl, "Monitor") == 0) match = "Activity";
        if(strcmp(lbl, "NetDiag") == 0) match = "Network";

        if (win->title && strstr(win->title, match)) {
            *out_x = x_pos[i];
            *out_y = shelf_y + 10;
            *out_w = sizes[i];
            *out_h = sizes[i];
            return;
        }
    }
}

int dock_handle_click(int mx, int my, int w, int h) {
    if (my < h - 100) return 0;

    int x_pos[MAX_DOCK_APPS], sizes[MAX_DOCK_APPS], total_w;
    get_dock_layout(w, mx, my, x_pos, sizes, &total_w);

    for(int i=0; i<dock_count; i++) {
        if (mx >= x_pos[i] && mx <= x_pos[i] + sizes[i]) {
            const char* match = dock_icons[i].label;
            if(strcmp(match, "Monitor") == 0) match = "Activity";

            window_t* win = find_app_window(match);

            if (win) {
                if (win->is_visible && win->state != WIN_STATE_MINIMIZED && win == active_win) {
                    win->anim_state = 3; // Minimize
                    win->anim_t = 0.0f;
                } else {
                    win->is_visible = 1;
                    if(win->state == WIN_STATE_MINIMIZED) {
                        win->state = WIN_STATE_NORMAL;
                        win->anim_state = 4; // Restore
                        win->anim_t = 0.0f;
                    }
                    ws_bring_to_front(win);
                }
            } else {
                execute_program(dock_icons[i].exec_path);
            }
            return 1;
        }
    }
    return 0;
}

void dock_render(uint32_t* buffer, int w, int h, int mx, int my) {
    if(dock_count == 0) return;
    
    int x_pos[MAX_DOCK_APPS], sizes[MAX_DOCK_APPS], total_w;
    get_dock_layout(w, mx, my, x_pos, sizes, &total_w);

    int shelf_h = 76;
    int shelf_y = h - shelf_h - 14; // Float off bottom
    int padding_x = 26;
    int shelf_w = total_w + (padding_x * 2);
    int shelf_x = (w - shelf_w) / 2;

    const theme_t* theme = theme_get_current();
    int is_dark = (theme_get_id() == THEME_DARK);

    // Soft drop shadow under the dock (stacked translucent rects)
    for (int s = 4; s >= 1; s--) {
        uint32_t shadow = is_dark ? 0x18000000 : 0x14000000;
        gfx_fill_rounded_rect(shelf_x - s, shelf_y + s + 2, shelf_w + s * 2,
                              shelf_h, shadow, 24);
    }

    // Glass background
    gfx_fill_rounded_rect(shelf_x, shelf_y, shelf_w, shelf_h, theme->dock_bg, 22);

    // Top edge highlight (glass sheen)
    uint32_t sheen = is_dark ? 0x28FFFFFF : 0x55FFFFFF;
    gfx_fill_rounded_rect(shelf_x + 3, shelf_y + 2, shelf_w - 6, 14, sheen, 10);

    // Subtle border ring
    gfx_draw_rect(shelf_x, shelf_y, shelf_w, shelf_h, theme->dock_border);

    for(int i=0; i<dock_count; i++) {
        int sz = sizes[i];
        int y = shelf_y + (shelf_h - sz)/2 - 4;

        // Magnified icons lift slightly upward
        if (sz > DOCK_BASE_SIZE) {
            y -= (sz - DOCK_BASE_SIZE) / 3;
        }

        cm_draw_image(buffer, dock_icons[i].icon_res, x_pos[i], y, sz, sz);

        const char* match = dock_icons[i].label;
        if(strcmp(match, "Monitor") == 0) match = "Activity";
        if(strcmp(match, "NetDiag") == 0) match = "Network";
        if(strcmp(match, "Calculator") == 0) match = "Calculator";

        if(is_app_running(match)) {
            // Rounded indicator pill under the icon
            int dot_sz = 5;
            int dot_x = x_pos[i] + (sz - dot_sz)/2;
            int dot_y = shelf_y + shelf_h - 10;
            gfx_fill_rounded_rect(dot_x, dot_y, dot_sz, 4, theme->accent_color, 2);
        }
    }
}