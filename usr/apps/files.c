// usr/apps/files.c - CamelOS Finder/Files App
// Full-featured file manager with navigation, context menus, and proper icon grid
// FIX: All per-window state moved into FMInstance for true multi-instance independence
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../clipboard.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/video/gfx_hal.h"
#include "../../core/window_server.h"
#include "../dock.h"
// Selection box API for rubber-band multi-select in the file grid
#include "../lib/selection_box.h"

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
// Each Finder window gets its own complete state: path, history, entries,
// selection, scroll, context menu, prompt — fully independent.
#define MAX_FM_INSTANCES 8
#define FM_HISTORY_SIZE 16
#define FM_MAX_ENTRIES   64

typedef struct {
    int active;
    int window_id;

    // Navigation
    char path[FM_PATH_MAX];
    char history[FM_HISTORY_SIZE][FM_PATH_MAX];
    int hist_count;
    int hist_pos;

    // Directory entries (was global last_entries/last_count)
    pfs32_direntry_t entries[FM_MAX_ENTRIES];
    int entry_count;
    int is_selected[FM_MAX_ENTRIES];

    // Scroll (was global scroll_offset/hscroll_offset)
    int scroll_offset;
    int hscroll_offset;

    // Scrollbar drag state (was global sb_dragging etc.)
    int sb_dragging;
    int sb_drag_start_y;
    int sb_drag_start_offset;

    // Window dimensions (was global files_win_w/files_win_h)
    int win_w;
    int win_h;

    // Context Menu State (was global ctx_*)
    int ctx_active;
    int ctx_x, ctx_y;
    int ctx_type;       // 0=Background, 1=File/Folder
    int ctx_target_idx;

    // Prompt State (was global prompt_*)
    int prompt_active;
    int prompt_is_dir;   // 1=creating folder, 0=creating file
    char prompt_buffer[40];
    int prompt_len;

    // Selection Box (rubber-band multi-select)
    selection_box_t selbox;
    int selbox_inited;

    // Double-click tracking (for open-on-double-click behavior)
    int last_click_idx;     // Index of last clicked entry (-1 = none)
    int last_click_frame;   // Frame counter when last click happened
    int btn_prev;           // Previous frame's btn value (for edge detection)
} FMInstance;

static FMInstance fm_instances[MAX_FM_INSTANCES];
static FMInstance* fm_cur = 0;

// Global frame counter for double-click timing
static int fm_frame_counter = 0;

// Double-click threshold: if two clicks on the same item happen within
// this many frames, it's a double-click (open). At ~60fps, 30 = 0.5s.
#define FM_DBLCLICK_FRAMES 30

// Static temp buffer for files_refresh (avoids 8KB stack allocation)
static pfs32_direntry_t fm_temp_entries[FM_MAX_ENTRIES];

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
            // Initialize history at index 0 with the root path so that
            // fm_nav_back() can always find a valid entry at hist_pos=0.
            // Previously history[0] was empty, causing back navigation to
            // set path="" which fell back to "/" in files_refresh().
            strcpy(fm_instances[i].history[0], "/");
            fm_instances[i].hist_count = 1;
            fm_instances[i].hist_pos = 0;
            fm_instances[i].win_w = 550;
            fm_instances[i].win_h = 400;
            // Initialize rubber-band selection box for this instance
            selbox_init(&fm_instances[i].selbox);
            fm_instances[i].selbox_inited = 1;
            fm_instances[i].last_click_idx = -1;
            fm_instances[i].last_click_frame = 0;
            fm_instances[i].btn_prev = 0;
            return &fm_instances[i];
        }
    }
    return 0;
}

// Set the current instance from the given window's user_data.
// This is the CORRECT way: each callback receives the window that owns it,
// so we resolve the instance directly from user_data instead of from
// the global active_win (which may point to a different window).
static void fm_set_current_for(window_t* win) {
    if (win && win->user_data) {
        fm_cur = (FMInstance*)win->user_data;
        return;
    }
    // Fallback: try active_win (for cases where win is not available)
    extern window_t* active_win;
    if (active_win) {
        FMInstance* inst = fm_find_instance(active_win->id);
        if (inst) { fm_cur = inst; return; }
    }
    for (int i = 0; i < MAX_FM_INSTANCES; i++) {
        if (fm_instances[i].active) { fm_cur = &fm_instances[i]; return; }
    }
    fm_cur = 0;
}

