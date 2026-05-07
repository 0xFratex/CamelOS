// usr/desktop.c
#include "../sys/api.h"
#include "framework.h"
#include "../core/string.h"
#include "lib/camel_framework.h"
#include "lib/camel_ui.h"
#include "../fs/pfs32.h"
#include "desktop.h"
#include "../hal/drivers/serial.h"
#include "../hal/video/gfx_hal.h"
#include "../core/theme.h"

// DMG mounter for .dmg install support
#include "../core/dmg_mount.h"
// App installer for drag-to-Applications
#include "../core/app_installer.h"
// Selection box API for rubber-band multi-select
#include "lib/selection_box.h"

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

// Forward declaration (defined later in this file)
void desktop_apply_selection(selection_box_t* sb);

#define DESKTOP_PATH_LEGACY "/Users/Desktop"
#define GRID_START_X 30
#define GRID_START_Y 60 
#define ICON_SPACING_X 100
#define ICON_SPACING_Y 100

// Dynamic desktop path - resolved from user config at init time
char g_desktop_path[128] = "";  // Empty until properly resolved from config

// --- Desktop Selection Box (rubber-band multi-select) ---
static selection_box_t g_desk_selbox;
static int g_desk_selbox_inited = 0;

// --- Wallpaper Cache ---
// Pre-computed gradient eliminates per-pixel arithmetic every frame.
// This is the #1 fix for window flickering on move.
static uint32_t* wallpaper_cache = 0;
int wallpaper_cache_w = 0;
static int wallpaper_cache_h = 0;

int desktop_is_ctx_open() {
    return g_ctx_menu.active;
}

// Returns 1 if the desktop rubber-band selection is currently being dragged.
// Used by bubbleview.c to keep dispatching mouse events during the drag.
int desktop_selbox_active() {
    return g_desk_selbox_inited && g_desk_selbox.state == SELBOX_DRAGGING;
}

// Cancel the desktop selbox (e.g., when a double-click opens an item
// and the selbox from the first click needs to be dismissed).
void desktop_cancel_selbox() {
    if (g_desk_selbox_inited) {
        selbox_cancel(&g_desk_selbox);
    }
}

pfs32_direntry_t desk_entries[32];
int desk_count = 0;
int desk_selected[32];

void desktop_refresh() {
    uint32_t blk = 0xFFFFFFFF;
    extern int get_dir_block(const char*, uint32_t*);
    
    // Use the dynamic path (resolved from config in desktop_init)
    const char* desktop_path = g_desktop_path;
    if(get_dir_block(desktop_path, &blk) != 0) {
        // Dynamic path failed - create the full path structure
        // First ensure /Users exists
        sys_fs_create("/Users", 1);
        
        // Extract username from g_desktop_path to create /Users/<name>/Desktop
        char user_home[128];
        strcpy(user_home, g_desktop_path);
        // Find the "/Desktop" suffix and truncate to get /Users/<name>
        char* desktop_suffix = strstr(user_home, "/Desktop");
        if (desktop_suffix) {
            *desktop_suffix = 0;  // Truncate at /Desktop
            sys_fs_create(user_home, 1);
        }
        sys_fs_create(g_desktop_path, 1);
        get_dir_block(g_desktop_path, &blk);
    }
    
    // Clear old state explicitly
    desk_count = 0;
    memset(desk_entries, 0, sizeof(desk_entries));
    memset(desk_selected, 0, sizeof(desk_selected));
    
    // Preserve rename state during refresh if active
    // (desktop_refresh is called periodically and would otherwise kill rename)
    int saved_rename_active = desktop_rename_active;
    int saved_rename_idx = desktop_rename_idx;
    char saved_rename_buf[64];
    if (desktop_rename_active && desktop_rename_idx >= 0) {
        extern char desktop_rename_buf[];
        strncpy(saved_rename_buf, desktop_rename_buf, 63);
        saved_rename_buf[63] = 0;
    }

    if (blk != 0xFFFFFFFF) {
        pfs32_direntry_t temp[32];
        int raw = sys_fs_list_dir(g_desktop_path, temp, 32);
        // Deduplicate: track seen filenames to prevent duplicate entries
        char seen_names[32][64];
        int seen_count = 0;
        for(int i=0; i<raw; i++) {
            if(temp[i].filename[0] != 0 && 
               strcmp(temp[i].filename, ".") != 0 && 
               strcmp(temp[i].filename, "..") != 0) {
                // Check for duplicates
                int is_dup = 0;
                for(int j=0; j<seen_count; j++) {
                    if(strcmp(seen_names[j], temp[i].filename) == 0) {
                        is_dup = 1;
                        break;
                    }
                }
                if(!is_dup && desk_count < 32) {
                    strncpy(seen_names[seen_count], temp[i].filename, 63);
                    seen_names[seen_count][63] = 0;
                    seen_count++;
                    desk_entries[desk_count++] = temp[i];
                }
            }
        }
    }
    
    // Restore rename state after refresh (if it was active before)
    if (saved_rename_active) {
        desktop_rename_active = 1;
        desktop_rename_idx = saved_rename_idx;
        if (saved_rename_idx >= 0 && saved_rename_idx < desk_count) {
            extern char desktop_rename_buf[];
            extern int desktop_rename_cursor;
            strncpy(desktop_rename_buf, saved_rename_buf, 63);
            desktop_rename_buf[63] = 0;
            desktop_rename_cursor = strlen(saved_rename_buf);
        } else {
            // The renamed item is no longer in the directory listing
            desktop_rename_active = 0;
            desktop_rename_idx = -1;
        }
    }
}

