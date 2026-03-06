// usr/apps/textedit_cdl.c
#include "../../sys/cdl_defs.h"
#include "../../include/input_defs.h"
#include "../lib/camel_framework.h"

static kernel_api_t* sys = 0;

// UI Constants
#define C_BG        0xFFFFFFFF
#define C_TEXT      0xFF000000
#define C_TOOLBAR   0xFFE8E8E8
#define C_STATUS    0xFFD0D0D0
#define C_BORDER    0xFFAAAAAA
#define C_CURSOR    0xFF007AFF
#define TOOLBAR_H   40
#define STATUS_H    24
#define MARGIN      10
#define FONT_W      6
#define FONT_H      10

#define MAX_BUFFER  64000 
static char* doc_buffer = 0;
static int doc_len = 0;
static char current_path[128] = "Untitled.txt";
static int is_dirty = 0;

static int cursor_idx = 0;
static int scroll_y = 0; 
static int win_w = 600, win_h = 450; 
static int show_save_prompt = 0;

// --- Navigation State ---
static int preferred_cur_x = -1; // -1 means "use actual X", >= 0 means try to stick to this column

// --- Basic Edits ---
void doc_insert(char c) {
    if (!doc_buffer || doc_len >= MAX_BUFFER - 1) return;
    for (int i = doc_len; i > cursor_idx; i--) doc_buffer[i] = doc_buffer[i-1];
    doc_buffer[cursor_idx] = c;
    cursor_idx++; doc_len++; doc_buffer[doc_len] = 0;
    is_dirty = 1;
    preferred_cur_x = -1;
}

void doc_backspace() {
    if (!doc_buffer || cursor_idx <= 0) return;
    for (int i = cursor_idx; i < doc_len; i++) doc_buffer[i-1] = doc_buffer[i];
    cursor_idx--; doc_len--; doc_buffer[doc_len] = 0;
    is_dirty = 1;
    preferred_cur_x = -1;
}

void doc_delete() {
    if (!doc_buffer || cursor_idx >= doc_len) return;
    for (int i = cursor_idx; i < doc_len - 1; i++) doc_buffer[i] = doc_buffer[i+1];
    doc_len--; doc_buffer[doc_len] = 0;
    is_dirty = 1;
    preferred_cur_x = -1;
}

// --- Visual Navigation Logic ---

// Calculates the (x, y) coordinates of a specific index based on the *exact* same 
// wrapping logic used in on_paint.
void get_visual_pos_of_index(int target_idx, int* out_x, int* out_y) {
    int cx = MARGIN;
    int cy = MARGIN; 
    
    // We iterate 0 to target_idx-1. 
    // If target_idx is 0, loop doesn't run, x=MARGIN, y=MARGIN. Correct.
    for(int i=0; i<target_idx && i < doc_len; i++) {
        char c = doc_buffer[i]; 
        
        // Wrap Check
        if(c == '\n' || (cx > win_w - MARGIN - FONT_W)) {
            cy += FONT_H + 2; 
            cx = MARGIN;
            if (c == '\n') continue;
        } 
        
        cx += FONT_W;
    } 
    
    if (out_x) *out_x = cx;
    if (out_y) *out_y = cy;
}

// Scans the document to find the index that is physically closest to (target_x, target_y).
int get_index_from_visual_pos(int target_x, int target_y) {
    int cx = MARGIN;
    int cy = MARGIN;
    int best_idx = doc_len; // Default to end
    int min_diff = 100000;
    
    // Iterate *all* possible cursor positions (0 to doc_len inclusive)
    for(int i=0; i<=doc_len; i++) {
        
        // Is this the right line?
        if (cy == target_y) {
            int diff = (cx > target_x) ? (cx - target_x) : (target_x - cx);
            if (diff < min_diff) {
                min_diff = diff;
                best_idx = i;
            }
        } 
        else if (cy > target_y) {
            // We have passed the target line.
            // If we found a candidate on the target line, return it.
            if (min_diff < 100000) return best_idx;
            
            // If the target line was skipped (empty?), return the index that started the next line?
            // Actually, if we passed it without finding a match, it usually means the line didn't exist 
            // (e.g. clicking below text). Return current index or previous.
            return (i > 0) ? i - 1 : 0;
        }

        // Advance layout state for next char
        if (i < doc_len) {
            char c = doc_buffer[i];
            if(c == '\n' || (cx > win_w - MARGIN - FONT_W)) {
                cy += FONT_H + 2; 
                cx = MARGIN;
                if (c == '\n') continue;
            }
            cx += FONT_W;
        }
    } 
    
    // End of file reached
    return best_idx;
}