// Legacy compatibility: resolve from active_win
static void fm_set_current(void) {
    extern window_t* active_win;
    fm_set_current_for(active_win);
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

// Forward Declarations
void files_refresh();
void files_on_scroll(window_t* win, int delta);

// External desktop path for sync
extern char g_desktop_path[128];
extern void desktop_refresh();

// ===== Scrollbar helpers (now take FMInstance*) =====

static int files_max_scroll(FMInstance* inst) {
    int win_w = inst->win_w;
    int win_h = inst->win_h;
    int cols = (win_w - MARGIN_LEFT - SCROLLBAR_W) / CELL_W;
    if (cols < 1) cols = 1;
    int total_rows = (inst->entry_count + cols - 1) / cols;
    int total_content_h = total_rows * CELL_H;
    int visible_h = win_h - 30 - MARGIN_TOP;
    int max_s = total_content_h - visible_h;
    if (max_s < 0) max_s = 0;
    return max_s;
}

static void files_clamp_scroll(FMInstance* inst) {
    int max_s = files_max_scroll(inst);
    if (inst->scroll_offset < 0) inst->scroll_offset = 0;
    if (inst->scroll_offset > max_s) inst->scroll_offset = max_s;
}

// Resize callback — receives window* as first arg for instance resolution
static void files_on_resize(window_t* win, int new_w, int new_h) {
    fm_set_current_for(win);
    if (!fm_cur) return;
    fm_cur->win_w = new_w;
    fm_cur->win_h = new_h;
    files_clamp_scroll(fm_cur);
}

// Scroll callback — receives window* for instance resolution
void files_on_scroll(window_t* win, int delta) {
    fm_set_current_for(win);
    if (!fm_cur) return;
    fm_cur->scroll_offset -= delta * CELL_H;
    files_clamp_scroll(fm_cur);
}

void files_on_hscroll(window_t* win, int delta) {
    fm_set_current_for(win);
    if (!fm_cur) return;
    fm_cur->hscroll_offset -= delta * CELL_W;
    int max_hscroll = (fm_cur->entry_count * CELL_W) - fm_cur->win_w + SCROLLBAR_W;
    if (max_hscroll < 0) max_hscroll = 0;
    if (fm_cur->hscroll_offset < 0) fm_cur->hscroll_offset = 0;
    if (fm_cur->hscroll_offset > max_hscroll) fm_cur->hscroll_offset = max_hscroll;
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
    strcpy(fm_cur->path, fm_cur->history[fm_cur->hist_pos]);
    files_refresh();
}

static void fm_nav_forward(void) {
    if (!fm_cur || fm_cur->hist_pos >= fm_cur->hist_count - 1) return;
    fm_cur->hist_pos++;
    strcpy(fm_cur->path, fm_cur->history[fm_cur->hist_pos]);
    files_refresh();
}

void op_up_dir() {
    fm_set_current();  // Uses active_win — OK because ops are user-triggered on the active window
    if (!fm_cur) return;
    if (strcmp(fm_cur->path, "/") == 0) return;
    char new_path[FM_PATH_MAX];
    strncpy(new_path, fm_cur->path, FM_PATH_MAX - 1);
    new_path[FM_PATH_MAX - 1] = 0;
    int len = strlen(new_path);
    if (len > 1 && new_path[len-1] == '/') new_path[len-1] = 0;
    char* last = strrchr(new_path, '/');
    if (last && last != new_path) *last = 0;
    else strcpy(new_path, "/");
    strncpy(fm_cur->path, new_path, FM_PATH_MAX - 1);
    fm_cur->path[FM_PATH_MAX - 1] = 0;
    fm_nav_push(fm_cur, fm_cur->path);
    files_refresh();
}

// ===== Navigate into a folder =====
static void fm_navigate_into(const char* folder_name) {
    fm_set_current();  // Uses active_win — OK because navigation is user-triggered
    if (!fm_cur) return;
    
    char new_path[FM_PATH_MAX];
    if (!fm_build_path(new_path, FM_PATH_MAX, fm_cur->path, folder_name)) {
        return;
    }
    
    strcpy(fm_cur->path, new_path);
    fm_nav_push(fm_cur, fm_cur->path);
    files_refresh();
}

// ===== Refresh (uses fm_cur instance directly) =====
void files_refresh() {
    if (!fm_cur) return;
    
    fm_cur->ctx_active = 0;
    fm_cur->prompt_active = 0;
    fm_cur->scroll_offset = 0;
    fm_cur->hscroll_offset = 0;
    fm_cur->sb_dragging = 0;
    
    uint32_t blk = 0;
    extern int get_dir_block(const char*, uint32_t*);
    if(get_dir_block(fm_cur->path, &blk) != 0) {
        strcpy(fm_cur->path, "/");
    }

    memset(fm_cur->entries, 0, sizeof(fm_cur->entries));
    memset(fm_cur->is_selected, 0, sizeof(fm_cur->is_selected));
    
    extern int sys_fs_list_dir(const char*, void*, int);
    memset(fm_temp_entries, 0, sizeof(fm_temp_entries));
    int raw = sys_fs_list_dir(fm_cur->path, fm_temp_entries, FM_MAX_ENTRIES);
    
    fm_cur->entry_count = 0;
    for(int i=0; i<raw; i++) {
        if(fm_temp_entries[i].filename[0] != 0 && fm_temp_entries[i].filename[0] != '.' && 
           strcmp(fm_temp_entries[i].filename, "..") != 0) {
            fm_cur->entries[fm_cur->entry_count++] = fm_temp_entries[i];
        }
    }
}

// ===== Clipboard Operations =====
void op_copy() {
    if (!fm_cur) return;
    int idx = -1;
    for(int i=0; i<fm_cur->entry_count; i++) if(fm_cur->is_selected[i]) idx=i;
    if(idx >= 0) {
        fm_build_path(clipboard_path, sizeof(clipboard_path), fm_cur->path, fm_cur->entries[idx].filename);
        clipboard_active = 1; clipboard_op = 0;
    }
}

void op_paste() {
    if (!fm_cur || !clipboard_active) return;
    char* fname = strrchr(clipboard_path, '/');
    if(fname) fname++; else fname = clipboard_path;
    
    char dest[128]; 
    strcpy(dest, fm_cur->path);
    if(strcmp(fm_cur->path, "/")!=0) strcat(dest, "/"); 
    
    char final_name[64];
    char temp_path[128]; strcpy(temp_path, dest); strcat(temp_path, fname);
    if (sys_fs_exists(temp_path)) {
        sys_fs_generate_unique_name(fm_cur->path, fname, 0, final_name);
    } else {
        strcpy(final_name, fname);
    }
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
    
    if (strcmp(fm_cur->path, g_desktop_path) == 0) {
        desktop_refresh();
    }
}

void op_delete() {
    if (!fm_cur) return;
    int idx = -1;
    for(int i=0; i<fm_cur->entry_count; i++) if(fm_cur->is_selected[i]) idx=i;
    if(idx < 0) return;
    
    char path[128];
    if (!fm_build_path(path, sizeof(path), fm_cur->path, fm_cur->entries[idx].filename)) return;
    
    if(fm_cur->entries[idx].attributes & 0x10)
        sys_fs_delete_recursive(path);
    else
        sys_fs_delete(path);
    
    files_refresh();
    
    if (strcmp(fm_cur->path, g_desktop_path) == 0) {
        desktop_refresh();
    }
}

void op_new_item(int is_dir) {
    if (!fm_cur) return;
    fm_cur->prompt_active = 1;
    fm_cur->prompt_is_dir = is_dir;
    fm_cur->prompt_buffer[0] = 0;
    fm_cur->prompt_len = 0;
}

void op_commit_new_item() {
    if (!fm_cur) return;
    if (fm_cur->prompt_len == 0) { fm_cur->prompt_active = 0; return; }
    
    char path[128];
    if (!fm_build_path(path, sizeof(path), fm_cur->path, fm_cur->prompt_buffer)) {
        fm_cur->prompt_active = 0;
        return;
    }
    
    sys_fs_create(path, fm_cur->prompt_is_dir);
    fm_cur->prompt_active = 0;
    files_refresh();
    
    if (strcmp(fm_cur->path, g_desktop_path) == 0) {
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
    if (!fm_cur || !fm_cur->ctx_active) return;
    
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
    /* macOS-style: when right-clicking a .app bundle, show an extra
     * "Show Package Contents" item that navigates INTO the bundle
     * directory instead of launching it. */
    CtxItem app_items[] = {
        {"Open",                   5},
        {"Show Package Contents",  7},
        {"Copy",                   2},
        {"Delete",                 4},
    };
    
    /* Detect if the right-clicked item is a .app bundle. */
    int is_app_bundle = 0;
    if (fm_cur->ctx_type == 1 &&
        fm_cur->ctx_target_idx >= 0 &&
        fm_cur->ctx_target_idx < fm_cur->entry_count) {
        int elen = strlen(fm_cur->entries[fm_cur->ctx_target_idx].filename);
        if ((fm_cur->entries[fm_cur->ctx_target_idx].attributes & 0x10) &&
            elen > 4 &&
            strcmp(fm_cur->entries[fm_cur->ctx_target_idx].filename + elen - 4, ".app") == 0) {
            is_app_bundle = 1;
        }
    }
    
    CtxItem* items;
    int item_count;
    if (fm_cur->ctx_type == 0) { items = bg_items; item_count = 4; }
    else if (is_app_bundle)    { items = app_items; item_count = 4; }
    else                       { items = file_items; item_count = 3; }
    
    int max_w = 0;
    for (int i = 0; i < item_count; i++) {
        int w = strlen(items[i].label) * CTX_FONT_W + CTX_PAD_X * 2;
        if (w > max_w) max_w = w;
    }
    int menu_h = item_count * CTX_ITEM_H + 4;
    
    int mx = win_x + fm_cur->ctx_x;
    int my = win_y + fm_cur->ctx_y;
    if (mx + max_w > win_x + fm_cur->win_w) mx = win_x + fm_cur->win_w - max_w;
    if (my + menu_h > win_y + fm_cur->win_h) my = win_y + fm_cur->win_h - menu_h;
    
    // Shadow then background
    gfx_fill_rounded_rect(mx + 2, my + 2, max_w, menu_h, 0x40000000, 6);
    gfx_fill_rounded_rect(mx, my, max_w, menu_h, 0xFFFFFFFF, 6);
    gfx_draw_rect(mx, my, max_w, menu_h, 0xFF888888);
    
    // Hover detection
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
        if (i == hover_idx) {
            gfx_fill_rounded_rect(mx + 2, iy, max_w - 4, CTX_ITEM_H, 0xFF007AFF, 4);
            gfx_draw_string(mx + CTX_PAD_X, iy + 4, items[i].label, 0xFFFFFFFF);
        } else {
            gfx_draw_string(mx + CTX_PAD_X, iy + 4, items[i].label, 0xFF333333);
        }
    }
}

void files_ctx_click(int click_x, int click_y) {
    if (!fm_cur || !fm_cur->ctx_active) return;
    
    CtxItem bg_items[] = {{"New Folder",0},{"New File",1},{"Paste",3},{"Refresh",6}};
    CtxItem file_items[] = {{"Open",5},{"Copy",2},{"Delete",4}};
    CtxItem app_items[] = {{"Open",5},{"Show Package Contents",7},{"Copy",2},{"Delete",4}};
    
    /* Detect if the right-clicked item is a .app bundle (same logic
     * as in files_draw_ctx above — kept duplicated to avoid touching
     * the shared state during paint). */
    int is_app_bundle = 0;
    if (fm_cur->ctx_type == 1 &&
        fm_cur->ctx_target_idx >= 0 &&
        fm_cur->ctx_target_idx < fm_cur->entry_count) {
        int elen = strlen(fm_cur->entries[fm_cur->ctx_target_idx].filename);
        if ((fm_cur->entries[fm_cur->ctx_target_idx].attributes & 0x10) &&
            elen > 4 &&
            strcmp(fm_cur->entries[fm_cur->ctx_target_idx].filename + elen - 4, ".app") == 0) {
            is_app_bundle = 1;
        }
    }
    
    CtxItem* items;
    int item_count;
    if (fm_cur->ctx_type == 0) { items = bg_items; item_count = 4; }
    else if (is_app_bundle)    { items = app_items; item_count = 4; }
    else                       { items = file_items; item_count = 3; }
    
    int max_w = 0;
    for (int i = 0; i < item_count; i++) {
        int w = strlen(items[i].label) * CTX_FONT_W + CTX_PAD_X * 2;
        if (w > max_w) max_w = w;
    }
    int menu_h = item_count * CTX_ITEM_H + 4;
    
    int mx = fm_cur->ctx_x, my = fm_cur->ctx_y;
    if (mx + max_w > fm_cur->win_w) mx = fm_cur->win_w - max_w;
    if (my + menu_h > fm_cur->win_h) my = fm_cur->win_h - menu_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    
    if (click_x >= mx && click_x < mx + max_w && click_y >= my && click_y < my + menu_h) {
        int idx = (click_y - my - 2) / CTX_ITEM_H;
        if (idx >= 0 && idx < item_count) {
            int action = items[idx].action;
            switch(action) {
                case 0: op_new_item(1); break;
                case 1: op_new_item(0); break;
                case 2: op_copy(); break;
                case 3: op_paste(); break;
                case 4: op_delete(); break;
                case 5: {
                    if (fm_cur->ctx_target_idx >= 0 && fm_cur->ctx_target_idx < fm_cur->entry_count) {
                        int elen = strlen(fm_cur->entries[fm_cur->ctx_target_idx].filename);
                        int is_app_dir = (fm_cur->entries[fm_cur->ctx_target_idx].attributes & 0x10) &&
                            (elen > 4 && strcmp(fm_cur->entries[fm_cur->ctx_target_idx].filename + elen - 4, ".app") == 0);
                        
                        if ((fm_cur->entries[fm_cur->ctx_target_idx].attributes & 0x10) && !is_app_dir) {
                            fm_navigate_into(fm_cur->entries[fm_cur->ctx_target_idx].filename);
                        } else {
                            char full_path[256];
                            if (fm_build_path(full_path, sizeof(full_path), fm_cur->path, fm_cur->entries[fm_cur->ctx_target_idx].filename)) {
                                extern void desktop_execute_item(const char*, int);
                                desktop_execute_item(full_path, 0);
                            }
                        }
                    }
                    break;
                }
                case 6: files_refresh(); break;
                case 7: {
                    /* "Show Package Contents" — navigate INTO the .app
                     * bundle directory instead of launching it. This is
                     * the macOS behaviour: right-click a .app, pick
                     * "Show Package Contents", and Finder opens the
                     * bundle as a regular folder so you can browse
                     * Contents/, MacOS/, Resources/, etc. */
                    if (fm_cur->ctx_target_idx >= 0 &&
                        fm_cur->ctx_target_idx < fm_cur->entry_count) {
                        fm_navigate_into(fm_cur->entries[fm_cur->ctx_target_idx].filename);
                    }
                    break;
                }
            }
        }
    }
    fm_cur->ctx_active = 0;
}

// ===== Input Handling =====
void files_on_input(window_t* win, int key) {
    fm_set_current_for(win);
    if (!fm_cur) return;
    
    if (fm_cur->prompt_active) {
        if (key == '\n' || key == '\r' || key == 13) {
            op_commit_new_item();
        } else if (key == '\b' || key == 127) {
            if (fm_cur->prompt_len > 0) {
                fm_cur->prompt_len--;
                fm_cur->prompt_buffer[fm_cur->prompt_len] = 0;
            }
        } else if (key == 27) {
            fm_cur->prompt_active = 0;
        } else if (key >= 32 && key != 127 && (key <= 126 || key >= 160) && fm_cur->prompt_len < 38) {
            fm_cur->prompt_buffer[fm_cur->prompt_len++] = (char)key;
            fm_cur->prompt_buffer[fm_cur->prompt_len] = 0;
        }
        return;
    }
    
    if (key == '\b' || key == 127) op_up_dir();
    if (key == 0x107 || key == 0x14B) op_up_dir();
}

// ===== Toolbar Drawing =====
void draw_toolbar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, TOOLBAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + TOOLBAR_H - 1, w, 1, 0xFFC6C6C8);
    
    int bx = x + 6;
    
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 8, y + 9, "<", 0xFF555555);
    
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 8, y + 9, ">", 0xFF555555);
    
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 6, y + 9, "^", 0xFF555555);
    
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 36, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 4, y + 9, "Rfr", 0xFF555555);
    
    bx += 42;
    int path_w = w - (bx - x) - 10;
    if (path_w > 0 && fm_cur) {
        gfx_fill_rounded_rect(bx, y + 4, path_w, 24, 0xFFFFFFFF, 4);
        gfx_draw_rect(bx, y + 4, path_w, 24, 0xFFC6C6C8);
        int max_chars = (path_w - 8) / 6;
        if (max_chars > 0) {
            char display_path[128];
            int plen = strlen(fm_cur->path);
            if (plen > max_chars) {
                strcpy(display_path, "...");
                strcat(display_path, fm_cur->path + plen - max_chars + 3);
            } else {
                strcpy(display_path, fm_cur->path);
            }
            gfx_draw_string(bx + 6, y + 9, display_path, 0xFF333333);
        }
    }
}

