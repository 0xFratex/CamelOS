// usr/lib/file_picker.c - File Picker Dialog Implementation
// A modal "Choose From" file picker that can be opened from any app
#include "file_picker.h"
#include "../framework.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/drivers/serial.h"

// Layout constants
#define FP_ROW_H       24     // Row height in file list
#define FP_PATH_BAR_H  32     // Path bar height
#define FP_BTN_BAR_H   44     // Button bar height
#define FP_WIN_W       420    // Default window width
#define FP_WIN_H       380    // Default window height
#define FP_MARGIN       8
#define FP_DBLCLICK_FRAMES 30

// Forward declarations
static void fp_on_paint(window_t* win, int x, int y, int w, int h);
static void fp_on_mouse(window_t* win, int mx, int my, int btn);
static void fp_on_input(window_t* win, int key);
static void fp_on_scroll(window_t* win, int delta);
static void fp_on_close(window_t* win);
static void fp_refresh(void);
static void fp_navigate_to(const char* path);
static void fp_go_up(void);

// Global picker state
file_picker_state_t g_file_picker;

// External image drawing
extern void cm_draw_image(uint32_t* buffer, const char* name, int x, int y, int req_w, int req_h);

// ============================================================================
// Open the file picker
// ============================================================================
void file_picker_open(const char* start_path, file_picker_callback_t on_selected) {
    // If already active, close the old one first
    if (g_file_picker.active && g_file_picker.window) {
        window_t* w = g_file_picker.window;
        if (w->close_callback) {
            typedef void (*close_cb)(window_t*);
            ((close_cb)w->close_callback)(w);
        }
    }

    // Initialize state
    memset(&g_file_picker, 0, sizeof(g_file_picker));
    g_file_picker.active = 1;
    g_file_picker.selected_idx = -1;
    g_file_picker.last_click_idx = -1;
    g_file_picker.win_w = FP_WIN_W;
    g_file_picker.win_h = FP_WIN_H;
    g_file_picker.on_selected = on_selected;

    if (start_path && start_path[0]) {
        strncpy(g_file_picker.current_path, start_path, sizeof(g_file_picker.current_path) - 1);
    } else {
        strcpy(g_file_picker.current_path, "/");
    }

    // Create modal window
    g_file_picker.window = ws_create_window_ex(
        "Choose From",
        200, 150, FP_WIN_W, FP_WIN_H,
        WIN_STYLE_MODAL | WIN_STYLE_TOOL_WINDOW,
        (void*)fp_on_paint,
        (void*)fp_on_input,
        (void*)fp_on_mouse
    );

    if (!g_file_picker.window) {
        g_file_picker.active = 0;
        s_printf("[FilePicker] Failed to create window\n");
        return;
    }

    window_t* w = g_file_picker.window;
    w->min_w = 300;
    w->min_h = 250;
    w->scroll_callback = (void*)fp_on_scroll;
    w->close_callback = (void*)fp_on_close;
    ws_center_window(w);

    // Load initial directory
    fp_refresh();

    s_printf("[FilePicker] Opened at: ");
    s_printf("%s\n", g_file_picker.current_path);
}

// ============================================================================
// Close the file picker
// ============================================================================
void file_picker_close(void) {
    if (!g_file_picker.active) return;
    g_file_picker.active = 0;
    g_file_picker.on_selected = NULL;
}

// ============================================================================
// Refresh directory listing
// ============================================================================
static void fp_refresh(void) {
    if (!g_file_picker.current_path[0]) return;

    memset(g_file_picker.entries, 0, sizeof(g_file_picker.entries));
    g_file_picker.entry_count = 0;
    g_file_picker.selected_idx = -1;
    g_file_picker.scroll_offset = 0;

    pfs32_direntry_t temp[64];
    int raw = sys_fs_list_dir(g_file_picker.current_path, temp, 64);

    for (int i = 0; i < raw && g_file_picker.entry_count < 64; i++) {
        if (temp[i].filename[0] != 0 &&
            strcmp(temp[i].filename, ".") != 0 &&
            strcmp(temp[i].filename, "..") != 0) {
            g_file_picker.entries[g_file_picker.entry_count++] = temp[i];
        }
    }
}

