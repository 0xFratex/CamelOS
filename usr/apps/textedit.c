// usr/apps/textedit.c - CamelOS TextEdit App
// Text editor with horizontal scrolling, arrow keys, file open/save support
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../dock.h"
#include "../../fs/pfs32.h"
#include "../../core/window_server.h"
#include "../lib/file_picker.h"

// Layout
#define PAD 8
#define CHAR_W 8
#define CHAR_H 16
#define TOOLBAR_H 32
#define LINE_NUM_W 40  // Width reserved for line numbers
#define TEXT_LEFT_PAD (LINE_NUM_W + PAD)  // Left offset for text content

// Text buffer
#define MAX_LINES 200
#define MAX_LINE_LEN 256
static char text_lines[MAX_LINES][MAX_LINE_LEN];
static int line_count = 1;
static int cursor_line = 0;
static int cursor_col = 0;
static int scroll_offset = 0;    // Vertical scroll (in lines)
static int hscroll_offset = 0;   // Horizontal scroll (in characters)

// File state
static char current_file[128] = "";
static int file_modified = 0;
static char status_msg[64] = "New File";

// Window reference for title updates
static Window* te_window = 0;

// File picker callback — opens the selected file in the editor
static void textedit_on_file_selected(const char* path) {
    if (path && path[0]) {
        te_open_file(path);
    }
}

// Prompt state (for file open/save path input)
static int prompt_active = 0;  // 0=none, 1=save, 2=open
static char prompt_buf[128];
static int prompt_len = 0;

static void te_update_title() {
    if (!te_window) return;
    char title[80];
    const char* fname = current_file[0] ? strrchr(current_file, '/') : 0;
    if (fname) fname++; else fname = current_file[0] ? current_file : "Untitled";
    strcpy(title, fname);
    if (file_modified) strcat(title, " *");
    ws_set_title((window_t*)te_window, title);
}

static void te_new() {
    for (int i = 0; i < MAX_LINES; i++) text_lines[i][0] = 0;
    line_count = 1;
    cursor_line = 0;
    cursor_col = 0;
    scroll_offset = 0;
    hscroll_offset = 0;
    current_file[0] = 0;
    file_modified = 0;
    strcpy(status_msg, "New File");
    te_update_title();
}

static void te_new_with_welcome() {
    te_new();
    // Insert welcome/instruction text
    strcpy(text_lines[0],  "Welcome to CamelOS TextEdit");
    strcpy(text_lines[1],  "");
    strcpy(text_lines[2],  "Keyboard Shortcuts:");
    strcpy(text_lines[3],  "  Arrow keys  - Move cursor");
    strcpy(text_lines[4],  "  Home/End    - Jump to start/end of line");
    strcpy(text_lines[5],  "  Enter       - Insert new line");
    strcpy(text_lines[6],  "  Backspace   - Delete character before cursor");
    strcpy(text_lines[7],  "  Delete      - Delete character after cursor");
    strcpy(text_lines[8],  "");
    strcpy(text_lines[9],  "Toolbar Buttons:");
    strcpy(text_lines[10], "  New   - Create a new empty document");
    strcpy(text_lines[11], "  Open  - Open a file from disk");
    strcpy(text_lines[12], "  Save  - Save the current file");
    strcpy(text_lines[13], "");
    strcpy(text_lines[14], "Scroll with mouse wheel or arrow keys.");
    strcpy(text_lines[15], "Start typing to replace this text!");
    line_count = 16;
    cursor_line = 15;
    cursor_col = strlen(text_lines[15]);
    strcpy(status_msg, "New File");
    te_update_title();
}

