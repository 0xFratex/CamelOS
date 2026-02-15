// usr/apps/files_cdl.c
#include "../../sys/cdl_defs.h"
#include "../lib/camel_framework.h"

static kernel_api_t* sys = 0;

// Colors
#define C_SIDEBAR     0xFFF0F0F5
#define C_MAIN_BG     0xFFFFFFFF
#define C_SELECTION   0xFFB3D7FF
#define C_TEXT        0xFF000000
#define C_TOOLBAR     0xFFE8E8E8
#define C_CTX_BG      0xFFF2F2F2
#define C_CTX_BORDER  0xFFBBBBBB
#define C_DIALOG_BG   0xFFF8F8F8

char current_path[256] = "/home/desktop";
char history[10][256];
int hist_idx = 0;
int hist_max = 0; 

int selected_idx = -1;
int renaming_idx = -1;
char rename_buf[64] = {0};
int rename_len = 0;
int ctx_active = 0;
int ctx_x = 0, ctx_y = 0;
int ctx_target_idx = -1;
int open_with_active = 0;
int open_with_target_idx = -1;

static uint32_t last_fs_gen = 0;

// Window Dimensions (Dynamic)
static int win_w = 0;
static int win_h = 0;

// Dynamic App Registry
#define MAX_APPS 10
char app_names[MAX_APPS][32];
char app_paths[MAX_APPS][64];
int app_count = 0;

typedef struct {
    char filename[40];
    uint32_t size;
    uint32_t start_block;
    unsigned char attr;
    unsigned char res[3];
    uint32_t dates[3];
} __attribute__((packed)) direntry_t;

direntry_t entries[64];
int entry_count = 0;

int has_ext(const char* fname, const char* ext) {
    int flen = sys->strlen(fname);
    int elen = sys->strlen(ext);
    if (flen <= elen) return 0;
    return (sys->strcmp(fname + flen - elen, ext) == 0);
}

void scan_apps() {
    app_count = 0;
    direntry_t temp[32]; 
    
    // Default Apps
    sys->strcpy(app_names[0], "Terminal"); sys->strcpy(app_paths[0], "/usr/apps/Terminal.app");
    sys->strcpy(app_names[1], "Files");    sys->strcpy(app_paths[1], "/usr/apps/Files.app");
    sys->strcpy(app_names[2], "TextEdit"); sys->strcpy(app_paths[2], "/usr/apps/TextEdit.app");
    app_count = 3;

    int raw = sys->fs_list("/usr/apps", (void*)temp, 32);
    for(int i=0; i<raw; i++) {
        if(temp[i].filename[0] == 0 || temp[i].filename[0] == '.') continue;
        int len = sys->strlen(temp[i].filename);
        if (len > 4 && sys->strcmp(temp[i].filename + len - 4, ".app") == 0) {
            // Skip already registered defaults to avoid duplicates
            if (sys->strcmp(temp[i].filename, "Terminal.app") == 0) continue;
            if (sys->strcmp(temp[i].filename, "Files.app") == 0) continue;
            if (sys->strcmp(temp[i].filename, "TextEdit.app") == 0) continue;

            if (app_count < MAX_APPS) {
                sys->memset(app_names[app_count], 0, 32);
                sys->memcpy(app_names[app_count], temp[i].filename, len-4);
                sys->strcpy(app_paths[app_count], "/usr/apps/");
                sys->strcpy(app_paths[app_count] + 10, temp[i].filename);
                app_count++;
            }
        }
    }
}

void refresh_view() {
    sys->memset(entries, 0, sizeof(entries));
    direntry_t temp[64];
    int raw = sys->fs_list(current_path, (void*)temp, 64);

    entry_count = 0;
    for(int i=0; i<raw; i++) {
        if(temp[i].filename[0] == 0 || temp[i].filename[0] == '.') continue;
        entries[entry_count++] = temp[i];
    }
    if (renaming_idx == -1) selected_idx = -1;
    ctx_active = 0;
    last_fs_gen = sys->get_fs_generation();
}