// ============================================================================
// Navigate to a new directory path
// ============================================================================
static void fp_navigate_to(const char* path) {
    strncpy(g_file_picker.current_path, path, sizeof(g_file_picker.current_path) - 1);
    g_file_picker.current_path[sizeof(g_file_picker.current_path) - 1] = 0;
    fp_refresh();
}

// ============================================================================
// Go up one directory level
// ============================================================================
static void fp_go_up(void) {
    char* last_slash = strrchr(g_file_picker.current_path, '/');
    if (last_slash && last_slash != g_file_picker.current_path) {
        *last_slash = 0;
    } else if (last_slash == g_file_picker.current_path) {
        strcpy(g_file_picker.current_path, "/");
    }
    fp_refresh();
}

// ============================================================================
// Build full path for an entry
// ============================================================================
static void fp_build_path(char* buf, int buf_size, const char* dir, const char* name) {
    int dlen = strlen(dir);
    if (dlen > 0 && dir[dlen-1] == '/') {
        snprintf(buf, buf_size, "%s%s", dir, name);
    } else {
        snprintf(buf, buf_size, "%s/%s", dir, name);
    }
}

// ============================================================================
// Paint callback
// ============================================================================
static void fp_on_paint(window_t* win, int x, int y, int w, int h) {
    (void)win;

    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);

    // ---- Path Bar ----
    gfx_fill_rect(x, y, w, FP_PATH_BAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + FP_PATH_BAR_H - 1, w, 1, 0xFFC6C6C8);

    // Up button
    gfx_fill_rounded_rect(x + FP_MARGIN, y + 4, 28, 24, 0xFFE5E5EA, 4);
    gfx_draw_string(x + FP_MARGIN + 8, y + 10, "^", 0xFF333333);

    // Current path display
    char display_path[64];
    int plen = strlen(g_file_picker.current_path);
    if (plen > 40) {
        strcpy(display_path, "...");
        strcat(display_path, g_file_picker.current_path + plen - 37);
    } else {
        strncpy(display_path, g_file_picker.current_path, sizeof(display_path) - 1);
        display_path[sizeof(display_path) - 1] = 0;
    }
    gfx_draw_string(x + 44, y + 10, display_path, 0xFF333333);

    // ---- File List ----
    int list_y = y + FP_PATH_BAR_H;
    int list_h = h - FP_PATH_BAR_H - FP_BTN_BAR_H;
    int visible_rows = list_h / FP_ROW_H;

    // Clip to list area
    gfx_set_clip(x, list_y, w, list_h);

    for (int i = 0; i < g_file_picker.entry_count; i++) {
        int row_y = list_y + (i * FP_ROW_H) - g_file_picker.scroll_offset;

        // Skip if not visible
        if (row_y + FP_ROW_H < list_y || row_y > list_y + list_h) continue;

        // Selection highlight
        if (i == g_file_picker.selected_idx) {
            gfx_fill_rect(x, row_y, w, FP_ROW_H, 0xFF007AFF);
        } else if (i % 2 == 1) {
            gfx_fill_rect(x, row_y, w, FP_ROW_H, 0xFFF8F8FC);
        }

        // Icon
        const char* icon = "file";
        int elen = strlen(g_file_picker.entries[i].filename);
        if (g_file_picker.entries[i].attributes & 0x10) icon = "folder";
        if (elen > 4 && strcmp(g_file_picker.entries[i].filename + elen - 4, ".app") == 0) icon = "terminal";
        if (elen > 4 && strcmp(g_file_picker.entries[i].filename + elen - 4, ".cdl") == 0) icon = "terminal";
        if (elen > 4 && strcmp(g_file_picker.entries[i].filename + elen - 4, ".dmg") == 0) icon = "hdd_icon";

        cm_draw_image(0, icon, x + FP_MARGIN, row_y + 4, 16, 16);

        // Filename
        uint32_t text_color = (i == g_file_picker.selected_idx) ? 0xFFFFFFFF : 0xFF333333;
        gfx_draw_string(x + FP_MARGIN + 22, row_y + 6, g_file_picker.entries[i].filename, text_color);
    }

    gfx_reset_clip();

    // Scrollbar
    if (g_file_picker.entry_count > visible_rows) {
        int sb_x = x + w - 12;
        int sb_h = list_h - 4;
        int total_h = g_file_picker.entry_count * FP_ROW_H;
        int thumb_h = (list_h * list_h) / total_h;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int max_scroll = total_h - list_h;
        int thumb_y = list_y + 2;
        if (max_scroll > 0) {
            thumb_y = list_y + 2 + (g_file_picker.scroll_offset * (sb_h - thumb_h)) / max_scroll;
        }
        if (thumb_y < list_y + 2) thumb_y = list_y + 2;
        if (thumb_y + thumb_h > list_y + 2 + sb_h) thumb_y = list_y + 2 + sb_h - thumb_h;
        gfx_fill_rect(sb_x, list_y + 2, 8, sb_h, 0x20C0C0C0);
        gfx_fill_rounded_rect(sb_x + 1, thumb_y, 6, thumb_h, 0xFFC0C0C0, 3);
    }

    // ---- Button Bar ----
    int btn_y = y + h - FP_BTN_BAR_H;
    gfx_fill_rect(x, btn_y, w, FP_BTN_BAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, btn_y, w, 1, 0xFFC6C6C8);

    // Cancel button
    int cancel_x = x + w - FP_MARGIN - 180;
    gfx_fill_rounded_rect(cancel_x, btn_y + 8, 80, 28, 0xFFE5E5EA, 6);
    gfx_draw_string(cancel_x + 20, btn_y + 15, "Cancel", 0xFF333333);

    // Open button
    int open_x = x + w - FP_MARGIN - 90;
    int can_open = (g_file_picker.selected_idx >= 0);
    uint32_t open_col = can_open ? 0xFF007AFF : 0xFFC0C0C0;
    gfx_fill_rounded_rect(open_x, btn_y + 8, 80, 28, open_col, 6);
    gfx_draw_string(open_x + 24, btn_y + 15, "Open", 0xFFFFFFFF);
}