static void te_open_file(const char* path) {
    // Allocate on heap to avoid stack overflow (kernel stack is only 16KB,
    // and te_open_file can be called deep in the call chain from context menus)
    #define TE_OPEN_BUFSZ 16384
    char* buf = (char*)kmalloc(TE_OPEN_BUFSZ);
    if (!buf) {
        strcpy(status_msg, "Error: Out of memory");
        return;
    }
    int len = sys_fs_read(path, buf, TE_OPEN_BUFSZ - 1);
    if (len <= 0) {
        kfree(buf);
        strcpy(status_msg, "Error: Could not read file");
        return;
    }
    buf[len] = 0;
    
    te_new();
    strcpy(current_file, path);
    
    // Parse into lines
    int line = 0;
    int col = 0;
    for (int i = 0; i < len && line < MAX_LINES; i++) {
        if (buf[i] == '\n') {
            text_lines[line][col] = 0;
            line++;
            col = 0;
            if (line >= MAX_LINES) break;
        } else if (buf[i] == '\r') {
            // Skip CR
        } else {
            if (col < MAX_LINE_LEN - 1) {
                text_lines[line][col++] = buf[i];
            }
        }
    }
    text_lines[line][col] = 0;
    line_count = line + 1;
    
    // Extract filename for status
    const char* fname = strrchr(path, '/');
    if (fname) fname++; else fname = path;
    strncpy(status_msg, fname, sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = 0;
    
    file_modified = 0;
    te_update_title();
    kfree(buf);
}

static void te_save_file(const char* path) {
    // Build output buffer on heap to avoid stack overflow
    #define TE_SAVE_BUFSZ 16384
    char* buf = (char*)kmalloc(TE_SAVE_BUFSZ);
    if (!buf) {
        strcpy(status_msg, "Error: Out of memory!");
        return;
    }
    int pos = 0;
    for (int i = 0; i < line_count; i++) {
        int len = strlen(text_lines[i]);
        if (pos + len + 2 < TE_SAVE_BUFSZ) {
            memcpy(buf + pos, text_lines[i], len);
            pos += len;
            buf[pos++] = '\n';
        }
    }
    
    int res = sys_fs_write(path, buf, pos);
    if (res >= 0) {
        strcpy(current_file, path);
        file_modified = 0;
        const char* fname = strrchr(path, '/');
        if (fname) fname++; else fname = path;
        strncpy(status_msg, fname, sizeof(status_msg) - 1);
        status_msg[sizeof(status_msg) - 1] = 0;
        te_update_title();
    } else {
        strcpy(status_msg, "Error: Save failed!");
    }
    kfree(buf);
}

// Auto-scroll horizontally to keep cursor visible
static void te_ensure_cursor_visible(int w) {
    int max_visible_chars = (w - TEXT_LEFT_PAD - PAD) / CHAR_W;
    if (max_visible_chars < 1) max_visible_chars = 1;
    
    // If cursor is left of the visible area
    if (cursor_col < hscroll_offset) {
        hscroll_offset = cursor_col;
    }
    // If cursor is right of the visible area (with 2 char margin)
    if (cursor_col >= hscroll_offset + max_visible_chars - 2) {
        hscroll_offset = cursor_col - max_visible_chars + 3;
        if (hscroll_offset < 0) hscroll_offset = 0;
    }
}

static void textedit_on_paint(window_t* win, int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
    
    // Toolbar
    gfx_fill_rect(x, y, w, TOOLBAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + TOOLBAR_H - 1, w, 1, 0xFFC6C6C8);
    
    int bx = x + 6;
    // New button
    gfx_fill_rounded_rect(bx, y + 4, 36, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 4, y + 9, "New", 0xFF555555);
    bx += 40;
    // Open button
    gfx_fill_rounded_rect(bx, y + 4, 40, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 4, y + 9, "Open", 0xFF555555);
    bx += 44;
    // Save button
    gfx_fill_rounded_rect(bx, y + 4, 40, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 4, y + 9, "Save", 0xFF555555);
    bx += 44;
    
    // Status / filename
    gfx_draw_string(bx + 8, y + 9, status_msg, file_modified ? 0xFFCC0000 : 0xFF555555);
    if (file_modified) {
        gfx_draw_string(bx + 8 + strlen(status_msg) * 8 + 4, y + 9, " *", 0xFFCC0000);
    }
    
    // Text area
    int text_y_start = y + TOOLBAR_H + 4;
    int max_rows = (h - TOOLBAR_H - 4) / CHAR_H;
    int max_visible_chars = (w - TEXT_LEFT_PAD - PAD) / CHAR_W;
    if (max_visible_chars < 1) max_visible_chars = 1;
    
    // Line number gutter background
    gfx_fill_rect(x, text_y_start, LINE_NUM_W, h - TOOLBAR_H - 4, 0xFFF5F5F5);
    // Gutter separator line
    gfx_draw_line(x + LINE_NUM_W, text_y_start, x + LINE_NUM_W, text_y_start + max_rows * CHAR_H, 0xFFE0E0E0);
    
    // Line numbers (drawn first, in gutter area)
    for (int r = 0; r < max_rows && (r + scroll_offset) < line_count; r++) {
        char num[8];
        int_to_str(r + scroll_offset + 1, num);
        int num_w = strlen(num) * 8;
        // Right-align line numbers in the gutter
        gfx_draw_string(x + LINE_NUM_W - num_w - 6, text_y_start + r * CHAR_H, num, 0xFFAAAAAA);
    }
    
    // Text content (with horizontal scroll offset)
    for (int r = 0; r < max_rows && (r + scroll_offset) < line_count; r++) {
        int line_idx = r + scroll_offset;
        int len = strlen(text_lines[line_idx]);
        if (len > 0 && len > hscroll_offset) {
            // Draw only the visible portion of the line
            int draw_start = hscroll_offset;
            int draw_len = len - hscroll_offset;
            if (draw_len > max_visible_chars) draw_len = max_visible_chars;
            if (draw_len > 0) {
                char tmp[257];
                strncpy(tmp, text_lines[line_idx] + draw_start, draw_len);
                tmp[draw_len] = 0;
                gfx_draw_string(x + TEXT_LEFT_PAD, text_y_start + r * CHAR_H, tmp, 0xFF333333);
            }
        }
    }
    
    // Cursor
    if (cursor_line >= scroll_offset && cursor_line < scroll_offset + max_rows) {
        static int blink = 0; blink++;
        if (blink % 60 < 30) {
            int cy = text_y_start + (cursor_line - scroll_offset) * CHAR_H;
            int cx = x + TEXT_LEFT_PAD + (cursor_col - hscroll_offset) * CHAR_W;
            // Only draw cursor if it's in the visible horizontal range
            if (cursor_col >= hscroll_offset && cursor_col < hscroll_offset + max_visible_chars) {
                gfx_fill_rect(cx, cy, 2, CHAR_H, 0xFF007AFF);
            }
        }
    }
    
    // Horizontal scroll indicator
    if (hscroll_offset > 0) {
        // Show left arrow indicator
        gfx_draw_string(x + TEXT_LEFT_PAD - 8, text_y_start, "<", 0xFF007AFF);
    }
    
    // Prompt overlay
    if (prompt_active) {
        int pw = 300, ph = 60;
        int px = x + (w - pw) / 2;
        int py = y + h / 2 - 30;
        
        gfx_fill_rounded_rect(px, py, pw, ph, 0xFFFFFFFF, 8);
        gfx_draw_rect(px, py, pw, ph, 0xFF007AFF);
        
        const char* title = (prompt_active == 1) ? "Save As:" : "Open File:";
        gfx_draw_string(px + 10, py + 6, title, 0xFF333333);
        
        gfx_fill_rect(px + 10, py + 26, pw - 20, 22, 0xFFF2F2F7);
        gfx_draw_rect(px + 10, py + 26, pw - 20, 22, 0xFFC6C6C8);
        gfx_draw_string(px + 14, py + 30, prompt_buf, 0xFF333333);
        
        static int pb = 0; pb++;
        if (pb % 60 < 30) {
            gfx_fill_rect(px + 14 + prompt_len * 8, py + 30, 1, 14, 0xFF007AFF);
        }
    }
}

static void textedit_on_input(window_t* win, int key) {
    if (key == 0) return;
    
    // Prompt mode
    if (prompt_active) {
        if (key == '\n') {
            if (prompt_len > 0) {
                if (prompt_active == 1) te_save_file(prompt_buf);
                else te_open_file(prompt_buf);
            }
            prompt_active = 0;
        } else if (key == 27) {
            prompt_active = 0;
        } else if (key == '\b') {
            if (prompt_len > 0) prompt_buf[--prompt_len] = 0;
        } else if (key >= 32 && key != 127 && (key <= 126 || key >= 160) && prompt_len < 126) {
            prompt_buf[prompt_len++] = (char)key;
            prompt_buf[prompt_len] = 0;
        }
        return;
    }
    
    if (key == '\n') {
        // Insert new line
        if (line_count < MAX_LINES) {
            // Move lines down
            for (int i = line_count; i > cursor_line + 1; i--) {
                strcpy(text_lines[i], text_lines[i-1]);
            }
            // Split current line
            int col = cursor_col;
            int len = strlen(text_lines[cursor_line]);
            if (col < len) {
                strcpy(text_lines[cursor_line + 1], text_lines[cursor_line] + col);
                text_lines[cursor_line][col] = 0;
            } else {
                text_lines[cursor_line + 1][0] = 0;
            }
            line_count++;
            cursor_line++;
            cursor_col = 0;
            file_modified = 1;
        }
    } else if (key == '\b') {
        if (cursor_col > 0) {
            // Delete char before cursor
            int len = strlen(text_lines[cursor_line]);
            for (int i = cursor_col - 1; i < len; i++) {
                text_lines[cursor_line][i] = text_lines[cursor_line][i+1];
            }
            cursor_col--;
            file_modified = 1;
        } else if (cursor_line > 0) {
            // Join with previous line
            int prev_len = strlen(text_lines[cursor_line - 1]);
            int cur_len = strlen(text_lines[cursor_line]);
            if (prev_len + cur_len < MAX_LINE_LEN) {
                strcat(text_lines[cursor_line - 1], text_lines[cursor_line]);
            }
            cursor_col = prev_len;
            // Move lines up
            for (int i = cursor_line; i < line_count - 1; i++) {
                strcpy(text_lines[i], text_lines[i+1]);
            }
            line_count--;
            cursor_line--;
            file_modified = 1;
        }
    } else if (key == 127) { // Delete key
        int len = strlen(text_lines[cursor_line]);
        if (cursor_col < len) {
            for (int i = cursor_col; i < len; i++) {
                text_lines[cursor_line][i] = text_lines[cursor_line][i+1];
            }
            file_modified = 1;
        }
    } else if (key == 128) { // KEY_UP
        if (cursor_line > 0) {
            cursor_line--;
            int len = strlen(text_lines[cursor_line]);
            if (cursor_col > len) cursor_col = len;
        }
    } else if (key == 129) { // KEY_DOWN
        if (cursor_line < line_count - 1) {
            cursor_line++;
            int len = strlen(text_lines[cursor_line]);
            if (cursor_col > len) cursor_col = len;
        }
    } else if (key == 130) { // KEY_LEFT
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_line > 0) {
            cursor_line--;
            cursor_col = strlen(text_lines[cursor_line]);
        }
    } else if (key == 131) { // KEY_RIGHT
        int len = strlen(text_lines[cursor_line]);
        if (cursor_col < len) {
            cursor_col++;
        } else if (cursor_line < line_count - 1) {
            cursor_line++;
            cursor_col = 0;
        }
    } else if (key == 132) { // KEY_HOME
        cursor_col = 0;
    } else if (key == 133) { // KEY_END
        cursor_col = strlen(text_lines[cursor_line]);
    } else if (key >= 32 && key != 127 && (key <= 126 || key >= 160)) {
        // Insert character (ASCII printable + Latin-1 Supplement)
        int len = strlen(text_lines[cursor_line]);
        if (len < MAX_LINE_LEN - 1) {
            for (int i = len; i > cursor_col; i--) {
                text_lines[cursor_line][i] = text_lines[cursor_line][i-1];
            }
            text_lines[cursor_line][cursor_col] = (char)key;
            text_lines[cursor_line][len + 1] = 0;
            cursor_col++;
            file_modified = 1;
        }
    }
    
    // Adjust vertical scroll to keep cursor visible
    int win_h = te_window ? ((window_t*)te_window)->height : 380;
    int visible_lines = (win_h - TOOLBAR_H - 4) / CHAR_H;
    if (visible_lines < 1) visible_lines = 1;
    if (cursor_line < scroll_offset) scroll_offset = cursor_line;
    if (cursor_line >= scroll_offset + visible_lines) scroll_offset = cursor_line - visible_lines + 1;
    if (scroll_offset < 0) scroll_offset = 0;
    
    // Adjust horizontal scroll to keep cursor visible
    int win_w = te_window ? ((window_t*)te_window)->width : 500;
    te_ensure_cursor_visible(win_w);
}