void commit_rename() {
    if (renaming_idx != -1 && rename_len > 0) {
        char old_full[256], new_full[256];
        sys->strcpy(old_full, current_path);
        if(old_full[sys->strlen(old_full)-1]!='/') sys->strcpy(old_full+sys->strlen(old_full), "/");
        sys->strcpy(new_full, old_full);

        sys->strcpy(old_full+sys->strlen(old_full), entries[renaming_idx].filename);
        sys->strcpy(new_full+sys->strlen(new_full), rename_buf);
        sys->fs_rename(old_full, new_full);
    }
    renaming_idx = -1;
}

void start_rename(int idx) {
    if(idx < 0 || idx >= entry_count) return;
    renaming_idx = idx;
    selected_idx = idx;
    sys->strcpy(rename_buf, entries[idx].filename);
    rename_len = sys->strlen(rename_buf);
    ctx_active = 0;
}

void create_item(int is_dir) {
    char base[32];
    sys->strcpy(base, is_dir ? "New Folder" : "New File.txt");
    
    char path[256];
    int counter = 1;
    
    // Try exact name first
    sys->strcpy(path, current_path);
    if(path[sys->strlen(path)-1] != '/') sys->strcpy(path + sys->strlen(path), "/");
    sys->strcpy(path + sys->strlen(path), base);
    
    // If exists, try numbered loop
    if (sys->fs_exists(path)) {
        while(1) {
            char candidate[64];
            
            if (is_dir) {
                // Folder: "New Folder (1)"
                char num[8]; sys->itoa(counter, num);
                sys->strcpy(candidate, "New Folder (");
                sys->strcpy(candidate + sys->strlen(candidate), num);
                sys->strcpy(candidate + sys->strlen(candidate), ")");
            } else {
                // File: "New File (1).txt"
                char num[8]; sys->itoa(counter, num);
                sys->strcpy(candidate, "New File (");
                sys->strcpy(candidate + sys->strlen(candidate), num);
                sys->strcpy(candidate + sys->strlen(candidate), ").txt");
            }
            
            sys->strcpy(path, current_path);
            if(path[sys->strlen(path)-1] != '/') sys->strcpy(path + sys->strlen(path), "/");
            sys->strcpy(path + sys->strlen(path), candidate);
            
            if (!sys->fs_exists(path)) break;
            counter++;
        }
    }

    sys->fs_create(path, is_dir);
    refresh_view();
}

void open_item(int idx, int force_dialog) {
    if (idx < 0 || idx >= entry_count) return;

    // Directory?
    if (entries[idx].attr & 0x10) {
        char newp[256];
        sys->memset(newp, 0, 256);
        sys->strcpy(newp, current_path);
        
        int len = sys->strlen(newp);
        if(len > 0 && newp[len-1] != '/') sys->strcpy(newp + len, "/");
        sys->strcpy(newp + sys->strlen(newp), entries[idx].filename);

        if (hist_idx < 9) {
            hist_idx++;
            sys->strcpy(history[hist_idx], newp);
            hist_max = hist_idx;
        }
        sys->strcpy(current_path, newp);
        refresh_view();
        return;
    }

    // Construct Absolute Path safely
    char full_path[256];
    sys->memset(full_path, 0, 256);
    sys->strcpy(full_path, current_path);
    
    int plen = sys->strlen(full_path);
    if(plen > 0 && full_path[plen-1] != '/') sys->strcpy(full_path + plen, "/");
    sys->strcpy(full_path + sys->strlen(full_path), entries[idx].filename);

    if (force_dialog) {
        open_with_active = 1;
        open_with_target_idx = idx;
        return;
    }

    char* fname = entries[idx].filename;
    int len = sys->strlen(fname);

    // .app Bundle
    if (len > 4 && sys->strcmp(fname + len - 4, ".app") == 0) {
        sys->exec(full_path);
        return;
    }

    // Text Files -> TextEdit
    if (has_ext(fname, ".txt") || has_ext(fname, ".c") || has_ext(fname, ".h") ||
        has_ext(fname, ".md") || has_ext(fname, ".cfg") || has_ext(fname, ".json")) {
        sys->exec_with_args("/usr/apps/TextEdit.app", full_path);
        return;
    }
    
    // Fallback -> Terminal
    sys->exec_with_args("/usr/apps/Terminal.app", full_path);
}

