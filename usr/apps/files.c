// usr/apps/files.c - CamelOS Finder/Files App
// Full-featured file manager with navigation, context menus, and proper icon grid
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../clipboard.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/video/gfx_hal.h"
#include "../../core/window_server.h"
#include "../dock.h"

// Forward declaration for framework image drawing
extern void cm_draw_image(uint32_t* buffer, const char* name, int x, int y, int req_w, int req_h);

// ===== Layout Constants =====
#define CHAR_W        8       // Character width in pixels
#define ICON_SIZE     32      // Icon image size
#define ICON_PAD_X    18      // Horizontal padding around icon
#define ICON_PAD_Y    10      // Top padding above icon
#define LABEL_H       28      // Height for label below icon
#define CELL_W        (ICON_SIZE + ICON_PAD_X * 2)   // 68px per cell
#define CELL_H        (ICON_SIZE + ICON_PAD_Y + LABEL_H)  // 70px per cell
#define TOOLBAR_H     32      // Toolbar height at top
#define MARGIN_LEFT   8
#define MARGIN_TOP    (TOOLBAR_H + 4)

// Scrollbar constants
#define SCROLLBAR_W    14     // Scrollbar width in pixels
#define SCROLLBAR_MIN_H 20    // Minimum thumb height
#define SCROLLBAR_PAD   2     // Padding inside scrollbar track

// Path size limit
#define FM_PATH_MAX 128

// ===== Per-Instance Data =====
// Each Finder window gets its own directory path and navigation history
#define MAX_FM_INSTANCES 8
#define FM_HISTORY_SIZE 16
typedef struct {
    int active;
    int window_id;
    char path[FM_PATH_MAX];
    char history[FM_HISTORY_SIZE][FM_PATH_MAX];
    int hist_count;
    int hist_pos;
} FMInstance;

static FMInstance fm_instances[MAX_FM_INSTANCES];
static FMInstance* fm_cur = 0;

static FMInstance* fm_find_instance(int window_id) {
    for (int i = 0; i < MAX_FM_INSTANCES; i++) {
        if (fm_instances[i].active && fm_instances[i].window_id == window_id)
            return &fm_instances[i];
    }
    return 0;
}

static FMInstance* fm_alloc_instance(int window_id) {
    for (int i = 0; i < MAX_FM_INSTANCES; i++) {
        if (!fm_instances[i].active) {
            memset(&fm_instances[i], 0, sizeof(FMInstance));
            fm_instances[i].active = 1;
            fm_instances[i].window_id = window_id;
            strcpy(fm_instances[i].path, "/");
            return &fm_instances[i];
        }
    }
    return 0;
}

// Set the current instance from the active window
static void fm_set_current(void) {
    extern window_t* active_win;
    if (active_win) {
        FMInstance* inst = fm_find_instance(active_win->id);
        if (inst) { fm_cur = inst; return; }
    }
    for (int i = 0; i < MAX_FM_INSTANCES; i++) {
        if (fm_instances[i].active) { fm_cur = &fm_instances[i]; return; }
    }
}

// Safe path builder: appends "/" + name to base, with bounds checking
// Returns 1 on success, 0 if the result would overflow
static int fm_build_path(char* buf, int buf_size, const char* base, const char* name) {
    int blen = strlen(base);
    int nlen = strlen(name);
    int need = blen + (strcmp(base, "/") != 0 ? 1 : 0) + nlen + 1;
    if (need > buf_size) return 0;
    strcpy(buf, base);
    if (strcmp(base, "/") != 0) strcat(buf, "/");
    strcat(buf, name);
    return 1;
}

// ===== Globals (shared across instances for scroll, selection, etc.) =====
char fm_path[FM_PATH_MAX] = "/";
pfs32_direntry_t last_entries[64];
int is_selected[64];
int last_count = 0;
int scroll_offset = 0;
int hscroll_offset = 0;

// Context Menu State
int ctx_active = 0;
int ctx_x = 0, ctx_y = 0;
int ctx_type = 0;       // 0=Background, 1=File/Folder
int ctx_target_idx = -1;

// Prompt State (for new folder/file name input)
int prompt_active = 0;
int prompt_is_dir = 0;   // 1=creating folder, 0=creating file
char prompt_buffer[40];
int prompt_len = 0;

// Scrollbar drag state
static int sb_dragging = 0;      // 1 when user is dragging the scrollbar thumb
static int sb_drag_start_y = 0;  // Mouse Y when drag started
static int sb_drag_start_offset = 0; // scroll_offset when drag started