static void textedit_on_mouse(window_t* win, int x, int y, int btn) {
    if (btn != 1) return;
    
    // Toolbar buttons
    if (y >= 0 && y < TOOLBAR_H) {
        int bx = 6;
        // New
        if (x >= bx && x < bx + 36) { te_new(); return; }
        bx += 40;
        // Open (launches file picker)
        if (x >= bx && x < bx + 40) {
            file_picker_open("/Users", textedit_on_file_selected);
            return;
        }
        bx += 44;
        // Save
        if (x >= bx && x < bx + 40) {
            if (current_file[0]) {
                te_save_file(current_file);
            } else {
                prompt_active = 1;
                prompt_buf[0] = 0;
                prompt_len = 0;
            }
            return;
        }
        return;
    }
    
    // Click in text area - position cursor
    if (y >= TOOLBAR_H + 4) {
        int clicked_line = (y - TOOLBAR_H - 4) / CHAR_H + scroll_offset;
        if (clicked_line >= 0 && clicked_line < line_count) {
            cursor_line = clicked_line;
            int col = (x - TEXT_LEFT_PAD) / CHAR_W + hscroll_offset;
            int len = strlen(text_lines[cursor_line]);
            if (col < 0) col = 0;
            if (col > len) col = len;
            cursor_col = col;
        }
    }
}