void launch_with_app(int app_idx) {
    if (app_idx < 0 || app_idx >= app_count) return;
    if (open_with_target_idx == -1) return;
    
    char full_path[256];
    sys->memset(full_path, 0, 256);
    sys->strcpy(full_path, current_path);
    if(full_path[sys->strlen(full_path)-1] != '/') sys->strcpy(full_path + sys->strlen(full_path), "/");
    sys->strcpy(full_path + sys->strlen(full_path), entries[open_with_target_idx].filename);
    
    sys->exec_with_args(app_paths[app_idx], full_path);
    open_with_active = 0;
}

void nav_back() {
    if (hist_idx > 0) {
        hist_idx--;
        sys->strcpy(current_path, history[hist_idx]);
        refresh_view();
    }
}

void nav_forward() {
    if (hist_idx < hist_max) {
        hist_idx++;
        sys->strcpy(current_path, history[hist_idx]);
        refresh_view();
    }
}

void on_input(int key) {
    if (open_with_active) { if(key==27) open_with_active=0; return; }
    if (renaming_idx != -1) {
        if (key == '\n') {
            commit_rename();
        } else if (key == 27) {
            renaming_idx = -1;
            refresh_view();
        } else if (key == '\b') {
            if (rename_len > 0) rename_buf[--rename_len] = 0;
        } else if (key >= 32 && key <= 126 && rename_len < 30) {
            rename_buf[rename_len++] = (char)key;
            rename_buf[rename_len] = 0;
        }
        return;
    }
}

void on_mouse(int x, int y, int btn) {
    // 1. Open With Dialog Logic
    if (open_with_active) {
        int bh = 40 + (app_count * 24);
        int bw = 200;
        int bx = (win_w - bw) / 2;
        int by = (win_h - bh) / 2;
        
        if (btn == 1) {
             // List starts at offset +30 relative to box top
             int list_start_y = by + 30;
             
             for(int i=0; i<app_count; i++) {
                int row_y = list_start_y + (i * 24);
                
                // Hit test for the specific row (24px height)
                if (x >= bx && x <= bx + bw && y >= row_y && y < row_y + 24) {
                    launch_with_app(i);
                    return;
                }
            }
            
            // Close if clicked outside
            if (x < bx || x > bx + bw || y < by || y > by + bh) {
                open_with_active = 0;
            }
        }
        return;
    }

    // 2. Context Menu
    if (ctx_active) {
        if (btn == 1) {
            int rx = x - ctx_x;
            int ry = y - ctx_y;
            int h = (ctx_target_idx == -1) ? 80 : 160; // Menu height
            
            if (rx >= 0 && rx <= 140 && ry >= 0 && ry <= h) {
                // Item height is 24px, starting at offset 8 (padding)
                int item = (ry - 8) / 24;
                
                if (ctx_target_idx == -1) {
                    if (item == 0) create_item(1); // New Folder
                    if (item == 1) create_item(0); // New File
                    if (item == 2) refresh_view();
                } else {
                    if (item == 0) open_item(ctx_target_idx, 0); // Open
                    if (item == 1) open_item(ctx_target_idx, 1); // Open With
                    if (item == 2) {
                        renaming_idx = ctx_target_idx;
                        sys->strcpy(rename_buf, entries[renaming_idx].filename);
                        rename_len = sys->strlen(rename_buf);
                    }
                    if (item == 5) { // Delete is item 5
                         char dpath[256]; sys->strcpy(dpath, current_path);
                         if(dpath[sys->strlen(dpath)-1]!='/') sys->strcpy(dpath + sys->strlen(dpath), "/");
                         sys->strcpy(dpath + sys->strlen(dpath), entries[ctx_target_idx].filename);
                         sys->fs_delete(dpath);
                         refresh_view();
                    }
                }
            }
            ctx_active = 0; // Close menu on any click
        }
        return;
    }

    if (renaming_idx != -1 && btn == 1) { commit_rename(); return; }

    int toolbar_h = 40;
    int sidebar_w = 150;

    if (y < toolbar_h) {
        if (btn == 1) {
            if (x >= 10 && x <= 34) nav_back();
            if (x >= 40 && x <= 64) nav_forward();
        }
        return;
    }

    if (x < sidebar_w) {
        if (btn == 1) {
            int ry = y - toolbar_h;
            const char* target = 0;
            if (ry > 30 && ry < 60) target = "/home/desktop";
            else if (ry > 60 && ry < 90) target = "/usr/apps";
            else if (ry > 90 && ry < 120) target = "/";

            if (target) {
                sys->strcpy(current_path, target);
                if (hist_idx < 9) hist_idx++;
                sys->strcpy(history[hist_idx], target);
                hist_max = hist_idx;
                refresh_view();
            }
        }
        return;
    }

    // File List
    int list_y = y - (toolbar_h + 24); // Account for header row (24px)
    if (list_y < 0) return;
    
    int idx = list_y / 24; // 24px row height

    if (idx >= 0 && idx < entry_count) {
        if (btn == 2) { // Right Click
            ctx_active = 1;
            ctx_target_idx = idx;
            selected_idx = idx;
            ctx_x = x; ctx_y = y; // Store relative coords for draw/hit logic
            
            // Bounds check to keep menu inside window
            if (ctx_x + 140 > win_w) ctx_x = win_w - 145;
            if (ctx_y + 140 > win_h) ctx_y = win_h - 145;
            return;
        }
        if (btn == 1) { // Left Click
            if (selected_idx == idx) {
                open_item(idx, 0); // Double click behavior
            } else {
                selected_idx = idx;
            }
        }
    } else {
        // Clicked empty space in list
        if (btn == 2) {
            ctx_active = 1;
            ctx_target_idx = -1; // Background context
            ctx_x = x; ctx_y = y;
            if (ctx_x + 140 > win_w) ctx_x = win_w - 145;
            if (ctx_y + 80 > win_h) ctx_y = win_h - 85;
            selected_idx = -1;
        } else {
            selected_idx = -1;
        }
    }
}