// Tracked window dimensions (updated via resize_callback)
static int files_win_w = 550;
static int files_win_h = 400;

// Static temp buffer for files_refresh (avoids 8KB stack allocation)
static pfs32_direntry_t fm_temp_entries[64];

// Sync fm_path from the current instance
static void fm_sync_path_from_instance(void) {
    if (fm_cur) strcpy(fm_path, fm_cur->path);
}

// Sync fm_path TO the current instance (call after modifying fm_path)
static void fm_sync_path_to_instance(void) {
    if (fm_cur) strcpy(fm_cur->path, fm_path);
}

// Forward Declarations
void files_refresh();
void files_on_scroll(int delta);

// External desktop path for sync
extern char g_desktop_path[128];
extern void desktop_refresh();

// ===== Scrollbar helpers =====

// Calculate the max scroll offset based on content size
// win_w, win_h are the FULL window dimensions (including title bar)
static int files_max_scroll(int win_w, int win_h) {
    int cols = (win_w - MARGIN_LEFT - SCROLLBAR_W) / CELL_W;
    if (cols < 1) cols = 1;
    int total_rows = (last_count + cols - 1) / cols;
    int total_content_h = total_rows * CELL_H;
    int visible_h = win_h - MARGIN_TOP;
    int max_s = total_content_h - visible_h;
    if (max_s < 0) max_s = 0;
    return max_s;
}

// Clamp scroll_offset to valid range
static void files_clamp_scroll(int win_w, int win_h) {
    int max_s = files_max_scroll(win_w, win_h);
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > max_s) scroll_offset = max_s;
}

// Resize callback — called when the window is resized
static void files_on_resize(int new_w, int new_h) {
    files_win_w = new_w;
    files_win_h = new_h;
    files_clamp_scroll(new_w, new_h - 30);
}

// Scroll callback — called from the main loop via window->scroll_callback
void files_on_scroll(int delta) {
    fm_set_current();
    fm_sync_path_from_instance();
    scroll_offset -= delta * CELL_H;
    files_clamp_scroll(files_win_w, files_win_h - 30);
}

void files_on_hscroll(int delta) {
    fm_set_current();
    hscroll_offset -= delta * CELL_W;
    int max_hscroll = (last_count * CELL_W) - files_win_w + SCROLLBAR_W;
    if (max_hscroll < 0) max_hscroll = 0;
    if (hscroll_offset < 0) hscroll_offset = 0;
    if (hscroll_offset > max_hscroll) hscroll_offset = max_hscroll;
}
extern void sys_fs_copy_recursive(const char* src, const char* dest);
extern int sys_fs_delete_recursive(const char* path);
extern void sys_fs_generate_unique_name(const char* path, const char* base, int is_dir, char* out);

// ===== Per-Instance Navigation History =====

static void fm_nav_push(FMInstance* inst, const char* path) {
    if (!inst) return;
    if (inst->hist_pos < FM_HISTORY_SIZE - 1) {
        inst->hist_pos++;
        strncpy(inst->history[inst->hist_pos], path, FM_PATH_MAX - 1);
        inst->history[inst->hist_pos][FM_PATH_MAX - 1] = 0;
        inst->hist_count = inst->hist_pos + 1;
    }
}

static void fm_nav_back(void) {
    if (!fm_cur || fm_cur->hist_pos <= 0) return;
    fm_cur->hist_pos--;
    strcpy(fm_path, fm_cur->history[fm_cur->hist_pos]);
    strcpy(fm_cur->path, fm_path);
    files_refresh();
}

static void fm_nav_forward(void) {
    if (!fm_cur || fm_cur->hist_pos >= fm_cur->hist_count - 1) return;
    fm_cur->hist_pos++;
    strcpy(fm_path, fm_cur->history[fm_cur->hist_pos]);
    strcpy(fm_cur->path, fm_path);
    files_refresh();
}

void op_up_dir() {
    fm_set_current();
    fm_sync_path_from_instance();
    if (strcmp(fm_path, "/") == 0) return;
    char new_path[FM_PATH_MAX];
    strncpy(new_path, fm_path, FM_PATH_MAX - 1);
    new_path[FM_PATH_MAX - 1] = 0;
    int len = strlen(new_path);
    if (len > 1 && new_path[len-1] == '/') new_path[len-1] = 0;
    char* last = strrchr(new_path, '/');
    if (last && last != new_path) *last = 0;
    else strcpy(new_path, "/");
    strncpy(fm_path, new_path, FM_PATH_MAX - 1);
    fm_path[FM_PATH_MAX - 1] = 0;
    fm_sync_path_to_instance();
    fm_nav_push(fm_cur, fm_path);
    files_refresh();
}

