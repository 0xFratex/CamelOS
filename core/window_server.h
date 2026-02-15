// core/window_server.h
#ifndef WINDOW_SERVER_H
#define WINDOW_SERVER_H

#include "../sys/api.h"
#include "../common/gui_types.h"

#define MAX_WINDOWS 32

// Z-Order Layers
#define LAYER_DESKTOP    0
#define LAYER_NORMAL     1
#define LAYER_ALWAYS_TOP 2
#define LAYER_POPUP      3
#define LAYER_OVERLAY    4 // App Switcher, Notifications

// Window States
#define WIN_STATE_NORMAL    0
#define WIN_STATE_MINIMIZED 1
#define WIN_STATE_MAXIMIZED 2
#define WIN_STATE_SNAPPED   3

// Window animations
#define WIN_ANIM_NONE       0
#define WIN_ANIM_OPENING    1
#define WIN_ANIM_CLOSING    2
#define WIN_ANIM_MINIMIZING 3
#define WIN_ANIM_RESTORING  4
#define WIN_ANIM_MAXIMIZING 5
#define WIN_ANIM_RESIZE     6

// Window resize edges
#define RESIZE_NONE    0
#define RESIZE_TOP     1
#define RESIZE_BOTTOM  2
#define RESIZE_LEFT    4
#define RESIZE_RIGHT   8
#define RESIZE_TOP_LEFT     (RESIZE_TOP | RESIZE_LEFT)
#define RESIZE_TOP_RIGHT    (RESIZE_TOP | RESIZE_RIGHT)
#define RESIZE_BOTTOM_LEFT  (RESIZE_BOTTOM | RESIZE_LEFT)
#define RESIZE_BOTTOM_RIGHT (RESIZE_BOTTOM | RESIZE_RIGHT)

// Window style flags
#define WIN_STYLE_STANDARD     0x00
#define WIN_STYLE_TOOL_WINDOW  0x01  // No minimize/maximize
#define WIN_STYLE_BORDERLESS   0x02  // No title bar
#define WIN_STYLE_MODAL        0x04  // Modal dialog
#define WIN_STYLE_FULLSCREEN   0x08  // Fullscreen

// Title bar height
#define TITLE_BAR_HEIGHT 28

// Resize border width
#define RESIZE_BORDER_WIDTH 6

typedef struct {
    int id;
    int is_active;

    // Geometry
    int x, y;
    int width, height;
    int min_w, min_h;
    int max_w, max_h;

    // Restore State
    int saved_x, saved_y;
    int saved_w, saved_h;

    // Advanced State Management
    int state;          // WIN_STATE_*
    int layer;          // LAYER_*
    float opacity;      // 0.0 to 1.0
    int is_visible;
    int is_focused;
    int style_flags;    // WIN_STYLE_*

    // Animation Properties
    int anim_state;     // WIN_ANIM_*
    float anim_t;       // 0.0 to 1.0 progress
    int anim_start_x, anim_start_y;
    int anim_start_w, anim_start_h;
    int anim_end_x, anim_end_y;
    int anim_end_w, anim_end_h;
    uint32_t anim_start_time;

    // Resize state
    int is_resizing;
    int resize_edge;
    int resize_start_x, resize_start_y;
    int resize_orig_w, resize_orig_h;
    int resize_orig_x, resize_orig_y;

    // Drag state
    int is_dragging;
    int drag_start_x, drag_start_y;
    int drag_orig_x, drag_orig_y;

    // Visuals
    char title[64];
    uint32_t title_bar_color;
    uint32_t border_color;
    uint32_t background_color;
    int corner_radius;
    int has_shadow;
    int shadow_radius;
    uint32_t shadow_color;

    // Callbacks
    void* paint_callback;
    void* input_callback;
    void* mouse_callback;
    void* resize_callback;    // Called when window is resized
    void* close_callback;     // Called before window closes

    int menu_count;
    MenuCategory menus[MAX_MENUS];
    void* on_menu_action;

    int owner_pid;
    // Icon asset name for App Switcher / Dock
    char icon_name[32];
} window_t;

// Kernel API
void ws_init();
window_t* ws_create_window(const char* title, int w, int h,
                          void* paint_cb, void* input_cb, void* mouse_cb);
window_t* ws_create_window_ex(const char* title, int x, int y, int w, int h,
                              int style_flags, void* paint_cb, void* input_cb, void* mouse_cb);
void ws_destroy_window(window_t* win);

// Window manipulation
void ws_set_title(window_t* win, const char* title);
void ws_set_geometry(window_t* win, int x, int y, int w, int h);
void ws_set_min_size(window_t* win, int min_w, int min_h);
void ws_set_max_size(window_t* win, int max_w, int max_h);
void ws_set_opacity(window_t* win, float opacity);
void ws_set_style(window_t* win, int style_flags);
void ws_set_colors(window_t* win, uint32_t title_bar, uint32_t border, uint32_t bg);

// Window state
void ws_minimize(window_t* win);
void ws_maximize(window_t* win);
void ws_restore(window_t* win);
void ws_close(window_t* win);

// Compositor API
int ws_get_count();
window_t* ws_get_window_at_index(int idx); // Returns by Z-order (0 = Bottom)
window_t* ws_get_window_at_position(int x, int y); // Get window under cursor
void ws_bring_to_front(window_t* win);
void ws_send_to_back(window_t* win);

// Hit testing
int ws_hit_test(window_t* win, int x, int y);  // Returns RESIZE_* or 0
int ws_is_in_title_bar(window_t* win, int x, int y);
int ws_is_in_resize_border(window_t* win, int x, int y);

// Animation
void ws_start_animation(window_t* win, int anim_type, int duration_ms);
void ws_update_animations(void);

// Mouse handling
void ws_handle_mouse(int x, int y, int button);
void ws_handle_mouse_move(int x, int y);
void ws_handle_mouse_down(int x, int y, int button);
void ws_handle_mouse_up(int x, int y, int button);

// Active window
extern window_t* active_win;
window_t* ws_get_active_window(void);
void ws_set_active_window(window_t* win);

#endif