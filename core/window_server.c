// core/window_server.c
#include "window_server.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"

// Import screen size
extern int screen_w;
extern int screen_h;

static window_t window_store[MAX_WINDOWS];
static window_t* z_order[MAX_WINDOWS];
static int next_win_id = 1;

window_t* active_win = 0;

void ws_init() {
    memset(window_store, 0, sizeof(window_store));
    memset(z_order, 0, sizeof(z_order));
    next_win_id = 1;
}

static void z_add(window_t* w) {
    for(int i=0; i<MAX_WINDOWS; i++) {
        if(z_order[i] == 0) {
            z_order[i] = w;
            return;
        }
    }
}

static void z_remove(window_t* w) {
    int found = 0;
    for(int i=0; i<MAX_WINDOWS-1; i++) {
        if(z_order[i] == w) found = 1;
        if(found) {
            z_order[i] = z_order[i+1];
        }
    }
    z_order[MAX_WINDOWS-1] = 0;
}

window_t* ws_create_window(const char* title, int w, int h,
                          void* paint_cb, void* input_cb, void* mouse_cb)
{
    s_printf("[WS] Create: "); s_printf(title); s_printf("\n");

    int slot = -1;
    for(int i=0; i<MAX_WINDOWS; i++) {
        if(window_store[i].is_active == 0) { slot = i; break; }
    }
    if(slot == -1) return 0;

    window_t* win = &window_store[slot];
    memset(win, 0, sizeof(window_t));

    win->is_active = 1;
    win->id = next_win_id++;

    // Smart Cascade Positioning
    static int cascade_x = 40;
    static int cascade_y = 50;

    // Reset cascade if off screen
    if (cascade_x + w > 1024) cascade_x = 40;
    if (cascade_y + h > 700) cascade_y = 50;

    win->x = cascade_x;
    win->y = cascade_y;

    // Clamp size
    if (w > 1024) w = 800;
    if (h > 700) h = 600;

    cascade_x += 30;
    cascade_y += 30;

    win->width = w;
    win->height = h;
    win->min_w = 150;
    win->min_h = 100;

    if(title) strncpy(win->title, title, 63);

    win->paint_callback = paint_cb;
    win->input_callback = input_cb;
    win->mouse_callback = mouse_cb;
    win->is_visible = 1;
    win->is_focused = 1;

    z_add(win);
    return win;
}

void ws_destroy_window(window_t* win) {
    if(win && win->is_active) {
        z_remove(win);
        win->is_active = 0;
    }
}

window_t* ws_get_window_by_id(int id) {
    for(int i=0; i<MAX_WINDOWS; i++) {
        if(window_store[i].is_active && window_store[i].id == id) {
            return &window_store[i];
        }
    }
    return 0;
}

int ws_get_count() { return MAX_WINDOWS; }

window_t* ws_get_window_at_index(int idx) {
    if(idx < 0 || idx >= MAX_WINDOWS) return 0;
    return z_order[idx];
}

void ws_bring_to_front(window_t* win) {
    if(!win) return;

    // Unfocus others
    for(int i=0; i<MAX_WINDOWS; i++) {
        if(z_order[i]) z_order[i]->is_focused = 0;
    }
    win->is_focused = 1;
    active_win = win;

    z_remove(win);
    z_add(win);
}

void ws_handle_mouse(int x, int y, int button) {
    int handled = 0;

    // Reverse Z-order (Topmost first)
    for(int i = MAX_WINDOWS - 1; i >= 0; i--) {
        window_t* w = z_order[i];
        
        if(w && w->is_visible && w->is_active) {
            // Check Hit against Window Rect (x, y, w, h)
            if (x >= w->x && x < w->x + w->width &&
                y >= w->y && y < w->y + w->height) {
                
                // Bring to front
                if (button != 0) ws_bring_to_front(w);

                // Dispatch to App
                if(w->mouse_callback) {
                    // --- FIX: Subtract Header Height (30px) ---
                    // The app paints assuming (0,0) is start of content.
                    // Screen Y has the header. Content Y starts at Window Y + 30.
                    
                    int local_x = x - w->x;
                    int local_y = y - w->y - 30; // Adjust for title bar
                    
                    typedef void (*mcb)(int,int,int);
                    ((mcb)w->mouse_callback)(local_x, local_y, button);
                }
                
                handled = 1;
                break; // Click consumed
            }
        }
    }
}