// ===== Navigate into a folder =====
static void fm_navigate_into(const char* folder_name) {
    fm_set_current();
    fm_sync_path_from_instance();
    
    char new_path[FM_PATH_MAX];
    if (!fm_build_path(new_path, FM_PATH_MAX, fm_path, folder_name)) {
        // Path too long, don't navigate
        return;
    }
    
    // Update fm_path and instance
    strcpy(fm_path, new_path);
    fm_sync_path_to_instance();
    fm_nav_push(fm_cur, fm_path);
    files_refresh();
}

// ===== Refresh =====
void files_refresh() {
    ctx_active = 0;
    prompt_active = 0;
    scroll_offset = 0;
    hscroll_offset = 0;
    sb_dragging = 0;
    
    // Sync from instance path to global
    fm_sync_path_from_instance();
    
    uint32_t blk = 0;
    extern int get_dir_block(const char*, uint32_t*);
    if(get_dir_block(fm_path, &blk) != 0) {
        strcpy(fm_path, "/");
    }
    
    // Sync back to instance
    fm_sync_path_to_instance();

    memset(last_entries, 0, sizeof(last_entries));
    memset(is_selected, 0, sizeof(is_selected));
    
    extern int sys_fs_list_dir(const char*, void*, int);
    // Use static buffer instead of stack to avoid 8KB stack allocation
    memset(fm_temp_entries, 0, sizeof(fm_temp_entries));
    int raw = sys_fs_list_dir(fm_path, fm_temp_entries, 64);
    
    last_count = 0;
    for(int i=0; i<raw; i++) {
        if(fm_temp_entries[i].filename[0] != 0 && fm_temp_entries[i].filename[0] != '.' && 
           strcmp(fm_temp_entries[i].filename, "..") != 0) {
            last_entries[last_count++] = fm_temp_entries[i];
        }
    }
}

// ===== Clipboard Operations =====
void op_copy() {
    int idx = -1;
    for(int i=0; i<last_count; i++) if(is_selected[i]) idx=i;
    if(idx >= 0) {
        fm_build_path(clipboard_path, sizeof(clipboard_path), fm_path, last_entries[idx].filename);
        clipboard_active = 1; clipboard_op = 0;
    }
}

void op_paste() {
    if(!clipboard_active) return;
    char* fname = strrchr(clipboard_path, '/');
    if(fname) fname++; else fname = clipboard_path;
    
    char dest[128]; 
    strcpy(dest, fm_path);
    if(strcmp(fm_path, "/")!=0) strcat(dest, "/"); 
    
    char final_name[64];
    char temp_path[128]; strcpy(temp_path, dest); strcat(temp_path, fname);
    if (sys_fs_exists(temp_path)) {
        sys_fs_generate_unique_name(fm_path, fname, 0, final_name);
    } else {
        strcpy(final_name, fname);
    }
    // Bounds check before strcat
    int dest_len = strlen(dest);
    int fname_len = strlen(final_name);
    if (dest_len + fname_len + 1 < 128) {
        strcat(dest, final_name);
        sys_fs_copy_recursive(clipboard_path, dest);
    }
    
    if(clipboard_op == 1) {
        sys_fs_delete_recursive(clipboard_path);
        clipboard_active = 0;
    }
    files_refresh();
    
    // Sync desktop if we modified the desktop directory
    if (strcmp(fm_path, g_desktop_path) == 0) {
        desktop_refresh();
    }
}

void op_delete() {
    int idx = -1;
    for(int i=0; i<last_count; i++) if(is_selected[i]) idx=i;
    if(idx < 0) return;
    
    char path[128];
    if (!fm_build_path(path, sizeof(path), fm_path, last_entries[idx].filename)) return;
    
    if(last_entries[idx].attributes & 0x10)
        sys_fs_delete_recursive(path);
    else
        sys_fs_delete(path);
    
    files_refresh();
    
    // Sync desktop if we modified the desktop directory
    if (strcmp(fm_path, g_desktop_path) == 0) {
        desktop_refresh();
    }
}

void op_new_item(int is_dir) {
    prompt_active = 1;
    prompt_is_dir = is_dir;
    prompt_buffer[0] = 0;
    prompt_len = 0;
}

