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

typedef struct {
    int id;
    int is_active;

    // Geometry
    int x, y;
    int width, height;
    int min_w, min_h;

    // Restore State
    int saved_x, saved_y;
    int saved_w, saved_h;

    // Advanced State Management
    int state;          // WIN_STATE_*
    int layer;          // LAYER_*
    float opacity;      // 0.0 to 1.0
    int is_visible;
    int is_focused;

    // Animation Properties
    int anim_state;     // 0=None, 1=Opening, 2=Closing, 3=Minimizing, 4=Restoring
    float anim_t;       // 0.0 to 1.0 progress

    // Visuals
    char title[64];

    // Callbacks
    void* paint_callback;
    void* input_callback;
    void* mouse_callback;

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
void ws_destroy_window(window_t* win);

// Compositor API
int ws_get_count();
window_t* ws_get_window_at_index(int idx); // Returns by Z-order (0 = Bottom)
void ws_bring_to_front(window_t* win);
void ws_send_to_back(window_t* win);

#endif