void desktop_init() {
    // Read username from config to build the correct desktop path
    char buf[1024];
    int len = sys_fs_read("/Library/Preferences/system.conf", buf, sizeof(buf)-1);
    if (len <= 0) {
        // Try legacy path
        len = sys_fs_read("/etc/system.conf", buf, sizeof(buf)-1);
    }
    if (len > 0) {
        buf[len] = 0;
        char* line = strstr(buf, "username=");
        if (line) {
            char username[64];
            strncpy(username, line + 9, sizeof(username)-1);
            username[sizeof(username)-1] = 0;
            // Strip newline/carriage return
            char* nl = username;
            while (*nl && *nl != '\n' && *nl != '\r') nl++;
            *nl = 0;
            if (username[0]) {
                // Build /Users/<username>/Desktop
                strcpy(g_desktop_path, "/Users/");
                strcat(g_desktop_path, username);
                strcat(g_desktop_path, "/Desktop");
            }
        }
    }
    
    // If we couldn't resolve a username from config, use a sensible default.
    // Do NOT use /Users/Desktop (which causes duplicate desktop folder).
    if (g_desktop_path[0] == 0) {
        strcpy(g_desktop_path, "/Users/Shared/Desktop");
    }
    
    // Ensure the path exists
    if (!sys_fs_exists(g_desktop_path)) {
        // Create parent directories
        char parent[128];
        strcpy(parent, g_desktop_path);
        char* last_slash = strrchr(parent, '/');
        if (last_slash) {
            *last_slash = 0;
            sys_fs_create(parent, 1);
        }
        sys_fs_create(g_desktop_path, 1);
    }
    
    desktop_refresh();

    // Initialize the desktop rubber-band selection box
    if (!g_desk_selbox_inited) {
        selbox_init(&g_desk_selbox);
        g_desk_selbox_inited = 1;
    }
}
// Called lazily on first desktop_draw or desktop_fill_wallpaper_region.
static void wallpaper_cache_ensure(int w, int h) {
    if (wallpaper_cache && wallpaper_cache_w == w && wallpaper_cache_h == h) return;
    if (!wallpaper_cache) {
        wallpaper_cache = (uint32_t*)kmalloc(w * h * 4);
        if (!wallpaper_cache) return;  // fallback to per-pixel
        wallpaper_cache_w = w;
        wallpaper_cache_h = h;
    }
    // Fill cached gradient using theme desktop_bg color
    const theme_t* theme = theme_get_current();
    uint32_t base = theme->desktop_bg;
    for(int y=0; y<h; y++) {
        uint32_t col = base - (y/4); // Gradient from base color
        for(int x=0; x<w; x++) wallpaper_cache[y*w+x] = col;
    }
}

// Fill a rectangular region of the back buffer from the wallpaper cache.
// Used by the dirty-region drag optimisation in bubbleview.c.
void desktop_fill_wallpaper_region(uint32_t* buffer, int rx, int ry, int rw, int rh) {
    int w = gfx_ctx.width;
    int h = gfx_ctx.height;
    wallpaper_cache_ensure(w, h);
    if (!wallpaper_cache) {
        // Fallback: compute per-pixel for just the region using theme color
        const theme_t* theme_fb = theme_get_current();
        uint32_t base_fb = theme_fb->desktop_bg;
        for(int y=ry; y<ry+rh && y<h; y++) {
            uint32_t col = base_fb - (y/4);
            for(int x=rx; x<rx+rw && x<w; x++) buffer[y*w+x] = col;
        }
        return;
    }
    // Clip region to screen bounds
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > w) rw = w - rx;
    if (ry + rh > h) rh = h - ry;
    if (rw <= 0 || rh <= 0) return;

    int row_bytes = rw * 4;
    for(int y = 0; y < rh; y++) {
        memcpy(&buffer[(ry + y) * w + rx],
               &wallpaper_cache[(ry + y) * w + rx],
               row_bytes);
    }
}