void on_paint(int x, int y, int w, int h) {
    // --- FIX: Update Window Dimensions for Mouse Handler ---
    win_w = w;
    win_h = h;

    if (sys->get_fs_generation() != last_fs_gen) refresh_view();

    int toolbar_h = 40;
    int sidebar_w = 150;

    sys->draw_rect(x, y, w, h, C_MAIN_BG);
    sys->draw_rect(x, y, w, toolbar_h, C_TOOLBAR);
    sys->draw_rect(x, y+toolbar_h-1, w, 1, 0xFFAAAAAA);

    sys->draw_rect_rounded(x+10, y+10, 24, 20, 0xFFFFFFFF, 4);
    sys->draw_text(x+18, y+16, "<", (hist_idx > 0) ? 0xFF000000 : 0xFF888888);

    sys->draw_rect_rounded(x+40, y+10, 24, 20, 0xFFFFFFFF, 4);
    sys->draw_text(x+48, y+16, ">", (hist_idx < hist_max) ? 0xFF000000 : 0xFF888888);

    sys->draw_rect_rounded(x+75, y+8, w-90, 24, 0xFFFFFFFF, 4);
    sys->draw_text_clipped(x+85, y+14, current_path, C_TEXT, w-100);

    sys->draw_rect(x, y+toolbar_h, sidebar_w, h-toolbar_h, C_SIDEBAR);
    sys->draw_text(x+10, y+toolbar_h+10, "FAVORITES", 0xFF888888);

    int py = y+toolbar_h+30;
    sys->draw_image_scaled(x+15, py, 16, 16, "folder"); sys->draw_text(x+40, py+4, "Desktop", C_TEXT); py+=30;
    sys->draw_image_scaled(x+15, py, 16, 16, "backpack"); sys->draw_text(x+40, py+4, "Apps", C_TEXT); py+=30;
    sys->draw_image_scaled(x+15, py, 16, 16, "hdd_icon"); sys->draw_text(x+40, py+4, "Root", C_TEXT); py+=30;

    int lx = x + sidebar_w;
    int ly = y + toolbar_h;

    sys->draw_rect(lx, ly, w-sidebar_w, 24, 0xFFF0F0F0);
    sys->draw_text(lx+30, ly+6, "Name", 0xFF666666);
    sys->draw_rect(lx, ly+23, w-sidebar_w, 1, 0xFFDDDDDD);

    int row_y = ly + 24;
    for(int i=0; i<entry_count; i++) {
        if (row_y + 24 > y + h) break;

        if (i == selected_idx) {
            sys->draw_rect(lx, row_y, w-sidebar_w, 24, C_SELECTION);
        }

        const char* icon = (entries[i].attr & 0x10) ? "folder" : "file";
        int len = sys->strlen(entries[i].filename);
        if(len > 4 && sys->strcmp(entries[i].filename + len - 4, ".app") == 0) icon = "terminal";
        sys->draw_image_scaled(lx+5, row_y+4, 16, 16, icon);

        if (i == renaming_idx) {
            sys->draw_rect(lx+28, row_y+2, 200, 20, 0xFFFFFFFF);
            sys->draw_text(lx+32, row_y+6, rename_buf, 0xFF000000);
        } else {
            sys->draw_text(lx+30, row_y+6, entries[i].filename, C_TEXT);
        }
        row_y += 24;
    }

    if (ctx_active) {
        int cw = 140;
        int ch = (ctx_target_idx == -1) ? 80 : 160;
        int cx = x + ctx_x;
        int cy = y + ctx_y;
        sys->draw_rect_rounded(cx, cy, cw, ch, C_CTX_BG, 6);
        sys->draw_rect(cx, cy, cw, 1, C_CTX_BORDER); 

        if (ctx_target_idx == -1) {
            sys->draw_text(cx+10, cy+8, "New Folder", C_TEXT);
            sys->draw_text(cx+10, cy+32, "New File", C_TEXT);
            sys->draw_text(cx+10, cy+58, "Refresh", C_TEXT);
        } else {
            const char* items[] = {"Open", "Open With...", "Rename", "Duplicate", "Get Info", "Delete"};
            for(int k=0; k<6; k++) sys->draw_text(cx+10, cy+8+(k*24), items[k], C_TEXT);
        }
    }

    // --- DIALOG RENDERING ---
    if (open_with_active) {
        int bx = x + (w-200)/2;
        int bh = 40 + (app_count * 24);
        int by = y + (h-bh)/2;

        sys->draw_rect_rounded(bx+5, by+5, 200, bh, 0x40000000, 8);
        sys->draw_rect_rounded(bx, by, 200, bh, C_DIALOG_BG, 6);
        sys->draw_rect(bx, by, 200, 1, 0xFF888888);
        sys->draw_rect(bx, by+bh, 200, 1, 0xFF888888);
        sys->draw_rect(bx, by, 1, bh, 0xFF888888);
        sys->draw_rect(bx+200, by, 1, bh, 0xFF888888);

        sys->draw_text(bx+10, by+10, "Open With:", C_TEXT);

        // List starts at offset 30
        int list_start_y = by + 30;
        for(int i=0; i<app_count; i++) {
            sys->draw_text(bx+20, list_start_y + (i*24) + 6, app_names[i], C_TEXT);
        }
    }
}