static void textedit_on_scroll(window_t* win, int delta) {
    scroll_offset -= delta * 3;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > line_count - 5) scroll_offset = line_count - 5;
    if (scroll_offset < 0) scroll_offset = 0;
}

static void textedit_on_hscroll(window_t* win, int delta) {
    hscroll_offset -= delta * 5;  // Scroll 5 characters at a time
    if (hscroll_offset < 0) hscroll_offset = 0;
    // Don't scroll past the longest line
    int max_len = 0;
    for (int i = 0; i < line_count; i++) {
        int len = strlen(text_lines[i]);
        if (len > max_len) max_len = len;
    }
    int max_visible = (te_window ? ((window_t*)te_window)->width : 500) - TEXT_LEFT_PAD - PAD;
    int max_hscroll = max_len - max_visible / CHAR_W + 2;
    if (max_hscroll < 0) max_hscroll = 0;
    if (hscroll_offset > max_hscroll) hscroll_offset = max_hscroll;
}

static void textedit_on_resize(window_t* win, int new_w, int new_h) {
    // Re-clamp horizontal scroll on resize
    te_ensure_cursor_visible(new_w);
}

// Menu action handler — dispatches menu bar clicks
static void textedit_on_menu_action(int menu_id, int item_idx) {
    if (menu_id == 0) {  // File menu
        if (item_idx == 0) {         // New
            te_new();
        } else if (item_idx == 1) {  // Open...
            file_picker_open("/Users", textedit_on_file_selected);
        } else if (item_idx == 2) {  // Save
            if (current_file[0]) {
                te_save_file(current_file);
            } else {
                prompt_active = 1;
                prompt_buf[0] = 0;
                prompt_len = 0;
            }
        }
    } else if (menu_id == 1) {  // Edit menu
        // Cut/Copy/Paste — no-op for now (clipboard integration TBD)
    }
}