// Draw desktop icons on top of whatever is currently in the buffer.
// Called after desktop_draw (full) or desktop_fill_wallpaper_region (dirty region).
void desktop_draw_icons(uint32_t* buffer) {
    int x = GRID_START_X;
    int y = GRID_START_Y;

    for(int i=0; i<desk_count; i++) {
        // Always repaint the icon's background from the wallpaper cache first.
        // This prevents stale highlights or ghost pixels from a previous frame
        // when using the dirty-region optimisation during window dragging.
        desktop_fill_wallpaper_region(buffer, x - 10, y - 5, 68, 80);

        // Selection Highlight
        if(desk_selected[i] && !(desktop_rename_active && desktop_rename_idx == i)) {
            sys_gfx_rect(x-10, y-5, 68, 80, 0x40FFFFFF);
        }

        const char* icon = (desk_entries[i].attributes & 0x10) ? "folder" : "file";
        // Check for .app extension - show as app icon (not installer)
        int len = strlen(desk_entries[i].filename);
        if(len > 4 && strcmp(desk_entries[i].filename + len - 4, ".app") == 0) icon = "terminal";
        // Check for .dmg extension - show as disk image
        if(len > 4 && strcmp(desk_entries[i].filename + len - 4, ".dmg") == 0) icon = "hdd_icon";
        // Check for .cdl extension - show as app
        if(len > 4 && strcmp(desk_entries[i].filename + len - 4, ".cdl") == 0) icon = "terminal";

        cm_draw_image(buffer, icon, x, y, 48, 48);

        // --- RENAME LOGIC FIX ---
        if (desktop_rename_active && desktop_rename_idx == i) {
            int text_w = strlen(desktop_rename_buf) * 8;
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
            int text_w = strlen(desk_entries[i].filename) * 8;
            int label_x = x + 24 - (text_w / 2);
            sys_gfx_string(label_x+1, y+53, desk_entries[i].filename, 0xFF000000); // Shadow
            sys_gfx_string(label_x, y+52, desk_entries[i].filename, 0xFFFFFFFF);   // Text
        }
        
        y += ICON_SPACING_Y;
        if (y > 600) { y = GRID_START_Y; x += ICON_SPACING_X; }
    }
}

void desktop_draw(uint32_t* buffer) {
    int w = gfx_ctx.width;
    int h = gfx_ctx.height;

    // 1. Wallpaper — use cached gradient (fast memcpy instead of per-pixel arithmetic)
    wallpaper_cache_ensure(w, h);
    if (wallpaper_cache) {
        memcpy(buffer, wallpaper_cache, w * h * 4);
    } else {
        // Fallback if kmalloc failed — use theme color
        const theme_t* theme_dd = theme_get_current();
        uint32_t base_dd = theme_dd->desktop_bg;
        for(int y=0; y<h; y++) {
            uint32_t col = base_dd - (y/4);
            for(int x=0; x<w; x++) buffer[y*w+x] = col;
        }
    }

    // 2. Draw Icons
    desktop_draw_icons(buffer);

    // 3. Draw Selection Box (rubber-band) if active
    if (g_desk_selbox_inited) {
        selbox_draw(&g_desk_selbox);
    }
}