void op_commit_new_item() {
    if (prompt_len == 0) { prompt_active = 0; return; }
    
    char path[128];
    if (!fm_build_path(path, sizeof(path), fm_path, prompt_buffer)) {
        prompt_active = 0;
        return;
    }
    
    sys_fs_create(path, prompt_is_dir);
    prompt_active = 0;
    files_refresh();
    
    // Sync desktop if we modified the desktop directory
    if (strcmp(fm_path, g_desktop_path) == 0) {
        desktop_refresh();
    }
}

// ===== Context Menu =====
#define CTX_ITEM_H  22
#define CTX_PAD_X   8
#define CTX_FONT_W  8

typedef struct {
    const char* label;
    int action;  // 0=new folder, 1=new file, 2=copy, 3=paste, 4=delete, 5=open, 6=refresh
} CtxItem;

void files_draw_ctx(int win_x, int win_y) {
    if (!ctx_active) return;
    
    CtxItem bg_items[] = {
        {"New Folder", 0},
        {"New File",   1},
        {"Paste",      3},
        {"Refresh",    6},
    };
    CtxItem file_items[] = {
        {"Open",     5},
        {"Copy",     2},
        {"Delete",   4},
    };
    
    CtxItem* items;
    int item_count;
    if (ctx_type == 0) { items = bg_items; item_count = 4; }
    else { items = file_items; item_count = 3; }
    
    // Calculate menu size
    int max_w = 0;
    for (int i = 0; i < item_count; i++) {
        int w = strlen(items[i].label) * CTX_FONT_W + CTX_PAD_X * 2;
        if (w > max_w) max_w = w;
    }
    int menu_h = item_count * CTX_ITEM_H + 4;
    
    int mx = win_x + ctx_x;
    int my = win_y + ctx_y;
    // Ensure menu stays within window bounds
    if (mx + max_w > win_x + files_win_w) mx = win_x + files_win_w - max_w;
    if (my + menu_h > win_y + files_win_h) my = win_y + files_win_h - menu_h;
    
    // Background
    gfx_fill_rounded_rect(mx, my, max_w, menu_h, 0xFFFFFFFF, 6);
    gfx_draw_rect(mx, my, max_w, menu_h, 0xFF888888);
    
    // Shadow
    gfx_fill_rounded_rect(mx + 2, my + 2, max_w, menu_h, 0x40000000, 6);
    gfx_fill_rounded_rect(mx, my, max_w, menu_h, 0xFFFFFFFF, 6);
    gfx_draw_rect(mx, my, max_w, menu_h, 0xFF888888);
    
    // Get mouse position for hover detection (mouse coords are window-local)
    extern int mouse_x, mouse_y;
    int hover_idx = -1;
    for (int i = 0; i < item_count; i++) {
        int iy = my + 2 + i * CTX_ITEM_H;
        if (mouse_x >= mx && mouse_x < mx + max_w &&
            mouse_y >= iy && mouse_y < iy + CTX_ITEM_H) {
            hover_idx = i;
            break;
        }
    }
    
    for (int i = 0; i < item_count; i++) {
        int iy = my + 2 + i * CTX_ITEM_H;
        // Highlight hovered item
        if (i == hover_idx) {
            gfx_fill_rounded_rect(mx + 2, iy, max_w - 4, CTX_ITEM_H, 0xFF007AFF, 4);
            gfx_draw_string(mx + CTX_PAD_X, iy + 4, items[i].label, 0xFFFFFFFF);
        } else {
            gfx_draw_string(mx + CTX_PAD_X, iy + 4, items[i].label, 0xFF333333);
        }
    }
}

