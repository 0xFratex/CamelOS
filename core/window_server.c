// core/window_server.c
#include "window_server.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"

// Import screen size
extern int screen_w;
extern int screen_h;

// Animation constants (in milliseconds)
#define WIN_ANIM_OPEN_DURATION  200
#define WIN_ANIM_CLOSE_DURATION 150
#define WIN_ANIM_SCALE_OPEN     0.8f   // Scale from 0.8 -> 1.0 on open
#define WIN_ANIM_SCALE_CLOSE    0.8f   // Scale from 1.0 -> 0.8 on close

// Timer ticks for animation timing
extern uint32_t timer_ticks;

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
    // Find the index first, then shift from that index
    int idx = -1;
    for(int i = 0; i < MAX_WINDOWS; i++) {
        if(z_order[i] == w) { idx = i; break; }
    }
    if(idx == -1) return;  // Not found
    // Shift remaining elements down
    for(int i = idx; i < MAX_WINDOWS - 1; i++) {
        z_order[i] = z_order[i+1];
    }
    z_order[MAX_WINDOWS-1] = 0;
}

window_t* ws_create_window(const char* title, int w, int h,
                          void* paint_cb, void* input_cb, void* mouse_cb)
{
    return ws_create_window_ex(title, -1, -1, w, h, WIN_STYLE_STANDARD,
                               paint_cb, input_cb, mouse_cb);
}

window_t* ws_create_window_ex(const char* title, int x, int y, int w, int h,
                              int style_flags, void* paint_cb, void* input_cb, void* mouse_cb)
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
    win->style_flags = style_flags;

    // Fullscreen style: override position and size to cover screen
    if (style_flags & WIN_STYLE_FULLSCREEN) {
        x = 0;
        y = 0;
        w = screen_w;
        h = screen_h;
    }

    // Position: use provided or cascade
    if (x >= 0 && y >= 0) {
        // Explicit position given
        win->x = x;
        win->y = y;
    } else {
        // Smart Cascade Positioning using actual screen dimensions
        static int cascade_x = 40;
        static int cascade_y = 50;

        // Reset cascade if off screen (use actual screen size)
        if (cascade_x + w > screen_w) cascade_x = 40;
        if (cascade_y + h > screen_h) cascade_y = 50;

        win->x = cascade_x;
        win->y = cascade_y;

        cascade_x += 30;
        cascade_y += 30;
    }

    // Clamp size to actual screen dimensions
    if (w > screen_w) w = screen_w - 40;
    if (h > screen_h) h = screen_h - 60;

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

    // Modal windows go in the popup layer
    if (style_flags & WIN_STYLE_MODAL) {
        win->layer = LAYER_POPUP;
    }

    // Start open animation (scale from 0.8 to 1.0 with fade-in)
    win->anim_state = WIN_ANIM_OPENING;
    win->anim_t = 0.0f;
    win->anim_start_x = win->x + (int)(w * (1.0f - WIN_ANIM_SCALE_OPEN) / 2);
    win->anim_start_y = win->y + (int)(h * (1.0f - WIN_ANIM_SCALE_OPEN) / 2);
    win->anim_start_w = (int)(w * WIN_ANIM_SCALE_OPEN);
    win->anim_start_h = (int)(h * WIN_ANIM_SCALE_OPEN);
    win->anim_end_x = win->x;
    win->anim_end_y = win->y;
    win->anim_end_w = w;
    win->anim_end_h = h;
    win->anim_start_time = timer_ticks;
    win->opacity = 0.0f;  // Start transparent for fade-in

    z_add(win);
    return win;
}

void ws_destroy_window(window_t* win) {
    if(win && win->is_active) {
        z_remove(win);
        win->is_active = 0;
    }
}

void ws_set_geometry(window_t* win, int x, int y, int w, int h) {
    if(!win) return;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
}