void desktop_on_mouse(int mx, int my, int lb, int rb) {
    // Initialize selbox on first call if not yet done
    if (!g_desk_selbox_inited) {
        selbox_init(&g_desk_selbox);
        g_desk_selbox_inited = 1;
    }

    if (rb) {
        // Right-click: cancel any active selection and show context menu
        selbox_cancel(&g_desk_selbox);

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
            static char path_buf[128];
            strcpy(path_buf, g_desktop_path);
            strcat(path_buf, "/");
            strcat(path_buf, desk_entries[hit_idx].filename);
            
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
            return;
        }

        // If the selbox is already being dragged, just update it.
        // This handles the continuous drag dispatch from bubbleview.c
        // where desktop_on_mouse is called every frame while lb is held.
        if (g_desk_selbox.state == SELBOX_DRAGGING) {
            selbox_update(&g_desk_selbox, mx, my);
            desktop_apply_selection(&g_desk_selbox);
            return;
        }

        // Start a new selbox drag from this point.  We always start the
        // selbox — even if the click is on an icon — so the user can
        // drag-select starting near icons.  The selbox_start coordinates
        // are recorded; on release, if the user didn't drag far enough
        // for the selbox to be visible (min_drag threshold), we treat it
        // as a simple click and handle icon selection instead.
        selbox_start(&g_desk_selbox, mx, my);
    } else {
        // Mouse button released — finish any active selection drag.
        if (g_desk_selbox.state == SELBOX_DRAGGING) {
            // Check if this was a real drag or just a click (no movement)
            int rx, ry, rw, rh;
            int was_drag = 0;
            if (selbox_get_rect(&g_desk_selbox, &rx, &ry, &rw, &rh)) {
                if (rw >= g_desk_selbox.min_drag || rh >= g_desk_selbox.min_drag) {
                    was_drag = 1;
                }
            }

            selbox_end(&g_desk_selbox);

            if (!was_drag) {
                // User clicked without dragging — treat as a simple click.
                // Check if an icon was hit and select it.
                int x = GRID_START_X;
                int y = GRID_START_Y;
                for(int i=0; i<desk_count; i++) {
                    if (mx >= x && mx <= x+48 && my >= y && my <= y+60) {
                        memset(desk_selected, 0, sizeof(desk_selected));
                        desk_selected[i] = 1;
                        return;
                    }
                    y += ICON_SPACING_Y;
                    if (y > 600) { y = GRID_START_Y; x += ICON_SPACING_X; }
                }
                // Clicked on empty space without dragging — deselect all
                memset(desk_selected, 0, sizeof(desk_selected));
            }
            // If it was a drag, the selection was already applied by
            // desktop_apply_selection() during the drag. Items stay
            // highlighted via desk_selected[] which is independent
            // of the selbox state.
        }
    }
}

// Apply the current selection box to desktop icons — select any icon
// whose bounding rectangle intersects the selection rubber-band.
void desktop_apply_selection(selection_box_t* sb) {
    if (!sb || sb->state != SELBOX_DRAGGING) return;

    int rx, ry, rw, rh;
    if (!selbox_get_rect(sb, &rx, &ry, &rw, &rh)) return;

    // Minimum size threshold to avoid accidental micro-selections
    if (rw < sb->min_drag && rh < sb->min_drag) return;

    // Clear all selections first, then re-select icons that intersect
    memset(desk_selected, 0, sizeof(desk_selected));

    int x = GRID_START_X;
    int y = GRID_START_Y;
    for(int i=0; i<desk_count; i++) {
        // Icon bounding box: (x, y) to (x+48, y+60)
        // Check intersection with selection rect
        if (x < rx + rw && x + 48 > rx && y < ry + rh && y + 60 > ry) {
            desk_selected[i] = 1;
        }
        y += ICON_SPACING_Y;
        if (y > 600) { y = GRID_START_Y; x += ICON_SPACING_X; }
    }
}
// =====================================================================
// DRAG-TO-APPLICATIONS INSTALL MECHANISM
// macOS-like: drag .app to Applications folder to install
// =====================================================================

// Check if a screen position is in the "Applications drop zone"
// This is the dock area where apps can be dropped to install
int desktop_is_droppable(int x, int y) {
    // Drop zone is the dock area (bottom of screen)
    if (y > 768 - 120 && x > 256 && x < 768) {
        return 1;
    }
    return 0;
}