// ===== Toolbar Click Handling =====
// CRITICAL: Uses rising-edge detection (click_edge) to prevent navigation
// buttons from firing on every frame while the mouse button is held down.
// Without this, holding the back/forward button triggers fm_nav_back() or
// fm_nav_forward() at ~60fps, causing rapid uncontrolled navigation and
// duplicate entries in the directory listing.
int handle_toolbar_click(int x, int y, int btn, int win_x, int win_y) {
    // Only process on the rising edge of a button press (initial click)
    int click_edge = (btn == 1 && fm_cur && fm_cur->btn_prev == 0);
    if (y >= win_y && y < win_y + TOOLBAR_H && btn == 1 && click_edge) {
        int bx = win_x + 6;
        if (x >= bx && x < bx + 28) { fm_nav_back(); return 1; }
        bx += 32;
        if (x >= bx && x < bx + 28) { fm_nav_forward(); return 1; }
        bx += 32;
        if (x >= bx && x < bx + 28) { op_up_dir(); return 1; }
        bx += 32;
        if (x >= bx && x < bx + 36) { files_refresh(); return 1; }
        return 1;
    }
    return 0;
}

// ===== Main Paint =====
void files_on_paint(window_t* win, int x, int y, int w, int h) {
    fm_set_current_for(win);
    if (!fm_cur) return;
    
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
    for(int i = 0; i < fm_cur->entry_count; i++) {
        int col = i % cols;
        int row = i / cols;
        
        int ix = x + MARGIN_LEFT + col * CELL_W + ICON_PAD_X - fm_cur->hscroll_offset;
        int iy = content_y + row * CELL_H + ICON_PAD_Y - fm_cur->scroll_offset;
        
        if (iy + CELL_H < content_y) continue;
        if (iy > content_y + content_h) break;
        
        if (fm_cur->is_selected[i]) {
            gfx_fill_rounded_rect(ix - 6, iy - 4, ICON_SIZE + 12, CELL_H - 4, 0x40007AFF, 6);
        }
        
        // Icon — .app bundles show as app icon, not installer
        const char* icon = (fm_cur->entries[i].attributes & 0x10) ? "folder" : "file";
        int len = strlen(fm_cur->entries[i].filename);
        if(len > 4 && strcmp(fm_cur->entries[i].filename + len - 4, ".app") == 0) icon = "terminal";
        if(len > 4 && strcmp(fm_cur->entries[i].filename + len - 4, ".dmg") == 0) icon = "hdd_icon";
        if(len > 4 && strcmp(fm_cur->entries[i].filename + len - 4, ".cdl") == 0) icon = "terminal";
        
        cm_draw_image(0, icon, ix, iy, ICON_SIZE, ICON_SIZE);
        
        // Label
        int label_x = ix + ICON_SIZE / 2;
        int label_y = iy + ICON_SIZE + 4;
        char display_name[42];
        strncpy(display_name, fm_cur->entries[i].filename, 40);
        display_name[40] = 0;
        int max_label_w = CELL_W - 4;
        int max_label_chars = max_label_w / CHAR_W;
        int name_len = strlen(display_name);
        if (name_len > max_label_chars && max_label_chars > 3) {
            display_name[max_label_chars - 1] = '~';
            display_name[max_label_chars] = 0;
        }
        int text_w = strlen(display_name) * 8;
        int lx = label_x - text_w / 2;
        
        sys_gfx_string(lx + 1, label_y + 1, display_name, 0xFF000000);
        sys_gfx_string(lx, label_y, display_name, fm_cur->is_selected[i] ? 0xFFFFFFFF : 0xFF333333);
    }
    
    // ===== Scrollbar =====
    int max_s = files_max_scroll(fm_cur);
    if (max_s > 0) {
        int sb_x = x + w - SCROLLBAR_W;
        int sb_track_y = content_y;
        int sb_track_h = content_h;
        
        gfx_fill_rect(sb_x, sb_track_y, SCROLLBAR_W, sb_track_h, 0xFFE8E8ED);
        gfx_draw_rect(sb_x, sb_track_y, SCROLLBAR_W, sb_track_h, 0xFFD1D1D6);
        
        int thumb_h = (sb_track_h * sb_track_h) / (sb_track_h + max_s);
        if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
        int thumb_y = sb_track_y + (fm_cur->scroll_offset * (sb_track_h - thumb_h)) / max_s;
        
        gfx_fill_rect(sb_x + SCROLLBAR_PAD, thumb_y, 
                       SCROLLBAR_W - SCROLLBAR_PAD * 2, thumb_h, 0xFFC1C1C6);
        gfx_fill_rect(sb_x + SCROLLBAR_PAD + 1, thumb_y, 
                       SCROLLBAR_W - SCROLLBAR_PAD * 2 - 2, 1, 0xFFD4D4D8);
    }
    
    // Prompt overlay
    if (fm_cur->prompt_active) {
        int pw = 320, ph = 130;
        int px = x + (w - pw) / 2;
        int py = y + h / 2 - 65;
        
        gfx_fill_rounded_rect(x, y, w, h, 0x40000000, 0);
        gfx_fill_rounded_rect(px + 3, py + 3, pw, ph, 0x40000000, 8);
        gfx_fill_rounded_rect(px, py, pw, ph, 0xFFFFFFFF, 8);
        gfx_draw_rect(px, py, pw, ph, 0xFF007AFF);
        
        const char* title = fm_cur->prompt_is_dir ? "New Folder" : "New File";
        gfx_draw_string(px + pw/2 - strlen(title)*4, py + 10, title, 0xFF333333);
        
        const char* hint = "Enter name:";
        gfx_draw_string(px + 16, py + 34, hint, 0xFF8E8E93);
        
        int field_w = pw - 32;
        gfx_fill_rect(px + 16, py + 52, field_w, 24, 0xFFF2F2F7);
        gfx_draw_rect(px + 16, py + 52, field_w, 24, 0xFFC6C6C8);
        gfx_draw_string(px + 20, py + 56, fm_cur->prompt_buffer, 0xFF333333);
        
        static int pb = 0; pb++;
        if (pb % 60 < 30) {
            int cw = fm_cur->prompt_len * 8;
            gfx_fill_rect(px + 20 + cw, py + 56, 1, 14, 0xFF007AFF);
        }
        
        int cancel_x = px + 16, btn_y = py + 86;
        gfx_fill_rounded_rect(cancel_x, btn_y, 100, 32, 0xFFE8E8ED, 6);
        gfx_draw_rect(cancel_x, btn_y, 100, 32, 0xFFC6C6C8);
        gfx_draw_string(cancel_x + 25, btn_y + 9, "Cancel", 0xFF333333);
        
        int create_x = px + pw - 136;
        gfx_fill_rounded_rect(create_x, btn_y, 120, 32, 0xFF007AFF, 6);
        gfx_draw_string(create_x + 28, btn_y + 9, "Create", 0xFFFFFFFF);
        
        gfx_draw_string(px + 16, py + ph - 16, "Enter=Create  Esc=Cancel", 0xFF8E8E93);
    }
    
    // Context menu
    files_draw_ctx(x, y);

    // Draw rubber-band selection box if active
    if (fm_cur->selbox_inited) {
        // The selbox stores window-content-local coordinates (same system
        // as the mouse x/y passed to files_on_mouse).  The paint callback
        // receives the window's screen position in (x, y), so we simply
        // add them to convert content-local → screen-absolute.
        selection_box_t* sb = &fm_cur->selbox;
        if (sb->state == SELBOX_DRAGGING) {
            int rx, ry, rw, rh;
            if (selbox_get_rect(sb, &rx, &ry, &rw, &rh)) {
                if (rw >= sb->min_drag && rh >= sb->min_drag) {
                    gfx_fill_rounded_rect(x + rx, y + ry, rw, rh, sb->color, 4);
                    gfx_draw_rect(x + rx, y + ry, rw, rh, sb->border_color);
                }
            }
        }
    }
}

