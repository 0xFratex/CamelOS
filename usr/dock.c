// usr/dock.c
#include "dock.h"
#include "lib/camel_framework.h"
#include "../hal/video/gfx_hal.h"
#include "../core/window_server.h"
#include "../core/string.h"

// Externs
extern void execute_program(const char* path);
extern window_t* active_win;

// --- Visual Configuration (Big Sur Style) ---
#define DOCK_BG_COLOR    0x50F0F0F0 // Translucent White/Light Grey (Glass)
#define DOCK_SHINE       0x20FFFFFF // Subtle inner highlight
#define DOCK_INDICATOR   0xFF404040 // Dark Grey Dot for active apps
#define DOCK_BASE_SIZE   54
#define DOCK_MAX_SIZE    90
#define DOCK_RANGE       150
#define DOCK_SPACING     12

// Dock State
DockIcon dock_icons[MAX_DOCK_APPS];
int dock_count = 0;

void dock_init() {
    dock_count = 0;
    // Register Default Apps
    dock_add_app("Finder",   "/usr/apps/Files.app",     "folder");
    dock_add_app("Terminal", "/usr/apps/Terminal.app",  "terminal");
    dock_add_app("Monitor",  "/usr/apps/Waterhole.app", "waterhole");
    dock_add_app("NetTools", "/usr/apps/NetTools.app",  "networking");
    dock_add_app("TextEdit", "/usr/apps/TextEdit.app",  "file");
    dock_add_app("Browser",  "/usr/apps/Browser.app",   "browser");
}

void dock_add_app(const char* label, const char* path, const char* icon_res) {
    if (dock_count >= MAX_DOCK_APPS) return;
    strcpy(dock_icons[dock_count].label, label);
    strcpy(dock_icons[dock_count].exec_path, path);
    dock_icons[dock_count].window_ref = 0;
    dock_count++;
}

// Deprecated API stubs
void dock_bind_window(window_t* win) {}
void dock_register(const char* label, int color, Window* win) {}

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
        const char* match = (strcmp(lbl, "Monitor") == 0) ? "Activity" : lbl;

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

    int shelf_h = 74;
    int shelf_y = h - shelf_h - 12; // Float off bottom
    int padding_x = 24;
    int shelf_w = total_w + (padding_x * 2);
    int shelf_x = (w - shelf_w) / 2;

    // 1. Draw Background (Glass Effect)
    // We use a high-transparency fill. 
    // IMPORTANT: To fix artifacts, the GFX HAL needs to handle blending properly (Source Over Destination).
    gfx_fill_rounded_rect(shelf_x, shelf_y, shelf_w, shelf_h, DOCK_BG_COLOR, 22);
    
    // 2. Inner Shine (Top Highlight)
    // Drawn slightly smaller to look like a bevel
    gfx_fill_rounded_rect(shelf_x+2, shelf_y+2, shelf_w-4, shelf_h-4, DOCK_SHINE, 20);

    for(int i=0; i<dock_count; i++) {
        int sz = sizes[i];
        int y = shelf_y + (shelf_h - sz)/2 - 4; 

        const char* asset = "file";
        if(i==0) asset = "folder";
        if(i==1) asset = "terminal";
        if(i==2) asset = "waterhole";
        if(i==3) asset = "networking";
        if(i==4) asset = "file";
        if(i==5) asset = "browser";

        cm_draw_image(buffer, asset, x_pos[i], y, sz, sz);

        const char* match = dock_icons[i].label;
        if(strcmp(match, "Monitor") == 0) match = "Activity";

        if(is_app_running(match)) {
            // Indicator Dot
            int dot_sz = 4;
            int dot_x = x_pos[i] + (sz - dot_sz)/2;
            int dot_y = shelf_y + shelf_h - 8;
            
            // Simple 4x4 rect acting as circle
            gfx_fill_rect(dot_x, dot_y, dot_sz, dot_sz, DOCK_INDICATOR);
        }
    }
}