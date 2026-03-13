// usr/apps/files.c
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../clipboard.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../dock.h"

// Globals
static kernel_api_t* sys = 0;
char fm_path[128] = "/";
pfs32_direntry_t last_entries[64];
int is_selected[64];
int last_count = 0;

// Menu / Mouse State
int ctx_active = 0;
int ctx_x = 0, ctx_y = 0;
int ctx_type = 0; // 0=Background, 1=File
int ctx_target_idx = -1;

// Prompt State
int prompt_active = 0;
int prompt_mode = 0;
char prompt_buffer[32];
int prompt_len = 0;

#define GRID_COLS 5
#define ICON_W 60
#define ICON_H 80
#define SPACING_X 90
#define SPACING_Y 90
#define MARGIN_LEFT 20
#define MARGIN_TOP 40

// Forward Declarations
void files_refresh();
extern void sys_fs_copy_recursive(const char* src, const char* dest);
extern int sys_fs_delete_recursive(const char* path);
extern void sys_fs_generate_unique_name(const char* path, const char* base, int is_dir, char* out);

void files_refresh() {
    ctx_active = 0;
    prompt_active = 0; 
    
    // Check path validity
    uint32_t blk = 0;
    extern int get_dir_block(const char*, uint32_t*);
    if(get_dir_block(fm_path, &blk) != 0) {
        strcpy(fm_path, "/");
    }

    memset(last_entries, 0, sizeof(last_entries));
    memset(is_selected, 0, sizeof(is_selected));
    
    extern int sys_fs_list_dir(const char*, void*, int);
    pfs32_direntry_t temp[64];
    int raw = sys_fs_list_dir(fm_path, temp, 64);
    
    last_count = 0;
    for(int i=0; i<raw; i++) {
        if(temp[i].filename[0] != 0 && temp[i].filename[0] != '.') {
            last_entries[last_count++] = temp[i];
        }
    }
}
void op_up_dir() { /* ... as before ... */ }
void op_new_item(int is_dir) { /* ... as before ... */ }
void op_commit_new_item() { /* ... as before ... */ }
void op_copy() { /* ... as before ... */
    int idx = -1;
    for(int i=0; i<last_count; i++) if(is_selected[i]) idx=i;
    if(idx >= 0) {
        strcpy(clipboard_path, fm_path);
        if(strcmp(fm_path, "/")!=0) strcat(clipboard_path, "/");
        strcat(clipboard_path, last_entries[idx].filename);
        clipboard_active = 1; clipboard_op = 0;
    }
}
void op_paste() { /* ... as before ... */
    if(!clipboard_active) return;
    char* fname = strrchr(clipboard_path, '/');
    if(fname) fname++; else fname = clipboard_path;
    
    char dest[128]; 
    strcpy(dest, fm_path);
    
    // Ensure we have a trailing slash in case of root
    if(strcmp(fm_path, "/")!=0) strcat(dest, "/"); 
    else strcat(dest, "/");
    
    char final_name[64];
    // Check collision
    char temp_path[128]; strcpy(temp_path, dest); strcat(temp_path, fname);
    if (sys_fs_exists(temp_path)) {
        sys_fs_generate_unique_name(fm_path, fname, 0, final_name);
    } else {
        strcpy(final_name, fname);
    }
    strcat(dest, final_name);

    sys_fs_copy_recursive(clipboard_path, dest);
    
    if(clipboard_op == 1) {
        sys_fs_delete_recursive(clipboard_path);
        clipboard_active = 0;
    }
    files_refresh();
}
void op_delete() { /* ... as before ... */ }

// ... (files_menu_action, files_draw_ctx, files_ctx_click, files_on_input - same as before) ...
void files_menu_action(int menu_idx, int item_idx) { if(menu_idx == 0) { /* ... same ... */ } else if(menu_idx == 1) { /* ... same ... */ } else if(menu_idx == 2) { files_refresh(); } }
void files_draw_ctx(int x, int y) { /* ... same ... */ }
void files_ctx_click(int mx, int my) { /* ... same ... */ }
void files_on_input(int key) { /* ... same ... */ }