// ===== Mouse Handling =====
void files_on_mouse(window_t* win, int x, int y, int btn) {
    fm_set_current_for(win);
    if (!fm_cur) return;

    // Handle mouse-up (btn==0): end any active rubber-band selection and return.
    // This event comes from the continuous drag dispatch in bubbleview.c when
    // the user releases the mouse button after a content drag.
    if (btn == 0) {
        if (fm_cur->selbox_inited && fm_cur->selbox.state == SELBOX_DRAGGING) {
            selbox_end(&fm_cur->selbox);
        }
        // Also end scrollbar drag on release
        if (fm_cur->sb_dragging) {
            fm_cur->sb_dragging = 0;
        }
        fm_cur->btn_prev = 0;  // Reset so next press is detected as click edge
        return; // Don't process a release as a click
    }
    
    int win_w = fm_cur->win_w, win_h = fm_cur->win_h - 30;
    int content_y_offset = MARGIN_TOP;
    int content_h = win_h - MARGIN_TOP;
    int cols = (win_w - MARGIN_LEFT - SCROLLBAR_W) / CELL_W;
    if (cols < 1) cols = 1;

    // ---- Scrollbar drag handling ----
    if (fm_cur->sb_dragging) {
        if (btn == 1) {
            int max_s = files_max_scroll(fm_cur);
            if (max_s > 0) {
                int sb_track_h = content_h;
                int thumb_h = (sb_track_h * sb_track_h) / (sb_track_h + max_s);
                if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
                int track_travel = sb_track_h - thumb_h;
                if (track_travel > 0) {
                    int dy = y - fm_cur->sb_drag_start_y;
                    int d_offset = (dy * max_s) / track_travel;
                    fm_cur->scroll_offset = fm_cur->sb_drag_start_offset + d_offset;
                    files_clamp_scroll(fm_cur);
                }
            }
            return;
        } else {
            fm_cur->sb_dragging = 0;
        }
    }

    // ---- Handle prompt mode ----
    if (fm_cur->prompt_active) {
        if (btn == 1) {
            int pw = 320;
            int px_off = (win_w - pw) / 2;
            int py_off = win_h / 2 - 65;
            
            if (x >= px_off + 16 && x <= px_off + 116 && y >= py_off + 86 && y <= py_off + 118) {
                fm_cur->prompt_active = 0;
                return;
            }
            if (x >= px_off + pw - 136 && x <= px_off + pw - 16 && y >= py_off + 86 && y <= py_off + 118) {
                op_commit_new_item();
                return;
            }
        }
        return;
    }
    
    // ---- Handle context menu click ----
    if (fm_cur->ctx_active) {
        if (btn == 1) {
            files_ctx_click(x, y);
        }
        fm_cur->ctx_active = 0;
        return;
    }
    
    // ---- Handle toolbar clicks ----
    if (handle_toolbar_click(x, y, btn, 0, 0)) return;

    // ---- Scrollbar click/drag ----
    int max_s = files_max_scroll(fm_cur);
    if (max_s > 0 && x >= win_w - SCROLLBAR_W && btn == 1) {
        int sb_track_y = content_y_offset;
        int sb_track_h = content_h;
        int thumb_h = (sb_track_h * sb_track_h) / (sb_track_h + max_s);
        if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
        int thumb_y = sb_track_y + (fm_cur->scroll_offset * (sb_track_h - thumb_h)) / max_s;

        if (y >= thumb_y && y < thumb_y + thumb_h) {
            fm_cur->sb_dragging = 1;
            fm_cur->sb_drag_start_y = y;
            fm_cur->sb_drag_start_offset = fm_cur->scroll_offset;
            return;
        } else if (y < thumb_y) {
            fm_cur->scroll_offset -= content_h;
            files_clamp_scroll(fm_cur);
            return;
        } else {
            fm_cur->scroll_offset += content_h;
            files_clamp_scroll(fm_cur);
            return;
        }
    }

    // ---- Handle content area clicks ----
    // If the selection box is being dragged, update it and apply selection
    if (fm_cur->selbox_inited && fm_cur->selbox.state == SELBOX_DRAGGING && btn == 1) {
        selbox_update(&fm_cur->selbox, x, y);
        // Apply selection: mark entries whose icons intersect the selection rect
        int rx, ry, rw, rh;
        if (selbox_get_rect(&fm_cur->selbox, &rx, &ry, &rw, &rh)) {
            if (rw >= fm_cur->selbox.min_drag || rh >= fm_cur->selbox.min_drag) {
                memset(fm_cur->is_selected, 0, sizeof(fm_cur->is_selected));
                for(int i = 0; i < fm_cur->entry_count; i++) {
                    int col = i % cols;
                    int row = i / cols;
                    int ix = MARGIN_LEFT + col * CELL_W;
                    int iy = content_y_offset + row * CELL_H - fm_cur->scroll_offset;
                    // Check intersection with selection rect
                    if (ix < rx + rw && ix + CELL_W > rx && iy < ry + rh && iy + CELL_H > ry) {
                        fm_cur->is_selected[i] = 1;
                    }
                }
            }
        }
        return; // During drag, don't process icon clicks
    }

    // Only process click/selection logic on the RISING EDGE of a button press
    // (i.e., the frame where btn first becomes 1). This prevents the continuous
    // drag dispatch (which sends btn==1 every frame while held) from triggering
    // false double-clicks — the single biggest cause of "one click opens file".
    int click_edge = (btn == 1 && fm_cur->btn_prev == 0);
    fm_cur->btn_prev = btn;

    if (click_edge) {
        fm_frame_counter++;
    }

    for(int i = 0; i < fm_cur->entry_count; i++) {
        int col = i % cols;
        int row = i / cols;
        
        int ix = MARGIN_LEFT + col * CELL_W;
        int iy = content_y_offset + row * CELL_H - fm_cur->scroll_offset;
        
        if (x >= ix && x < ix + CELL_W && y >= iy && y < iy + CELL_H) {
            if (btn == 2) {
                // Right-click on icon: select it and show context menu
                memset(fm_cur->is_selected, 0, sizeof(fm_cur->is_selected));
                fm_cur->is_selected[i] = 1;
                fm_cur->ctx_active = 1;
                fm_cur->ctx_x = x; fm_cur->ctx_y = y;
                fm_cur->ctx_type = 1;
                fm_cur->ctx_target_idx = i;
                return;
            }
            
            // Left-click on icon: only process on the click edge (initial press),
            // NOT on every frame while the button is held
            if (!click_edge) return;
            
            // Single click = select, double click = open
            // Check if this is a double-click (same item clicked within threshold)
            int is_dblclick = 0;
            if (fm_cur->last_click_idx == i &&
                (fm_frame_counter - fm_cur->last_click_frame) < FM_DBLCLICK_FRAMES) {
                is_dblclick = 1;
            }
            
            // Record this click for future double-click detection
            fm_cur->last_click_idx = i;
            fm_cur->last_click_frame = fm_frame_counter;
            
            if (is_dblclick && fm_cur->is_selected[i]) {
                // Double-click: open the item
                int elen = strlen(fm_cur->entries[i].filename);
                int is_app_dir = (fm_cur->entries[i].attributes & 0x10) &&
                    (elen > 4 && strcmp(fm_cur->entries[i].filename + elen - 4, ".app") == 0);
                
                if ((fm_cur->entries[i].attributes & 0x10) && !is_app_dir) {
                    fm_navigate_into(fm_cur->entries[i].filename);
                } else {
                    char full_path[256];
                    if (fm_build_path(full_path, sizeof(full_path), fm_cur->path, fm_cur->entries[i].filename)) {
                        extern void desktop_execute_item(const char*, int);
                        desktop_execute_item(full_path, 0);
                    }
                }
                // Reset double-click state after opening
                fm_cur->last_click_idx = -1;
                return;
            }
            
            // Single click: select this item only, deselect others
            memset(fm_cur->is_selected, 0, sizeof(fm_cur->is_selected));
            fm_cur->is_selected[i] = 1;
            // Cancel any active rubber-band selection since we clicked an icon
            if (fm_cur->selbox_inited) selbox_cancel(&fm_cur->selbox);
            return;
        }
    }
    
    // Clicked empty space
    if (btn == 2) {
        fm_cur->ctx_active = 1;
        fm_cur->ctx_x = x; fm_cur->ctx_y = y;
        fm_cur->ctx_type = 0;
        memset(fm_cur->is_selected, 0, sizeof(fm_cur->is_selected));
        selbox_cancel(&fm_cur->selbox);
    } else if (btn == 1) {
        // Start rubber-band selection from this point
        memset(fm_cur->is_selected, 0, sizeof(fm_cur->is_selected));
        selbox_start(&fm_cur->selbox, x, y);
    }
}

