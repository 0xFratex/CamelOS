// usr/desktop.c
#include "../sys/api.h"
#include "framework.h"
#include "../core/string.h"
#include "lib/camel_framework.h"
#include "lib/camel_ui.h"
#include "../fs/pfs32.h"
#include "desktop.h"

// Externs from bubbleview.c
extern int desktop_rename_active;
extern int desktop_rename_idx;
extern char desktop_rename_buf[64];
extern int desktop_rename_cursor;

// External declaration of the context menu from bubbleview.c
typedef struct {
    int active;
    int x, y, w, h;
    int item_count;
    struct {
        char label[32];
        int action_id;
        int enabled;
    } items[10];
    void* target_obj;
    int target_type;
} ContextMenuState;

extern ContextMenuState g_ctx_menu;

#define DESKTOP_PATH "/home/desktop"
#define GRID_START_X 30
#define GRID_START_Y 60 
#define ICON_SPACING_X 100
#define ICON_SPACING_Y 100

int desktop_is_ctx_open() {
    return g_ctx_menu.active;
}

pfs32_direntry_t desk_entries[32];
int desk_count = 0;
int desk_selected[32];

void desktop_refresh() {
    uint32_t blk = 0xFFFFFFFF;
    extern int get_dir_block(const char*, uint32_t*);
    if(get_dir_block(DESKTOP_PATH, &blk) != 0) {
        sys_fs_create(DESKTOP_PATH, 1);
        get_dir_block(DESKTOP_PATH, &blk);
    }
    
    // Clear old state explicitly
    desk_count = 0;
    memset(desk_entries, 0, sizeof(desk_entries));
    memset(desk_selected, 0, sizeof(desk_selected));
    
    // Force reset rename state on refresh to avoid ghost inputs
    if (desktop_rename_active) {
        // If we were renaming, we keep it active ONLY if the file still exists?
        // Better to cancel to prevent confusion.
        desktop_rename_active = 0;
        desktop_rename_idx = -1;
    }

    if (blk != 0xFFFFFFFF) {
        pfs32_direntry_t temp[32];
        int raw = sys_fs_list_dir(DESKTOP_PATH, temp, 32);
        for(int i=0; i<raw; i++) {
            if(temp[i].filename[0] != 0 && temp[i].filename[0] != '.') {
                desk_entries[desk_count++] = temp[i];
            }
        }
    }
}

void desktop_init() {
    desktop_refresh();
}

void desktop_draw(uint32_t* buffer) {
    // 1. Wallpaper
    for(int y=0; y<768; y++) {
        uint32_t col = 0xFF3b80c6 - (y/4); // Blue gradient
        for(int x=0; x<1024; x++) buffer[y*1024+x] = col;
    }

    int x = GRID_START_X;
    int y = GRID_START_Y;

    // 2. Draw Icons
    for(int i=0; i<desk_count; i++) {
        // Selection Highlight
        if(desk_selected[i] && !(desktop_rename_active && desktop_rename_idx == i)) {
            sys_gfx_rect(x-10, y-5, 68, 80, 0x40FFFFFF);
        }

        const char* icon = (desk_entries[i].attributes & 0x10) ? "folder" : "file";
        // Check for .app extension
        int len = strlen(desk_entries[i].filename);
        if(len > 4 && strcmp(desk_entries[i].filename + len - 4, ".app") == 0) icon = "terminal";

        cm_draw_image(buffer, icon, x, y, 48, 48);

        // --- RENAME LOGIC FIX ---
        if (desktop_rename_active && desktop_rename_idx == i) {
            int text_w = strlen(desktop_rename_buf) * 6;
            int box_w = (text_w < 60) ? 60 : text_w + 10;
            int box_x = x + 24 - (box_w / 2);

            // 1. Draw Opaque White Box
            sys_gfx_rect(box_x, y+52, box_w, 16, 0xFFFFFFFF);

            // 2. Draw Black Border
            sys_gfx_rect(box_x, y+52, box_w, 1, 0xFF000000); // Top
            sys_gfx_rect(box_x, y+67, box_w, 1, 0xFF000000); // Bottom
            sys_gfx_rect(box_x, y+52, 1, 16, 0xFF000000);    // Left
            sys_gfx_rect(box_x+box_w-1, y+52, 1, 16, 0xFF000000); // Right

            // 3. Draw Text (Black)
            int tx = box_x + 5;
            sys_gfx_string(tx, y+56, desktop_rename_buf, 0xFF000000);

            // 4. Cursor
            static int blink = 0; blink++;
            if ((blink / 20) % 2) {
                int cx = tx + (desktop_rename_cursor * 6);
                sys_gfx_rect(cx, y+55, 1, 10, 0xFF000000);
            }
        } else {
            // Normal Label (Shadowed for visibility)
            int text_w = strlen(desk_entries[i].filename) * 6;
            int label_x = x + 24 - (text_w / 2);
            sys_gfx_string(label_x+1, y+53, desk_entries[i].filename, 0xFF000000); // Shadow
            sys_gfx_string(label_x, y+52, desk_entries[i].filename, 0xFFFFFFFF);   // Text
        }
        
        y += ICON_SPACING_Y;
        if (y > 600) { y = GRID_START_Y; x += ICON_SPACING_X; }
    }
}

void desktop_on_mouse(int mx, int my, int lb, int rb) {
    if (rb) {
        int x = GRID_START_X;
        int y = GRID_START_Y;
        int hit_idx = -1;

        for(int i=0; i<desk_count; i++) {
             if (mx >= x && mx <= x+48 && my >= y && my <= y+60) {
                 hit_idx = i;
                 break;
             }
             y += ICON_SPACING_Y;
             if (y > 600) { y = GRID_START_Y; x += ICON_SPACING_X; }
        }

        if (hit_idx != -1) {
            // Store index for rename logic
            // Hack: pass index as string pointer? No, pass valid pointer for delete, 
            // but we need index for rename.
            // We'll calculate index again in bubbleview or use a static global.
            // For now, let's pass the path string as expected by bubbleview.
            static char path_buf[128];
            strcpy(path_buf, "/home/desktop/");
            strcat(path_buf, desk_entries[hit_idx].filename);
            
            // Also select it
            memset(desk_selected, 0, sizeof(desk_selected));
            desk_selected[hit_idx] = 1;
            
            ctx_menu_show(mx, my, 1, path_buf); 
        } else {
            ctx_menu_show(mx, my, 0, 0); 
        }
        return;
    }

    if (lb) {
        // If renaming, clicking outside commits
        if (desktop_rename_active) {
            // bubbleview handles commit, just return
            return;
        }

        int x = GRID_START_X;
        int y = GRID_START_Y;
        int hit = 0;
        for(int i=0; i<desk_count; i++) {
            if (mx >= x && mx <= x+48 && my >= y && my <= y+60) {
                memset(desk_selected, 0, sizeof(desk_selected));
                desk_selected[i] = 1;
                hit = 1;
                break;
            }
            y += ICON_SPACING_Y;
            if (y > 600) { y = GRID_START_Y; x += ICON_SPACING_X; }
        }
        if(!hit) memset(desk_selected, 0, sizeof(desk_selected));
    }
}