void ensure_visible() {
    int cx, cy;
    get_visual_pos_of_index(cursor_idx, &cx, &cy);
    int ta_h = win_h - TOOLBAR_H - STATUS_H; 
    
    if (cy - scroll_y < MARGIN) scroll_y = cy - MARGIN;
    if (cy - scroll_y > ta_h - FONT_H - MARGIN) scroll_y = cy - (ta_h - FONT_H - MARGIN);
    if (scroll_y < 0) scroll_y = 0;
}

// Move Up/Down visually
void move_vertical(int dir) {
    int cur_x, cur_y;
    get_visual_pos_of_index(cursor_idx, &cur_x, &cur_y); 
    
    // 1. Establish preferred X column if not set
    if (preferred_cur_x == -1) {
        preferred_cur_x = cur_x;
    } 
    
    // 2. Calculate target Y
    int line_h = FONT_H + 2;
    int target_y = cur_y + (dir * line_h); 
    
    // 3. Clamp Y (don't go above top margin)
    if (target_y < MARGIN) target_y = MARGIN; 
    
    // 4. Find best index
    int new_idx = get_index_from_visual_pos(preferred_cur_x, target_y); 
    
    cursor_idx = new_idx;
    ensure_visible();
}

// Move word by word (Ctrl+Left/Right)
void move_word(int dir) {
    if (dir < 0) { // Left
        if (cursor_idx > 0) cursor_idx--;
        // Consume whitespace
        while(cursor_idx > 0 && doc_buffer[cursor_idx] == ' ') cursor_idx--;
        // Consume word
        while(cursor_idx > 0 && doc_buffer[cursor_idx-1] != ' ' && doc_buffer[cursor_idx-1] != '\n') cursor_idx--;
    } else { // Right
        if (cursor_idx < doc_len) cursor_idx++;
        // Consume word
        while(cursor_idx < doc_len && doc_buffer[cursor_idx] != ' ' && doc_buffer[cursor_idx] != '\n') cursor_idx++;
        // Consume whitespace
        while(cursor_idx < doc_len && doc_buffer[cursor_idx] == ' ') cursor_idx++;
    }
    preferred_cur_x = -1; // Reset column memory
    ensure_visible();
}

// --- Inputs ---

void on_input(int key) {
    // Handle Dialogs first
    extern int cm_dialog_input(int);
    if (cm_dialog_input(key)) return;
    if (show_save_prompt) return;

    if (key == 0) return;

    int ctrl = 0, shift = 0, alt = 0;
    if (sys && sys->get_kbd_state) sys->get_kbd_state(&ctrl, &shift, &alt);

    switch(key) {
        case KEY_LEFT:
            if (ctrl) move_word(-1);
            else if(cursor_idx > 0) cursor_idx--;
            preferred_cur_x = -1;
            ensure_visible();
            break;
            
        case KEY_RIGHT:
            if (ctrl) move_word(1);
            else if(cursor_idx < doc_len) cursor_idx++;
            preferred_cur_x = -1;
            ensure_visible();
            break;
            
        case KEY_UP:
            move_vertical(-1);
            break;
            
        case KEY_DOWN:
            move_vertical(1);
            break;
            
        case KEY_HOME:
            // TODO: Visual Home vs Logical Home? 
            // Logical is safer for now.
            while(cursor_idx > 0 && doc_buffer[cursor_idx-1] != '\n') cursor_idx--;
            preferred_cur_x = -1;
            ensure_visible();
            break;
            
        case KEY_END:
            while(cursor_idx < doc_len && doc_buffer[cursor_idx] != '\n') cursor_idx++;
            preferred_cur_x = -1;
            ensure_visible();
            break;
            
        case KEY_PGUP:
            for(int i=0; i<10; i++) move_vertical(-1);
            break;
            
        case KEY_PGDN:
            for(int i=0; i<10; i++) move_vertical(1);
            break;
            
        case KEY_DELETE: doc_delete(); break;
        case '\b': doc_backspace(); break;
        case '\n': doc_insert('\n'); break;
        case '\t': doc_insert(' '); doc_insert(' '); break; // 2 spaces
        
        default:
            if (key >= 32 && key <= 126) {
                doc_insert((char)key);
            }
            break;
    }
}