// --- Drawing (Correct for File Display) ---
void files_on_paint(int x, int y, int w, int h) {
    // ... (Background, Toolbar, Back Button, Path) ...
    // Rest of setup, which is unchanged for display

    int start_y = y + MARGIN_TOP;
    for(int i=0; i<last_count; i++) {  // Changed from desk_count to last_count
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        int ix = x + MARGIN_LEFT + (col * SPACING_X);
        int iy = start_y + (row * SPACING_Y);

        if (iy + ICON_H > y + h) break;

        // Selection Highlight
        if (is_selected[i]) {
            sys_gfx_rect(x-10, y-5, 68, 80, 0x40FFFFFF);
        }

        const char* icon = (last_entries[i].attributes & 0x10) ? "folder" : "file";  // Changed from desk_entries to last_entries
        // Check for .app extension
        int len = strlen(last_entries[i].filename);  // Changed from desk_entries to last_entries
        if(len > 4 && strcmp(last_entries[i].filename + len - 4, ".app") == 0) icon = "terminal"; // Changed from desk_entries to last_entries

        sys->draw_image(x, y, icon);

        // --- RENAME LOGIC FIX (Removed as it's from desktop) ---
        // Normal Label (Shadowed for visibility)
        int text_w = strlen(last_entries[i].filename) * 6;  // Changed from desk_entries to last_entries
        int label_x = x + 24 - (text_w / 2);
        sys_gfx_string(label_x+1, y+53, last_entries[i].filename, 0xFF000000); // Shadow
        sys_gfx_string(label_x, y+52, last_entries[i].filename, 0xFFFFFFFF);   // Text
        
        y += SPACING_Y;
        if (y > 600) { y = MARGIN_TOP; x += SPACING_X; }
    }
}

void files_on_mouse(int x, int y, int btn) {
    // Correct the issue here:
    if (prompt_active) return; 
    if (ctx_active) { /* Context menu logic -- same as before */ }
    // 4. Handle Toolbar (Back Button)
    if (y < 28 && btn == 1) {
        if (x > 5 && x < 25) op_up_dir();
        return;
    }
    // 5. Handle Content Area
    int start_y = MARGIN_TOP;
    
    // Check Icons
    for(int i=0; i<last_count; i++) { // Corrected, use last_count
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int ix = MARGIN_LEFT + (col * SPACING_X);
        int iy = start_y + (row * SPACING_Y);

        if (x >= ix && x <= ix+ICON_W && y >= iy && y <= iy+ICON_H) {
            // Right Click (2)
            if (btn == 2) {
                memset(is_selected, 0, sizeof(is_selected));
                is_selected[i] = 1;
                ctx_active = 1;
                ctx_x = x; ctx_y = y;
                ctx_type = 1;
                return;
            }
            
            // Left Click (1)
            if (is_selected[i]) {
                // Double click simulation
                if (last_entries[i].attributes & 0x10) { // Corrected access to entries
                    if(strcmp(fm_path, "/")!=0) strcat(fm_path, "/");
                    strcat(fm_path, last_entries[i].filename); // Corrected access to entries
                    files_refresh();
                }
                return;
            }
            
            // Select
            memset(is_selected, 0, sizeof(is_selected));
            is_selected[i] = 1;
            return;
        }
    }
    // Clicked Empty Space
    if (btn == 2) { ctx_active = 1; ctx_x = x; ctx_y = y; ctx_type = 0; memset(is_selected, 0, sizeof(is_selected)); }
    else if (btn == 1) { memset(is_selected, 0, sizeof(is_selected)); }
}

void init_files_app() {
    sys = fw_get_api();
    files_refresh();
    Window* w = fw_create_window("Finder", 550, 400, files_on_paint, files_on_input, files_on_mouse);
    // Setup Header Menu
    // Menus: same
    dock_register("Finder", 2, w);
}