// Install an app by copying/moving it to /Applications/
// Supports: .app bundles, .cdl legacy apps, .dmg disk images
void desktop_install_app(const char* source_path) {
    if (!source_path || !source_path[0]) return;
    
    int len = strlen(source_path);
    
    // Ensure /Applications directory exists
    sys_fs_create("/Applications", 1);
    
    // Check what type of file we're installing
    if (len > 4 && strcmp(source_path + len - 4, ".dmg") == 0) {
        // DMG file - use the app_installer module for drag-to-Applications
        s_printf("[Desktop] Installing DMG: ");
        s_printf(source_path);
        s_printf("\n");
        
        install_result_t result = app_installer_quick_install(source_path);
        if (result.success) {
            s_printf("[Desktop] App installed successfully: ");
            s_printf(result.app_path);
            s_printf(" (");
            char size_buf[16];
            extern void int_to_str(int, char*);
            int_to_str(result.bytes_copied, size_buf);
            s_printf(size_buf);
            s_printf(" bytes)\n");
        } else {
            s_printf("[Desktop] DMG install failed: ");
            s_printf(result.error_msg);
            s_printf("\n");
        }
        
    } else if (len > 4 && strcmp(source_path + len - 4, ".app") == 0) {
        // .app bundle - copy to /Applications/
        s_printf("[Desktop] Installing .app: ");
        s_printf(source_path);
        s_printf("\n");
        
        // Extract app name from path
        const char* name_start = source_path;
        const char* p = source_path;
        while (*p) { if (*p == '/') name_start = p + 1; p++; }
        
        char dest_path[256];
        strcpy(dest_path, "/Applications/");
        strcat(dest_path, name_start);
        
        // Create the .app bundle structure in /Applications
        sys_fs_create(dest_path, 1);
        
        // Create Contents/MacOS directory
        char contents_path[256];
        strcpy(contents_path, dest_path);
        strcat(contents_path, "/Contents");
        sys_fs_create(contents_path, 1);
        
        char macos_path[256];
        strcpy(macos_path, contents_path);
        strcat(macos_path, "/MacOS");
        sys_fs_create(macos_path, 1);
        
        // Create Resources directory
        char resources_path[256];
        strcpy(resources_path, contents_path);
        strcat(resources_path, "/Resources");
        sys_fs_create(resources_path, 1);
        
        // Write Info.plist
        char plist_path[256];
        strcpy(plist_path, dest_path);
        strcat(plist_path, "/Info.plist");
        
        char app_name[64];
        strncpy(app_name, name_start, strlen(name_start) - 4); // Remove .app
        app_name[strlen(name_start) - 4] = 0;
        
        char plist_content[512];
        int pos = 0;
        pos += sprintf(plist_content + pos, "# CamelOS App Bundle Info\n");
        pos += sprintf(plist_content + pos, "CFBundleName=%s\n", app_name);
        pos += sprintf(plist_content + pos, "CFBundleIdentifier=com.camelos.%s\n", app_name);
        pos += sprintf(plist_content + pos, "CFBundleExecutable=%s\n", app_name);
        pos += sprintf(plist_content + pos, "CFBundleVersion=1.0\n");
        pos += sprintf(plist_content + pos, "CFBundleType=cdl\n");
        pos += sprintf(plist_content + pos, "CFBundleMinOSVersion=3.0\n");
        
        sys_fs_write(plist_path, plist_content, pos);
        
        // Copy the executable from source if it exists
        // Try source .app/Contents/MacOS/<name>
        char src_exec[256];
        strcpy(src_exec, source_path);
        strcat(src_exec, "/Contents/MacOS/");
        strcat(src_exec, app_name);
        
        char dst_exec[256];
        strcpy(dst_exec, macos_path);
        strcat(dst_exec, "/");
        strcat(dst_exec, app_name);
        
        // Try to copy the executable
        if (sys_fs_exists(src_exec)) {
            // Read source and write to destination
            char exec_buf[8192];
            int exec_size = sys_fs_read(src_exec, exec_buf, sizeof(exec_buf));
            if (exec_size > 0) {
                sys_fs_write(dst_exec, exec_buf, exec_size);
            }
        } else {
            // Try legacy .cdl path
            char cdl_path[256];
            strcpy(cdl_path, "/usr/apps/");
            strcat(cdl_path, app_name);
            strcat(cdl_path, ".cdl");
            
            if (sys_fs_exists(cdl_path)) {
                char exec_buf[8192];
                int exec_size = sys_fs_read(cdl_path, exec_buf, sizeof(exec_buf));
                if (exec_size > 0) {
                    sys_fs_write(dst_exec, exec_buf, exec_size);
                }
            }
        }
        
        s_printf("[Desktop] App installed to ");
        s_printf(dest_path);
        s_printf("\n");
        
    } else if (len > 4 && strcmp(source_path + len - 4, ".cdl") == 0) {
        // Legacy CDL app - wrap it in a .app bundle and install
        s_printf("[Desktop] Installing CDL app: ");
        s_printf(source_path);
        s_printf("\n");
        
        // Extract name without .cdl
        const char* name_start = source_path;
        const char* p = source_path;
        while (*p) { if (*p == '/') name_start = p + 1; p++; }
        
        char app_name[64];
        int name_len = strlen(name_start) - 4; // Remove .cdl
        if (name_len > 63) name_len = 63;
        strncpy(app_name, name_start, name_len);
        app_name[name_len] = 0;
        
        // Create the .app wrapper
        char wrapper_path[256];
        strcpy(wrapper_path, "/Applications/");
        strcat(wrapper_path, app_name);
        strcat(wrapper_path, ".app");
        
        // Use the install_app recursively with .app extension
        // But first create the source .app path temporarily
        desktop_install_app(wrapper_path);
        
    } else {
        s_printf("[Desktop] Unknown app format: ");
        s_printf(source_path);
        s_printf("\n");
    }
    
    // Refresh desktop to show new app
    desktop_refresh();
}