// ===== Menu Actions =====
void files_menu_action(int menu_idx, int item_idx) {
    fm_set_current();
    if(menu_idx == 0) {
        if(item_idx == 0) files_refresh();
    } else if(menu_idx == 1) {
        if(item_idx == 0) op_new_item(1);
        else if(item_idx == 1) op_new_item(0);
    } else if(menu_idx == 2) {
        files_refresh();
    }
}

// ===== Close callback — frees FMInstance when window is closed =====
static void files_on_close(window_t* win) {
    FMInstance* inst = (FMInstance*)win->user_data;
    if (inst) {
        inst->active = 0;
        win->user_data = 0;
    }
}

// ===== Build parent directory chain into history =====
// When opening a deep path like /Users/name/Desktop/MyFolder, this
// populates history[0..N] with "/", "/Users", "/Users/name", etc.
// so that the back button navigates through the parent directories
// instead of jumping to "/" or being stuck at the initial path.
static int fm_build_parent_history(FMInstance* inst, const char* initial_path) {
    if (!inst || !initial_path || !initial_path[0]) return 0;

    // Special case: root path
    if (strcmp(initial_path, "/") == 0) {
        strcpy(inst->history[0], "/");
        inst->hist_count = 1;
        inst->hist_pos = 0;
        return 1;
    }

    // Walk up from "/" to the initial_path, collecting each component
    char components[16][FM_PATH_MAX];
    int comp_count = 0;

    // Parse path components from initial_path
    // e.g. "/Users/name/Desktop/MyFolder" -> ["Users", "name", "Desktop", "MyFolder"]
    char temp[FM_PATH_MAX];
    strncpy(temp, initial_path, FM_PATH_MAX - 1);
    temp[FM_PATH_MAX - 1] = 0;

    // Build cumulative paths: "/", "/Users", "/Users/name", etc.
    char cumulative[FM_PATH_MAX];
    int hist_idx = 0;

    // Start with root
    strcpy(inst->history[hist_idx], "/");
    hist_idx++;
    if (hist_idx >= FM_HISTORY_SIZE) goto done;

    // Skip leading '/'
    char* p = temp;
    if (*p == '/') p++;

    cumulative[0] = '/';
    cumulative[1] = 0;
    int cum_len = 1;

    while (*p && hist_idx < FM_HISTORY_SIZE) {
        // Extract next component
        char* slash = p;
        while (*slash && *slash != '/') slash++;
        char saved = *slash;
        *slash = 0;

        int comp_len = strlen(p);
        if (comp_len > 0) {
            // Build cumulative path
            if (cum_len > 1) { // not root
                cumulative[cum_len] = '/';
                memcpy(cumulative + cum_len + 1, p, comp_len);
                cum_len += 1 + comp_len;
            } else { // first component after root
                memcpy(cumulative + 1, p, comp_len);
                cum_len = 1 + comp_len;
            }
            cumulative[cum_len] = 0;

            strcpy(inst->history[hist_idx], cumulative);
            hist_idx++;
        }

        *slash = saved;
        p = slash;
        if (saved == '/') p++;
    }

done:
    inst->hist_count = hist_idx;
    inst->hist_pos = hist_idx - 1; // Point to the last entry (= initial_path)
    return 1;
}