void files_ctx_click(int click_x, int click_y) {
    if (!ctx_active) return;
    
    CtxItem bg_items[] = {{"New Folder",0},{"New File",1},{"Paste",3},{"Refresh",6}};
    CtxItem file_items[] = {{"Open",5},{"Copy",2},{"Delete",4}};
    CtxItem* items;
    int item_count;
    if (ctx_type == 0) { items = bg_items; item_count = 4; }
    else { items = file_items; item_count = 3; }
    
    int max_w = 0;
    for (int i = 0; i < item_count; i++) {
        int w = strlen(items[i].label) * CTX_FONT_W + CTX_PAD_X * 2;
        if (w > max_w) max_w = w;
    }
    int menu_h = item_count * CTX_ITEM_H + 4;
    
    // Apply same clamping as files_draw_ctx() so click detection matches drawn position
    int mx = ctx_x, my = ctx_y;
    if (mx + max_w > files_win_w) mx = files_win_w - max_w;
    if (my + menu_h > files_win_h) my = files_win_h - menu_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    
    if (click_x >= mx && click_x < mx + max_w && click_y >= my && click_y < my + menu_h) {
        int idx = (click_y - my - 2) / CTX_ITEM_H;
        if (idx >= 0 && idx < item_count) {
            int action = items[idx].action;
            switch(action) {
                case 0: op_new_item(1); break; // New Folder
                case 1: op_new_item(0); break; // New File
                case 2: op_copy(); break;       // Copy
                case 3: op_paste(); break;      // Paste
                case 4: op_delete(); break;     // Delete
                case 5: { // Open
                    if (ctx_target_idx >= 0 && ctx_target_idx < last_count) {
                        if (last_entries[ctx_target_idx].attributes & 0x10) {
                            fm_navigate_into(last_entries[ctx_target_idx].filename);
                        } else {
                            // Open file with default app (TextEdit for text files)
                            char full_path[256];
                            if (fm_build_path(full_path, sizeof(full_path), fm_path, last_entries[ctx_target_idx].filename)) {
                                extern void desktop_execute_item(const char*, int);
                                desktop_execute_item(full_path, 0);
                            }
                        }
                    }
                    break;
                }
                case 6: files_refresh(); break; // Refresh
            }
        }
    }
    ctx_active = 0;
}

// ===== Input Handling =====
void files_on_input(int key) {
    fm_set_current();
    fm_sync_path_from_instance();
    if (prompt_active) {
        // Accept both LF (10) and CR (13) as Enter/Return
        if (key == '\n' || key == '\r' || key == 13) {
            op_commit_new_item();
        } else if (key == '\b' || key == 127) {
            if (prompt_len > 0) {
                prompt_len--;
                prompt_buffer[prompt_len] = 0;
            }
        } else if (key == 27) { // Escape
            prompt_active = 0;
        } else if (key >= 32 && key != 127 && (key <= 126 || key >= 160) && prompt_len < 38) {
            prompt_buffer[prompt_len++] = (char)key;
            prompt_buffer[prompt_len] = 0;
        }
        return;
    }
    
    if (key == '\b' || key == 127) op_up_dir();  // Backspace goes up
    if (key == 0x107 || key == 0x14B) op_up_dir();  // Left arrow or back
}

// ===== Toolbar Drawing =====
void draw_toolbar(int x, int y, int w) {
    // Toolbar background
    gfx_fill_rect(x, y, w, TOOLBAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + TOOLBAR_H - 1, w, 1, 0xFFC6C6C8);
    
    int bx = x + 6;
    
    // Back button
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 8, y + 9, "<", 0xFF555555);
    
    // Forward button
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 8, y + 9, ">", 0xFF555555);
    
    // Up button
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 6, y + 9, "^", 0xFF555555);
    
    // Reload button
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 36, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 4, y + 9, "Rfr", 0xFF555555);
    
    // Path display
    bx += 42;
    int path_w = w - (bx - x) - 10;
    if (path_w > 0) {
        gfx_fill_rounded_rect(bx, y + 4, path_w, 24, 0xFFFFFFFF, 4);
        gfx_draw_rect(bx, y + 4, path_w, 24, 0xFFC6C6C8);
        // Truncate path if too long
        int max_chars = (path_w - 8) / 6;
        if (max_chars > 0) {
            char display_path[128];
            int plen = strlen(fm_path);
            if (plen > max_chars) {
                strcpy(display_path, "...");
                strcat(display_path, fm_path + plen - max_chars + 3);
            } else {
                strcpy(display_path, fm_path);
            }
            gfx_draw_string(bx + 6, y + 9, display_path, 0xFF333333);
        }
    }
}

// ===== Toolbar Click Handling =====
int handle_toolbar_click(int x, int y, int btn, int win_x, int win_y) {
    if (y >= win_y && y < win_y + TOOLBAR_H && btn == 1) {
        int bx = win_x + 6;
        // Back button
        if (x >= bx && x < bx + 28) { fm_nav_back(); return 1; }
        bx += 32;
        // Forward button
        if (x >= bx && x < bx + 28) { fm_nav_forward(); return 1; }
        bx += 32;
        // Up button
        if (x >= bx && x < bx + 28) { op_up_dir(); return 1; }
        bx += 32;
        // Reload button
        if (x >= bx && x < bx + 36) { files_refresh(); return 1; }
        return 1; // Consumed by toolbar
    }
    return 0;
}