void menu_cb(int m, int i) {
    if (m == 0 && i == 0) create_item(1); 
    if (m == 0 && i == 1) create_item(0); 
    if (m == 2 && i == 0) refresh_view(); 
}

static cdl_exports_t exports = { .lib_name = "Files", .version = 22 };
cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;

    char args[256];
    sys->get_launch_args(args, 256);
    if(sys->strlen(args) > 0) {
        sys->strcpy(current_path, args);
    } else {
        sys->strcpy(current_path, "/home/desktop");
    }

    hist_idx = 0;
    hist_max = 0;
    sys->strcpy(history[0], current_path);

    scan_apps();
    refresh_view();

    void* win = sys->create_window("Finder", 640, 420, on_paint, (void*)on_input, on_mouse);

    static menu_def_t menus[3];
    sys->strcpy(menus[0].name, "File");
    menus[0].item_count = 3;
    sys->strcpy(menus[0].items[0].label, "New Folder");
    sys->strcpy(menus[0].items[1].label, "New File");
    sys->strcpy(menus[0].items[2].label, "Close");
    
    sys->strcpy(menus[1].name, "Edit");
    menus[1].item_count = 1;
    sys->strcpy(menus[1].items[0].label, "Copy");

    sys->strcpy(menus[2].name, "View");
    menus[2].item_count = 1;
    sys->strcpy(menus[2].items[0].label, "Refresh");

    sys->set_window_menu(win, menus, 3, menu_cb);

    return &exports;
}