// ============================================================================
// Mouse callback
// ============================================================================
static void fp_on_mouse(window_t* win, int mx, int my, int btn) {
    (void)win;
    if (!g_file_picker.active) return;

    int w = g_file_picker.win_w;
    int h = g_file_picker.win_h;
    int list_y = FP_PATH_BAR_H;
    int list_h = h - FP_PATH_BAR_H - FP_BTN_BAR_H;
    int btn_y = h - FP_BTN_BAR_H;

    if (btn == 0) {
        // Release - reset drag state if any
        return;
    }

    if (btn == 2) {
        // Right click - ignore
        return;
    }

    // Left click
    if (btn != 1) return;

    // ---- Path Bar ----
    if (my >= 0 && my < FP_PATH_BAR_H) {
        // Up button
        if (mx >= FP_MARGIN && mx < FP_MARGIN + 28) {
            fp_go_up();
            return;
        }
        return;
    }

    // ---- File List ----
    if (my >= list_y && my < list_y + list_h) {
        int row_idx = (my - list_y + g_file_picker.scroll_offset) / FP_ROW_H;
        if (row_idx >= 0 && row_idx < g_file_picker.entry_count) {
            g_file_picker.frame_counter++;

            // Double-click detection
            int is_dblclick = 0;
            if (g_file_picker.last_click_idx == row_idx &&
                (g_file_picker.frame_counter - g_file_picker.last_click_frame) < FP_DBLCLICK_FRAMES) {
                is_dblclick = 1;
            }
            g_file_picker.last_click_idx = row_idx;
            g_file_picker.last_click_frame = g_file_picker.frame_counter;

            g_file_picker.selected_idx = row_idx;

            if (is_dblclick) {
                // Double-click: navigate into folder or select file
                pfs32_direntry_t* entry = &g_file_picker.entries[row_idx];
                if (entry->attributes & 0x10) {
                    // Navigate into folder
                    char new_path[256];
                    fp_build_path(new_path, sizeof(new_path), g_file_picker.current_path, entry->filename);
                    fp_navigate_to(new_path);
                } else {
                    // File selected - invoke callback and close
                    char full_path[256];
                    fp_build_path(full_path, sizeof(full_path), g_file_picker.current_path, entry->filename);

                    file_picker_callback_t cb = g_file_picker.on_selected;
                    g_file_picker.active = 0;

                    // Close the window
                    window_t* w = g_file_picker.window;
                    if (w) w->anim_state = 2; // Start close animation

                    // Invoke callback after closing
                    if (cb) cb(full_path);
                }
            }
        }
        return;
    }

    // ---- Button Bar ----
    if (my >= btn_y && my < btn_y + FP_BTN_BAR_H) {
        int cancel_x = w - FP_MARGIN - 180;
        int open_x = w - FP_MARGIN - 90;

        // Cancel button
        if (mx >= cancel_x && mx < cancel_x + 80) {
            g_file_picker.active = 0;
            g_file_picker.on_selected = NULL;
            window_t* ww = g_file_picker.window;
            if (ww) ww->anim_state = 2;
            return;
        }

        // Open button
        if (mx >= open_x && mx < open_x + 80 && g_file_picker.selected_idx >= 0) {
            pfs32_direntry_t* entry = &g_file_picker.entries[g_file_picker.selected_idx];
            char full_path[256];
            fp_build_path(full_path, sizeof(full_path), g_file_picker.current_path, entry->filename);

            file_picker_callback_t cb = g_file_picker.on_selected;
            g_file_picker.active = 0;

            window_t* w = g_file_picker.window;
            if (w) w->anim_state = 2;

            if (cb) cb(full_path);
            return;
        }
    }
}