// ===== Main Paint =====
void files_on_paint(int x, int y, int w, int h) {
    fm_set_current();
    fm_sync_path_from_instance();
    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
    
    // Toolbar
    draw_toolbar(x, y, w);
    
    // Content area (exclude scrollbar width)
    int content_w = w - SCROLLBAR_W;
    int content_y = y + MARGIN_TOP;
    int content_h = h - MARGIN_TOP;
    int cols = (content_w - MARGIN_LEFT) / CELL_W;
    if (cols < 1) cols = 1;
    
    // Draw folder icons in a grid
    for(int i = 0; i < last_count; i++) {
        int col = i % cols;
        int row = i / cols;
        
        int ix = x + MARGIN_LEFT + col * CELL_W + ICON_PAD_X - hscroll_offset;
        int iy = content_y + row * CELL_H + ICON_PAD_Y - scroll_offset;
        
        // Skip if outside visible area
        if (iy + CELL_H < content_y) continue;
        if (iy > content_y + content_h) break;
        
        // Selection highlight
        if (is_selected[i]) {
            gfx_fill_rounded_rect(ix - 6, iy - 4, ICON_SIZE + 12, CELL_H - 4, 0x40007AFF, 6);
        }
        
        // Icon
        const char* icon = (last_entries[i].attributes & 0x10) ? "folder" : "file";
        int len = strlen(last_entries[i].filename);
        if(len > 4 && strcmp(last_entries[i].filename + len - 4, ".app") == 0) icon = "terminal";
        
        cm_draw_image(0, icon, ix, iy, ICON_SIZE, ICON_SIZE);
        
        // Label - centered under icon, with wrapping
        int label_x = ix + ICON_SIZE / 2;
        int label_y = iy + ICON_SIZE + 4;
        char display_name[42];
        strncpy(display_name, last_entries[i].filename, 40);
        display_name[40] = 0;
        // Truncate with ellipsis if too wide for the cell
        int max_label_w = CELL_W - 4;  // Max width for text
        int max_label_chars = max_label_w / CHAR_W;
        int name_len = strlen(display_name);
        if (name_len > max_label_chars && max_label_chars > 3) {
            display_name[max_label_chars - 1] = '~';
            display_name[max_label_chars] = 0;
        }
        int text_w = strlen(display_name) * 8;
        int lx = label_x - text_w / 2;
        
        // Shadow for readability
        sys_gfx_string(lx + 1, label_y + 1, display_name, 0xFF000000);
        sys_gfx_string(lx, label_y, display_name, is_selected[i] ? 0xFFFFFFFF : 0xFF333333);
    }
    
    // ===== Scrollbar =====
    int max_s = files_max_scroll(w, h);
    if (max_s > 0) {
        int sb_x = x + w - SCROLLBAR_W;
        int sb_track_y = content_y;
        int sb_track_h = content_h;
        
        // Track background
        gfx_fill_rect(sb_x, sb_track_y, SCROLLBAR_W, sb_track_h, 0xFFE8E8ED);
        // Track border
        gfx_draw_rect(sb_x, sb_track_y, SCROLLBAR_W, sb_track_h, 0xFFD1D1D6);
        
        // Thumb
        int thumb_h = (sb_track_h * sb_track_h) / (sb_track_h + max_s);
        if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
        int thumb_y = sb_track_y + (scroll_offset * (sb_track_h - thumb_h)) / max_s;
        
        // Thumb body (rounded look)
        gfx_fill_rect(sb_x + SCROLLBAR_PAD, thumb_y, 
                       SCROLLBAR_W - SCROLLBAR_PAD * 2, thumb_h, 0xFFC1C1C6);
        // Thumb highlight (top)
        gfx_fill_rect(sb_x + SCROLLBAR_PAD + 1, thumb_y, 
                       SCROLLBAR_W - SCROLLBAR_PAD * 2 - 2, 1, 0xFFD4D4D8);
    }
    
    // Prompt overlay (new folder/file name)
    if (prompt_active) {
        int pw = 320, ph = 130;
        int px = x + (w - pw) / 2;
        int py = y + h / 2 - 65;
        
        // Dim overlay
        gfx_fill_rounded_rect(x, y, w, h, 0x40000000, 0);
        
        // Dialog card shadow
        gfx_fill_rounded_rect(px + 3, py + 3, pw, ph, 0x40000000, 8);
        // Dialog card
        gfx_fill_rounded_rect(px, py, pw, ph, 0xFFFFFFFF, 8);
        gfx_draw_rect(px, py, pw, ph, 0xFF007AFF);
        
        const char* title = prompt_is_dir ? "New Folder" : "New File";
        gfx_draw_string(px + pw/2 - strlen(title)*4, py + 10, title, 0xFF333333);
        
        // Subtitle
        const char* hint = "Enter name:";
        gfx_draw_string(px + 16, py + 34, hint, 0xFF8E8E93);
        
        // Input field
        int field_w = pw - 32;
        gfx_fill_rect(px + 16, py + 52, field_w, 24, 0xFFF2F2F7);
        gfx_draw_rect(px + 16, py + 52, field_w, 24, 0xFFC6C6C8);
        gfx_draw_string(px + 20, py + 56, prompt_buffer, 0xFF333333);
        
        // Blinking cursor
        static int pb = 0; pb++;
        if (pb % 60 < 30) {
            int cw = prompt_len * 8;
            gfx_fill_rect(px + 20 + cw, py + 56, 1, 14, 0xFF007AFF);
        }
        
        // Cancel button
        int cancel_x = px + 16, btn_y = py + 86;
        gfx_fill_rounded_rect(cancel_x, btn_y, 100, 32, 0xFFE8E8ED, 6);
        gfx_draw_rect(cancel_x, btn_y, 100, 32, 0xFFC6C6C8);
        gfx_draw_string(cancel_x + 25, btn_y + 9, "Cancel", 0xFF333333);
        
        // Create button
        int create_x = px + pw - 136;
        gfx_fill_rounded_rect(create_x, btn_y, 120, 32, 0xFF007AFF, 6);
        gfx_draw_string(create_x + 28, btn_y + 9, "Create", 0xFFFFFFFF);
        
        // Hint
        gfx_draw_string(px + 16, py + ph - 16, "Enter=Create  Esc=Cancel", 0xFF8E8E93);
    }
    
    // Context menu
    files_draw_ctx(x, y);
}