// ===== Init =====
void init_files_app() {
    extern void wrap_get_args(char* b, int m);
    char args[256] = {0};
    wrap_get_args(args, sizeof(args) - 1);
    // Clear stale launch args immediately after reading to prevent
    // leaking the folder path into the next Files.app launch (e.g.,
    // from the dock, which calls wrap_exec() without args)
    extern void sys_set_launch_args(const char*);
    sys_set_launch_args(NULL);
    
    char initial_path[FM_PATH_MAX];
    if (args[0] == '/') {
        strncpy(initial_path, args, FM_PATH_MAX - 1);
        initial_path[FM_PATH_MAX - 1] = 0;
    } else {
        // Default to user's Desktop directory (reads username from system.conf)
        initial_path[0] = 0;
        char conf_buf[512];
        int conf_len = sys_fs_read("/etc/system.conf", conf_buf, sizeof(conf_buf) - 1);
        if (conf_len <= 0) {
            conf_len = sys_fs_read("/Library/Preferences/system.conf", conf_buf, sizeof(conf_buf) - 1);
        }
        if (conf_len > 0) {
            conf_buf[conf_len] = 0;
            char* line = strstr(conf_buf, "username=");
            if (line) {
                char username[64];
                strncpy(username, line + 9, sizeof(username) - 1);
                username[sizeof(username) - 1] = 0;
                char* nl = username;
                while (*nl && *nl != '\n' && *nl != '\r') nl++;
                *nl = 0;
                if (username[0]) {
                    strcpy(initial_path, "/Users/");
                    strcat(initial_path, username);
                    strcat(initial_path, "/Desktop");
                    // Fall back to home dir if Desktop doesn't exist
                    if (!sys_fs_exists(initial_path)) {
                        initial_path[strlen(initial_path) - 7] = 0; // Remove "/Desktop"
                    }
                }
            }
        }
        if (initial_path[0] == 0) {
            strcpy(initial_path, "/");
        }
    }
    
    window_t* w = ws_create_window("Finder", 550, 400, files_on_paint, files_on_input, files_on_mouse);
    
    FMInstance* inst = fm_alloc_instance(w->id);
    if (inst) {
        fm_cur = inst;
        strcpy(inst->path, initial_path);

        // Build the full parent directory chain into history so that
        // navigating back from /Users/name/Desktop/MyFolder goes to
        // /Users/name/Desktop (not "/").  Previously history[0] was
        // just the initial_path, meaning back-nav was impossible from
        // the starting directory.
        fm_build_parent_history(inst, initial_path);

        files_refresh();
        // Store instance pointer in window user_data so callbacks can
        // resolve their instance directly without relying on active_win
        w->user_data = (void*)inst;
    }
    
    w->scroll_callback = (void*)files_on_scroll;
    w->hscroll_callback = (void*)files_on_hscroll;
    w->resize_callback = (void*)files_on_resize;
    w->close_callback = (void*)files_on_close;
    
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
    // Multi-level submenu: View -> Sort By -> [Name, Size, Date]
    w->menus[2].items[1].is_separator = 1;  // Separator after Refresh
    strcpy(w->menus[2].items[2].label, "Sort By");
    w->menus[2].items[2].has_submenu = 1;
    strcpy(w->menus[2].items[2].submenu_labels[0], "Name");
    w->menus[2].items[2].submenu_action_ids[0] = 0;
    strcpy(w->menus[2].items[2].submenu_labels[1], "Size");
    w->menus[2].items[2].submenu_action_ids[1] = 1;
    strcpy(w->menus[2].items[2].submenu_labels[2], "Date");
    w->menus[2].items[2].submenu_action_ids[2] = 2;
    w->menus[2].items[2].submenu_count = 3;
    w->menus[2].item_count = 3;
    
    dock_register("Finder", 2, w);
}