void ws_set_colors(window_t* win, uint32_t title_bar, uint32_t border, uint32_t bg) {
    if(!win) return;
    win->title_bar_color = title_bar;
    win->border_color = border;
    win->background_color = bg;
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

void ws_set_title(window_t* win, const char* title) {
    if(!win || !title) return;
    strncpy(win->title, title, 63);
    win->title[63] = 0;
}

void ws_set_opacity(window_t* win, float opacity) {
    if(!win) return;
    if(opacity < 0.0f) opacity = 0.0f;
    if(opacity > 1.0f) opacity = 1.0f;
    win->opacity = opacity;
}

void ws_close(window_t* win) {
    if(!win || !win->is_active) return;

    // Call close callback if registered
    if(win->close_callback) {
        typedef void (*close_cb)(window_t*);
        ((close_cb)win->close_callback)(win);
    }

    // Start close animation (scale from 1.0 to 0.8 with fade-out)
    win->anim_state = WIN_ANIM_CLOSING;
    win->anim_t = 0.0f;
    win->anim_start_x = win->x;
    win->anim_start_y = win->y;
    win->anim_start_w = win->width;
    win->anim_start_h = win->height;
    win->anim_end_x = win->x + (int)(win->width * (1.0f - WIN_ANIM_SCALE_CLOSE) / 2);
    win->anim_end_y = win->y + (int)(win->height * (1.0f - WIN_ANIM_SCALE_CLOSE) / 2);
    win->anim_end_w = (int)(win->width * WIN_ANIM_SCALE_CLOSE);
    win->anim_end_h = (int)(win->height * WIN_ANIM_SCALE_CLOSE);
    win->anim_start_time = timer_ticks;
    // Don't remove from z_order yet — animation will do that when complete
}

void ws_set_icon(window_t* win, const char* icon_name) {
    if(!win || !icon_name) return;
    strncpy(win->icon_name, icon_name, 31);
    win->icon_name[31] = 0;
}

void ws_center_window(window_t* win) {
    if(!win) return;
    win->x = (screen_w - win->width) / 2;
    win->y = (screen_h - win->height) / 2;
}

window_t* ws_get_active_window(void) {
    return active_win;
}

void ws_set_active_window(window_t* win) {
    if(!win) return;
    ws_bring_to_front(win);
}

// Update all window animations each frame
void ws_update_animations(void) {
    for(int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = &window_store[i];
        if(!w->is_active || w->anim_state == WIN_ANIM_NONE) continue;

        // Calculate elapsed time
        uint32_t elapsed = timer_ticks - w->anim_start_time;
        int duration = (w->anim_state == WIN_ANIM_OPENING)
                       ? WIN_ANIM_OPEN_DURATION
                       : WIN_ANIM_CLOSE_DURATION;

        // Convert timer_ticks (at ~50Hz = 20ms per tick) to ms
        uint32_t elapsed_ms = elapsed * 20;

        // Compute progress [0.0 .. 1.0]
        float t = (float)elapsed_ms / (float)duration;
        if(t > 1.0f) t = 1.0f;

        w->anim_t = t;

        if(w->anim_state == WIN_ANIM_OPENING) {
            // Ease-out interpolation for smooth open
            // Interpolate position and size from start to end
            w->x = w->anim_start_x + (int)((w->anim_end_x - w->anim_start_x) * t);
            w->y = w->anim_start_y + (int)((w->anim_end_y - w->anim_start_y) * t);
            w->width = w->anim_start_w + (int)((w->anim_end_w - w->anim_start_w) * t);
            w->height = w->anim_start_h + (int)((w->anim_end_h - w->anim_start_h) * t);
            // Fade in: opacity 0.0 -> 1.0
            w->opacity = t;

            if(t >= 1.0f) {
                // Animation complete — snap to final values
                w->anim_state = WIN_ANIM_NONE;
                w->x = w->anim_end_x;
                w->y = w->anim_end_y;
                w->width = w->anim_end_w;
                w->height = w->anim_end_h;
                w->opacity = 1.0f;
            }
        } else if(w->anim_state == WIN_ANIM_CLOSING) {
            // Ease-in interpolation for close
            w->x = w->anim_start_x + (int)((w->anim_end_x - w->anim_start_x) * t);
            w->y = w->anim_start_y + (int)((w->anim_end_y - w->anim_start_y) * t);
            w->width = w->anim_start_w + (int)((w->anim_end_w - w->anim_start_w) * t);
            w->height = w->anim_start_h + (int)((w->anim_end_h - w->anim_start_h) * t);
            // Fade out: opacity 1.0 -> 0.0
            w->opacity = 1.0f - t;

            if(t >= 1.0f) {
                // Close animation complete — destroy the window
                z_remove(w);
                w->is_active = 0;
                w->is_visible = 0;
                w->is_focused = 0;
                w->anim_state = WIN_ANIM_NONE;
                if(active_win == w) active_win = 0;
            }
        }
    }
}

void ws_start_animation(window_t* win, int anim_type, int duration_ms) {
    if(!win) return;
    win->anim_state = anim_type;
    win->anim_t = 0.0f;
    win->anim_start_time = timer_ticks;
    // Caller should set anim_start_* and anim_end_* before calling
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
                    
                    typedef void (*mcb)(window_t*,int,int,int);
                    ((mcb)w->mouse_callback)(w, local_x, local_y, button);
                }
                
                handled = 1;
                break; // Click consumed
            }
        }
    }
}