// ===== Mouse Handling =====
void files_on_mouse(int x, int y, int btn) {
    fm_set_current();
    fm_sync_path_from_instance();
    // Content area dimensions — use tracked window dimensions
    int win_w = files_win_w, win_h = files_win_h - 30;
    int content_y_offset = MARGIN_TOP;
    int content_h = win_h - MARGIN_TOP;
    int cols = (win_w - MARGIN_LEFT - SCROLLBAR_W) / CELL_W;
    if (cols < 1) cols = 1;

    // ---- Scrollbar drag handling ----
    if (sb_dragging) {
        if (btn == 1) {
            int max_s = files_max_scroll(win_w, win_h);
            if (max_s > 0) {
                int sb_track_h = content_h;
                int thumb_h = (sb_track_h * sb_track_h) / (sb_track_h + max_s);
                if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
                int track_travel = sb_track_h - thumb_h;
                if (track_travel > 0) {
                    int dy = y - sb_drag_start_y;
                    int d_offset = (dy * max_s) / track_travel;
                    scroll_offset = sb_drag_start_offset + d_offset;
                    files_clamp_scroll(win_w, win_h);
                }
            }
            return;
        } else {
            sb_dragging = 0;
        }
    }

    // ---- Handle prompt mode - check button clicks ----
    if (prompt_active) {
        if (btn == 1) {
            int pw = 320;
            int px_off = (win_w - pw) / 2;
            int py_off = win_h / 2 - 65;
            
            // Cancel button
            if (x >= px_off + 16 && x <= px_off + 116 && y >= py_off + 86 && y <= py_off + 118) {
                prompt_active = 0;
                return;
            }
            // Create button
            if (x >= px_off + pw - 136 && x <= px_off + pw - 16 && y >= py_off + 86 && y <= py_off + 118) {
                op_commit_new_item();
                return;
            }
        }
        return;
    }
    
    // ---- Handle context menu click ----
    if (ctx_active) {
        if (btn == 1) {
            files_ctx_click(x, y);
        }
        ctx_active = 0;
        return;
    }
    
    // ---- Handle toolbar clicks ----
    if (handle_toolbar_click(x, y, btn, 0, 0)) return;

    // ---- Scrollbar click/drag ----
    int max_s = files_max_scroll(win_w, win_h);
    if (max_s > 0 && x >= win_w - SCROLLBAR_W && btn == 1) {
        int sb_track_y = content_y_offset;
        int sb_track_h = content_h;
        int thumb_h = (sb_track_h * sb_track_h) / (sb_track_h + max_s);
        if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
        int thumb_y = sb_track_y + (scroll_offset * (sb_track_h - thumb_h)) / max_s;

        if (y >= thumb_y && y < thumb_y + thumb_h) {
            // Clicked on thumb — start drag
            sb_dragging = 1;
            sb_drag_start_y = y;
            sb_drag_start_offset = scroll_offset;
            return;
        } else if (y < thumb_y) {
            // Clicked above thumb — page up
            scroll_offset -= content_h;
            files_clamp_scroll(win_w, win_h);
            return;
        } else {
            // Clicked below thumb — page down
            scroll_offset += content_h;
            files_clamp_scroll(win_w, win_h);
            return;
        }
    }

    // ---- Handle content area clicks ----
    for(int i = 0; i < last_count; i++) {
        int col = i % cols;
        int row = i / cols;
        
        int ix = MARGIN_LEFT + col * CELL_W;
        int iy = content_y_offset + row * CELL_H - scroll_offset;
        
        if (x >= ix && x < ix + CELL_W && y >= iy && y < iy + CELL_H) {
            // Right click - context menu
            if (btn == 2) {
                memset(is_selected, 0, sizeof(is_selected));
                is_selected[i] = 1;
                ctx_active = 1;
                ctx_x = x; ctx_y = y;
                ctx_type = 1;
                ctx_target_idx = i;
                return;
            }
            
            // Left click
            if (is_selected[i]) {
                // Double click - open
                if (last_entries[i].attributes & 0x10) {
                    // Open folder: navigate into it
                    fm_navigate_into(last_entries[i].filename);
                } else {
                    // Open file with default app
                    char full_path[256];
                    if (fm_build_path(full_path, sizeof(full_path), fm_path, last_entries[i].filename)) {
                        extern void desktop_execute_item(const char*, int);
                        desktop_execute_item(full_path, 0);
                    }
                }
                return;
            }
            
            // Select
            memset(is_selected, 0, sizeof(is_selected));
            is_selected[i] = 1;
            return;
        }
    }
    
    // Clicked empty space
    if (btn == 2) {
        ctx_active = 1;
        ctx_x = x; ctx_y = y;
        ctx_type = 0;
        memset(is_selected, 0, sizeof(is_selected));
    } else if (btn == 1) {
        memset(is_selected, 0, sizeof(is_selected));
    }
}