void init_textedit_app() {
    // Check for launch arguments (file path passed via "Open With" or command line)
    char launch_path[256];
    launch_path[0] = 0;
    extern void wrap_get_args(char* b, int m);
    wrap_get_args(launch_path, sizeof(launch_path) - 1);
    if (launch_path[0] && sys_fs_exists(launch_path)) {
        te_new();
        te_open_file(launch_path);
    } else {
        te_new_with_welcome();
    }

    te_window = fw_create_window("TextEdit", 500, 380, textedit_on_paint, textedit_on_input, textedit_on_mouse);
    ((window_t*)te_window)->min_w = 300;
    
    // Wire up scroll, hscroll, and resize callbacks
    ((window_t*)te_window)->scroll_callback = (void*)textedit_on_scroll;
    ((window_t*)te_window)->hscroll_callback = (void*)textedit_on_hscroll;
    ((window_t*)te_window)->resize_callback = (void*)textedit_on_resize;
    
    te_update_title();
    
    ((window_t*)te_window)->menu_count = 3;
    strcpy(((window_t*)te_window)->menus[0].name, "File");
    strcpy(((window_t*)te_window)->menus[0].items[0].label, "New");
    strcpy(((window_t*)te_window)->menus[0].items[1].label, "Open...");
    strcpy(((window_t*)te_window)->menus[0].items[2].label, "Save");
    ((window_t*)te_window)->menus[0].item_count = 3;
    
    strcpy(((window_t*)te_window)->menus[1].name, "Edit");
    strcpy(((window_t*)te_window)->menus[1].items[0].label, "Cut");
    strcpy(((window_t*)te_window)->menus[1].items[1].label, "Copy");
    strcpy(((window_t*)te_window)->menus[1].items[2].label, "Paste");
    ((window_t*)te_window)->menus[1].item_count = 3;
    
    strcpy(((window_t*)te_window)->menus[2].name, "View");
    strcpy(((window_t*)te_window)->menus[2].items[0].label, "Word Wrap");
    ((window_t*)te_window)->menus[2].item_count = 1;
    
    // Wire up menu action handler so "Open..." and other menu items work
    ((window_t*)te_window)->on_menu_action = (void*)textedit_on_menu_action;
    
    fw_register_dock("TextEdit", 3, te_window);
}