// --- Painting ---

void on_paint(int x, int y, int w, int h) {
    if (!sys) return;
    win_w = w; win_h = h; 
    
    // Toolbar
    sys->draw_rect(x, y, w, TOOLBAR_H, C_TOOLBAR);
    sys->draw_rect(x, y + TOOLBAR_H - 1, w, 1, C_BORDER); 
    
    // Draw Buttons (New, Open, Save) - simplified for brevity
    int bx = x + 10; int by = y + 8;
    sys->draw_rect_rounded(bx, by, 60, 24, 0xFFFFFFFF, 4); sys->draw_text(bx+18, by+8, "New", C_TEXT); bx+=70;
    sys->draw_rect_rounded(bx, by, 60, 24, 0xFFFFFFFF, 4); sys->draw_text(bx+18, by+8, "Open", C_TEXT); bx+=70;
    sys->draw_rect_rounded(bx, by, 60, 24, 0xFFFFFFFF, 4); sys->draw_text(bx+18, by+8, "Save", C_TEXT); bx+=70;

    char title[160];
    sys->strcpy(title, current_path);
    if (is_dirty) sys->strcpy(title + sys->strlen(title), " *");
    sys->draw_text(bx + 20, by + 8, title, 0xFF555555);

    // Text Area
    int ta_y = y + TOOLBAR_H;
    int ta_h = h - TOOLBAR_H - STATUS_H;
    sys->draw_rect(x, ta_y, w, ta_h, C_BG); 
    
    // Calculate layout for drawing (identical logic to nav)
    int cx = MARGIN, cy = MARGIN - scroll_y;
    int cur_x = -1, cur_y = -1;
    
    if (doc_buffer) {
        for (int i = 0; i <= doc_len; i++) {
            // Capture cursor position before character rendering
            if (i == cursor_idx) { cur_x = cx; cur_y = cy; } 
            
            char c = doc_buffer[i]; 
            
            // Wrap logic matches get_visual_pos_of_index
            if (c == '\n' || (cx > w - MARGIN - FONT_W)) {
                cy += FONT_H + 2; 
                cx = MARGIN;
                if (c == '\n') continue;
            }
            if (c == 0) break; 
            
            // Draw if visible
            if (cy + ta_y >= ta_y && cy + ta_y < ta_y + ta_h) {
                if (c >= 32) { char t[2]={c,0}; sys->draw_text_clipped(x + cx, ta_y + cy, t, C_TEXT, w); } 
            }
            cx += FONT_W;
        }
    } 
    
    // Draw Blink Cursor
    if (cur_x != -1 && cur_y + ta_y >= ta_y && cur_y + ta_y < ta_y + ta_h) {
        static int blink = 0; blink++;
        if ((blink / 20) % 2) {
            sys->draw_rect(x + cur_x, ta_y + cur_y, 2, FONT_H, C_CURSOR);
        }
    }

    // Status Bar
    int st_y = y + h - STATUS_H;
    sys->draw_rect(x, st_y, w, STATUS_H, C_STATUS);
    char stats[64]; 
    char num[12]; sys->itoa(doc_len, num);
    sys->strcpy(stats, "Length: "); sys->strcpy(stats+8, num);
    sys->draw_text(x + 10, st_y + 8, stats, 0xFF444444); 
    
    // Dialogs
    extern int cm_dialog_render(int, int, int, int);
    cm_dialog_render(x, y, w, h); 
    
    // (Save prompt rendering logic omitted for brevity, essentially the same as before)
}