// ============================================================================
// Input callback
// ============================================================================
static void fp_on_input(window_t* win, int key) {
    (void)win;
    if (!g_file_picker.active) return;

    if (key == 27) {
        // Escape - Cancel
        g_file_picker.active = 0;
        g_file_picker.on_selected = NULL;
        window_t* w = g_file_picker.window;
        if (w) w->anim_state = 2;
        return;
    }

    if (key == '\n' || key == '\r') {
        // Enter - Open selected or navigate into folder
        if (g_file_picker.selected_idx >= 0 && g_file_picker.selected_idx < g_file_picker.entry_count) {
            pfs32_direntry_t* entry = &g_file_picker.entries[g_file_picker.selected_idx];
            if (entry->attributes & 0x10) {
                char new_path[256];
                fp_build_path(new_path, sizeof(new_path), g_file_picker.current_path, entry->filename);
                fp_navigate_to(new_path);
            } else {
                char full_path[256];
                fp_build_path(full_path, sizeof(full_path), g_file_picker.current_path, entry->filename);

                file_picker_callback_t cb = g_file_picker.on_selected;
                g_file_picker.active = 0;
                window_t* w = g_file_picker.window;
                if (w) w->anim_state = 2;
                if (cb) cb(full_path);
            }
        }
        return;
    }

    if (key == '\b') {
        // Backspace - Go up
        fp_go_up();
        return;
    }

    // Arrow keys for navigation
    if (key == 0x4800 || key == 0x48) {
        // Up arrow
        if (g_file_picker.selected_idx > 0) {
            g_file_picker.selected_idx--;
        }
        return;
    }
    if (key == 0x5000 || key == 0x50) {
        // Down arrow
        if (g_file_picker.selected_idx < g_file_picker.entry_count - 1) {
            g_file_picker.selected_idx++;
        }
        return;
    }
}

// ============================================================================
// Scroll callback
// ============================================================================
static void fp_on_scroll(window_t* win, int delta) {
    (void)win;
    g_file_picker.scroll_offset += delta * FP_ROW_H * 2;
    if (g_file_picker.scroll_offset < 0) g_file_picker.scroll_offset = 0;

    int list_h = g_file_picker.win_h - FP_PATH_BAR_H - FP_BTN_BAR_H;
    int max_scroll = (g_file_picker.entry_count * FP_ROW_H) - list_h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_file_picker.scroll_offset > max_scroll) g_file_picker.scroll_offset = max_scroll;
}

// ============================================================================
// Close callback
// ============================================================================
static void fp_on_close(window_t* win) {
    (void)win;
    g_file_picker.active = 0;
}