// ===== Menu Actions =====
void files_menu_action(int menu_idx, int item_idx) {
    if(menu_idx == 0) {
        if(item_idx == 0) files_refresh();  // File > Refresh
    } else if(menu_idx == 1) {
        if(item_idx == 0) op_new_item(1);   // New > Folder
        else if(item_idx == 1) op_new_item(0); // New > File
    } else if(menu_idx == 2) {
        files_refresh();  // View > Refresh
    }
}

// ===== Init =====
void init_files_app() {
    // Check if a path was passed as launch argument
    extern void wrap_get_args(char* b, int m);
    char args[256] = {0};
    wrap_get_args(args, sizeof(args) - 1);
    
    // If a valid directory path was passed, use it as the initial path
    if (args[0] == '/') {
        strncpy(fm_path, args, FM_PATH_MAX - 1);
        fm_path[FM_PATH_MAX - 1] = 0;
    } else {
        strcpy(fm_path, "/");
    }
    
    files_refresh();
    
    window_t* w = ws_create_window("Finder", 550, 400, files_on_paint, files_on_input, files_on_mouse);
    
    // Allocate a per-window instance for independent navigation
    FMInstance* inst = fm_alloc_instance(w->id);
    if (inst) {
        fm_cur = inst;
        strcpy(inst->path, fm_path);
        fm_nav_push(inst, fm_path);
    }
    
    // Set scroll callback so the main loop can dispatch scroll wheel events
    w->scroll_callback = (void*)files_on_scroll;
    w->hscroll_callback = (void*)files_on_hscroll;
    w->resize_callback = (void*)files_on_resize;
    
    w->menu_count = 3;
    strcpy(w->menus[0].name, "File");
    strcpy(w->menus[0].items[0].label, "Refresh");
    strcpy(w->menus[0].items[1].label, "Close");
    w->menus[0].item_count = 2;
    
    strcpy(w->menus[1].name, "New");
    strcpy(w->menus[1].items[0].label, "Folder");
    strcpy(w->menus[1].items[1].label, "File");
    w->menus[1].item_count = 2;
    
    strcpy(w->menus[2].name, "View");
    strcpy(w->menus[2].items[0].label, "Refresh");
    w->menus[2].item_count = 1;
    
    dock_register("Finder", 2, w);
}