// ... Rest of file (on_mouse, file ops, main) kept as is or minimally adapted ...
// Forward declarations for dialog callbacks required
void on_file_picked_open(const char* path) {
    if (!sys || !doc_buffer) return;
    sys->memset(doc_buffer, 0, MAX_BUFFER);
    int len = sys->fs_read(path, doc_buffer, MAX_BUFFER - 1);
    if (len >= 0) {
        doc_len = len; doc_buffer[doc_len] = 0;
        sys->strcpy(current_path, path);
        cursor_idx = 0; scroll_y = 0; is_dirty = 0;
    }
}
void on_file_picked_save(const char* path) {
    if (!sys) return;
    if (!sys->fs_exists(path)) sys->fs_create(path, 0);
    sys->fs_write(path, doc_buffer, doc_len);
    sys->strcpy(current_path, path);
    is_dirty = 0;
    if (show_save_prompt) sys->exit();
}
void file_open_action() { cm_dialog_open("Open File", "/home", "*", on_file_picked_open); }
void file_save_action() {
    if (sys->strcmp(current_path, "Untitled.txt") == 0) cm_dialog_save("Save As", "/home", "New.txt", ".txt", on_file_picked_save);
    else {
        if (!sys->fs_exists(current_path)) sys->fs_create(current_path, 0);
        sys->fs_write(current_path, doc_buffer, doc_len);
        is_dirty = 0;
        if (show_save_prompt) sys->exit();
    }
}
void file_new_action() { doc_len=0; doc_buffer[0]=0; cursor_idx=0; is_dirty=0; sys->strcpy(current_path, "Untitled.txt"); }

void on_mouse(int x, int y, int btn) {
    // Toolbar hits
    if (y >= 0 && y <= TOOLBAR_H && btn == 1) {
        if (x >= 10 && x <= 70) { file_new_action(); return; }
        if (x >= 80 && x <= 140) { file_open_action(); return; }
        if (x >= 150 && x <= 210) { file_save_action(); return; }
    }
    // Click to move cursor
    if (y > TOOLBAR_H && y < win_h - STATUS_H && btn == 1) {
        int ta_y = TOOLBAR_H;
        int click_x = x - MARGIN; // Relative to content x
        int click_y = (y - ta_y) + scroll_y; // Relative to document y
        
        // Snap Y to line grid
        int line_h = FONT_H + 2;
        int snap_y = (click_y / line_h) * line_h + MARGIN;
        
        // Find index
        cursor_idx = get_index_from_visual_pos(x, snap_y); // Use x directly as get_index uses window coords logic
        preferred_cur_x = -1;
    }
    extern int cm_dialog_click(int, int, int, int);
    cm_dialog_click(win_w, win_h, x, y);
}

void menu_cb(int menu, int item) {
    if (menu == 0) { 
        if (item == 0) file_new_action();
        if (item == 1) file_open_action();
        if (item == 2) file_save_action();
        if (item == 3) sys->exit();
    }
}

static cdl_exports_t exports = { .lib_name = "TextEdit", .version = 4 };
cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    doc_buffer = (char*)sys->malloc(MAX_BUFFER);
    if(doc_buffer) sys->memset(doc_buffer, 0, MAX_BUFFER); 
    
    extern void cm_init(kernel_api_t*); cm_init(api);
    extern void cm_dialog_init(); cm_dialog_init();

    char args[256]; sys->get_launch_args(args, 256);
    if(sys->strlen(args)>0) on_file_picked_open(args);

    void* win = sys->create_window("TextEdit", 600, 450, on_paint, on_input, on_mouse);
    static menu_def_t menus[2];
    sys->strcpy(menus[0].name, "File");
    menus[0].item_count = 4;
    sys->strcpy(menus[0].items[0].label, "New");
    sys->strcpy(menus[0].items[1].label, "Open");
    sys->strcpy(menus[0].items[2].label, "Save");
    sys->strcpy(menus[0].items[3].label, "Quit");
    sys->set_window_menu(win, menus, 1, menu_cb);
    return &exports;
}
