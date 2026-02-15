#include "app_switcher.h"
#include "window_server.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "../kernel/assets.h"

static int switcher_active = 0;
static int selected_index = 0;
static int app_count = 0;
static window_t* app_list[MAX_WINDOWS];

void app_switcher_reset() {
    app_count = 0;
    // Collect all unique application windows (filtering helpers/dialogs if needed)
    // We iterate backwards (Z-order top to bottom) to find recent apps
    for (int i = ws_get_count() - 1; i >= 0; i--) {
        window_t* w = ws_get_window_at_index(i);
        if (w && w->is_visible && w->layer == LAYER_NORMAL) {
            app_list[app_count++] = w;
        }
    }
    // If active, select the second one (previous app), else first
    selected_index = (app_count > 1) ? 1 : 0;
}

void app_switcher_next() {
    if (app_count == 0) return;
    selected_index = (selected_index + 1) % app_count;
}

void app_switcher_prev() {
    if (app_count == 0) return;
    selected_index--;
    if (selected_index < 0) selected_index = app_count - 1;
}

void app_switcher_render(int screen_w, int screen_h) {
    if (!switcher_active || app_count == 0) return;

    int icon_size = 64;
    int padding = 20;
    int box_w = (icon_size + padding) * app_count + padding;
    int box_h = 120;
    int box_x = (screen_w - box_w) / 2;
    int box_y = (screen_h - box_h) / 2;

    // Draw Background (Glassy Dark)
    gfx_fill_rounded_rect(box_x, box_y, box_w, box_h, 0xC0202020, 15);

    // Draw Icons
    for (int i = 0; i < app_count; i++) {
        int ix = box_x + padding + i * (icon_size + padding);
        int iy = box_y + padding;

        // Highlight Selection
        if (i == selected_index) {
            gfx_fill_rounded_rect(ix - 5, iy - 5, icon_size + 10, icon_size + 10, 0x60FFFFFF, 8);
        }

        // Draw Icon (Use generic if not set)
        // Assume cm_draw_image is available or use gfx directly
        const char* icon = "terminal"; // Default
        if (strstr(app_list[i]->title, "Finder")) icon = "folder";
        // gfx_draw_asset_scaled(0, ix, iy, ..., icon_size, icon_size...);

        // Draw Label if selected
        if (i == selected_index) {
            // Center text below
            int tw = strlen(app_list[i]->title) * 8; // approx
            int tx = ix + (icon_size - tw) / 2;
            gfx_draw_string(tx, iy + icon_size + 15, app_list[i]->title, 0xFFFFFFFF);
        }
    }
}

void app_switcher_handle_key(int key_code, int ctrl_down, int shift_down) {
    // CMD/CTRL is held down externally
    if (key_code == 15) { // TAB
        if (!switcher_active) {
            switcher_active = 1;
            app_switcher_reset();
        } else {
            if (shift_down) app_switcher_prev();
            else app_switcher_next();
        }
    }
}

void app_switcher_release() {
    if (switcher_active && app_count > 0) {
        ws_bring_to_front(app_list[selected_index]);
        // Unminimize if needed
        if (app_list[selected_index]->state == WIN_STATE_MINIMIZED) {
            app_list[selected_index]->state = WIN_STATE_NORMAL;
            app_list[selected_index]->anim_state = 4; // Restore anim
            app_list[selected_index]->anim_t = 0.0f;
        }
    }
    switcher_active = 0;
}

int app_switcher_is_active() { return switcher_active; }