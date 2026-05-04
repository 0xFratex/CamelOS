// usr/bubbleview.c - Main GUI Event Loop
// Enhanced with screenlock integration, welcome setup, and macOS-like paths
#include "../sys/api.h"
#include "framework.h"
#include "dock.h"
#include "../core/string.h"
#include "lib/camel_framework.h"
#include "lib/camel_ui.h"
#include "../hal/video/gfx_hal.h"
#include "../core/window_server.h"
#include "../hal/video/compositor.h"
#include "../hal/video/animation.h"
#include "../core/app_switcher.h"
#include "desktop.h" // For desktop_is_ctx_open
#include "screenlock.h"
#include "welcome_setup.h"

// Extern for destroying window
extern void ws_destroy_window(window_t* win);

// Forward declarations
void desktop_refresh();

// Launch helper
// macOS-like application paths (both /Applications/ and legacy /usr/apps/ supported)
#define APP_FILES       "/Applications/Files.app"
#define APP_TERMINAL    "/Applications/Terminal.app"
#define APP_TEXTEDIT    "/Applications/TextEdit.app"
#define APP_BROWSER     "/Applications/Browser.app"
#define APP_SETTINGS    "/Applications/Settings.app"
#define APP_LEGACY_PREFIX "/usr/apps/"
#define APP_NEW_PREFIX    "/Applications/"

// Resolve app path - try macOS path first, fall back to legacy
static const char* resolve_app_path(const char* app_name) {
    static char resolved[128];
    // Safely construct path with bounds checking
    int name_len = strlen(app_name);
    if (name_len > 110) name_len = 110;  // Leave room for prefix
    // Try /Applications/ first
    strcpy(resolved, APP_NEW_PREFIX);
    strncat(resolved, app_name, name_len);
    resolved[127] = 0;
    if (sys_fs_exists(resolved)) return resolved;
    // Try legacy /usr/apps/
    strcpy(resolved, APP_LEGACY_PREFIX);
    strncat(resolved, app_name, name_len);
    resolved[127] = 0;
    if (sys_fs_exists(resolved)) return resolved;
    // Return new path as default even if doesn't exist yet
    strcpy(resolved, APP_NEW_PREFIX);
    strncat(resolved, app_name, name_len);
    resolved[127] = 0;
    return resolved;
}

void desktop_execute_item(const char* path, int is_dir) {
    extern int wrap_exec_with_args(const char*, const char*);
    extern int wrap_exec(const char*);
    
    if (is_dir) {
        // Open Files App pointing to this path
        wrap_exec_with_args(resolve_app_path("Files.app"), path);
    } else {
        int len = strlen(path);
        if (len > 4 && strcmp(path + len - 4, ".app") == 0) {
            wrap_exec(path);
        } else if (len > 4 && strcmp(path + len - 4, ".dmg") == 0) {
            // DMG file - use the app installer for drag-to-Applications
            extern int app_installer_open_dmg(const char*);
            app_installer_open_dmg(path);
        } else {
            // Open with TextEdit by default for text files
            wrap_exec_with_args(resolve_app_path("TextEdit.app"), path);
        }
    }
}

// Extern g_kernel_api from cdl_loader.c
extern kernel_api_t g_kernel_api;

// Extern desk_entries from desktop.c
extern pfs32_direntry_t desk_entries[32];
extern int desk_count;

#define HEADER_HEIGHT 28
#define RESIZE_MARGIN 16
#define SNAP_MARGIN 20 // Distance to edge to trigger snap
#define SNAP_PREVIEW_COLOR 0x4088AAFF // Transparent Blue

// Animation Constants
#define ANIM_SPEED 10

// -- State --
static int prev_lb = 0;
static int prev_rb = 0;

// Double Click State
static int last_click_time = 0;
static int frame_counter = 0; // Simple tick simulation
static int last_select_idx = -1; // New: track for "select -> click again" logic

// --- Config ---
#define STARTUP_GRACE_FRAMES 30 // Wait 0.5s before accepting mouse clicks

static int frames_drawn = 0;

// Menu State
static int open_menu_id = -2;
static int menu_rect_x = 0;
static int menu_rect_y = 0;
static int menu_rect_w = 0;
static int menu_rect_h = 0;

// Snapping State
static int snap_preview_active = 0;
static rect_t snap_preview_rect = {0,0,0,0};

// Auto Refresh
static uint32_t last_fs_gen = 0;

// Rename State
static int renaming_mode = 0;
static char rename_buffer[64] = {0};
static int rename_cursor = 0;
static int rename_target_idx = -1;

// Exported rename state for desktop.c
int desktop_rename_active = 0;
int desktop_rename_idx = -1;
char desktop_rename_buf[64] = {0};
int desktop_rename_cursor = 0;

// Context Menu State (Enhanced)
typedef struct {
    char label[32];
    int action_id; // 1=New Folder, 2=New File, 3=Rename, 4=Delete, 5=Copy, 6=Paste
    int enabled;
    int has_submenu; // Indicates if the item has a submenu
    int submenu_count; // Number of items in the submenu
    char submenu_items[5][32]; // Submenu items
} ContextMenuItem;

typedef struct {
    int active;
    int x, y, w, h;
    int item_count;
    ContextMenuItem items[10];
    void* target_obj; // Pointer to file entry or window
    int target_type;  // 0=Desktop, 1=File, 2=Window
    // Submenu State
    int submenu_active;
    int submenu_x, submenu_y;
    int submenu_parent_idx; // Index of the parent item in the main menu
} ContextMenuState;

ContextMenuState g_ctx_menu;

// "Open With" Submenu Items
static const char* open_with_apps[] = { "TextEdit", "Terminal", "Files" };
static int open_with_count = 3;

// File Clipboard
static char clip_file_path[128];
static int clip_is_cut = 0;
static int clip_active = 0;

// Window Dragging & Resizing
static window_t* drag_win = 0;
static int drag_off_x = 0;
static int drag_off_y = 0;

// Dirty-region tracking for drag optimisation
// Records the window's position BEFORE the drag update so we can
// repaint only the union of old+new rects instead of the whole screen.
static int drag_prev_x = 0, drag_prev_y = 0;
static int drag_was_active = 0;   // 1 when the previous frame was a drag frame
static int drag_just_released = 0; // 1 when drag was released this frame (need final dirty region)

#define DRAG_SHADOW_PAD 12       // extra pixels around the window for shadow

static window_t* resize_win = 0;
static int resize_orig_w, resize_orig_h;
static int resize_mx, resize_my;

static int cursor_type = 0; // 0=arrow, 1=resize

// System Menu Items
static char* sys_menu_items[] = { "About Camel OS", "-", "Restart", "Shutdown" };
static int sys_menu_count = 4;
static char* def_menus[] = { "File", "Edit", "View", "Window", "Help" };

int measure_text_width(const char* str) { return strlen(str) * 8; }

// --- Cursor Drawing ---
// Draw a crisp pixel-perfect macOS-style arrow cursor
// Arrow pointing top-left, ~12x19 pixels
static void draw_cursor(int mx, int my) {
    if (cursor_type == 1) {
        // Resize cursor (diagonal double-arrow) 13x13
        static const uint8_t resize_shape[13][13] = {
            {1,0,0,0,0,0,0,0,0,0,0,0,1},
            {0,2,0,0,0,0,0,0,0,0,0,2,0},
            {0,0,2,0,0,0,0,0,0,0,2,0,0},
            {0,0,0,2,0,0,0,0,0,2,0,0,0},
            {0,0,0,0,2,0,0,0,2,0,0,0,0},
            {0,0,0,0,0,2,0,2,0,0,0,0,0},
            {0,0,0,0,0,0,1,0,0,0,0,0,0},
            {0,0,0,0,0,2,0,2,0,0,0,0,0},
            {0,0,0,0,2,0,0,0,2,0,0,0,0},
            {0,0,0,2,0,0,0,0,0,2,0,0,0},
            {0,0,2,0,0,0,0,0,0,0,2,0,0},
            {0,2,0,0,0,0,0,0,0,0,0,2,0},
            {1,0,0,0,0,0,0,0,0,0,0,0,1},
        };
        for (int row = 0; row < 13; row++) {
            for (int col = 0; col < 13; col++) {
                int px = mx + col;
                int py = my + row;
                if (px < 0 || px >= 1024 || py < 0 || py >= 768) continue;
                uint8_t v = resize_shape[row][col];
                if (v == 1) gfx_put_pixel(px, py, 0xFF000000);
                else if (v == 2) gfx_put_pixel(px, py, 0xFFFFFFFF);
            }
        }
        return;
    }
    // Classic arrow cursor shape (1=black outline, 2=white fill)
    // Row 0: 1 pixel at tip
    // Each row builds the arrow shape
    static const uint8_t cursor_shape[19][12] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},  // row 0: tip
        {1,1,0,0,0,0,0,0,0,0,0,0},  // row 1
        {1,2,1,0,0,0,0,0,0,0,0,0},  // row 2
        {1,2,2,1,0,0,0,0,0,0,0,0},  // row 3
        {1,2,2,2,1,0,0,0,0,0,0,0},  // row 4
        {1,2,2,2,2,1,0,0,0,0,0,0},  // row 5
        {1,2,2,2,2,2,1,0,0,0,0,0},  // row 6
        {1,2,2,2,2,2,2,1,0,0,0,0},  // row 7
        {1,2,2,2,2,2,2,2,1,0,0,0},  // row 8
        {1,2,2,2,2,2,2,2,2,1,0,0},  // row 9
        {1,2,2,2,2,2,2,2,2,2,1,0},  // row 10
        {1,2,2,2,2,2,1,1,1,1,1,1},  // row 11: start of tail
        {1,2,2,2,1,2,2,1,0,0,0,0},  // row 12
        {1,2,2,1,0,1,2,2,1,0,0,0},  // row 13
        {1,2,1,0,0,1,2,2,1,0,0,0},  // row 14
        {1,1,0,0,0,0,1,2,2,1,0,0},  // row 15
        {1,0,0,0,0,0,1,2,2,1,0,0},  // row 16
        {0,0,0,0,0,0,0,1,2,1,0,0},  // row 17
        {0,0,0,0,0,0,0,1,1,0,0,0},  // row 18
    };

    for (int row = 0; row < 19; row++) {
        for (int col = 0; col < 12; col++) {
            int px = mx + col;
            int py = my + row;
            if (px < 0 || px >= 1024 || py < 0 || py >= 768) continue;
            uint8_t v = cursor_shape[row][col];
            if (v == 1) {
                gfx_put_pixel(px, py, 0xFF000000);  // Black outline
            } else if (v == 2) {
                gfx_put_pixel(px, py, 0xFFFFFFFF);  // White fill
            }
        }
    }
}

// Context Menu Functions
void ctx_menu_init() {
    g_ctx_menu.active = 0;
    clip_active = 0;
}

// === MODIFIED: Added Open With items and submenu support ===
void ctx_menu_show(int x, int y, int type, void* target) {
    g_ctx_menu.x = x;
    g_ctx_menu.y = y;
    g_ctx_menu.target_type = type;
    g_ctx_menu.target_obj = target;
    g_ctx_menu.w = 180; // Widen for "Open With..."
    g_ctx_menu.submenu_active = 0; // Reset submenu state

    if (type == 0) { // Desktop Background
        g_ctx_menu.item_count = 4;
        strcpy(g_ctx_menu.items[0].label, "New Folder"); g_ctx_menu.items[0].action_id = 1;
        strcpy(g_ctx_menu.items[1].label, "New File");   g_ctx_menu.items[1].action_id = 2;
        strcpy(g_ctx_menu.items[2].label, "-");          g_ctx_menu.items[2].action_id = 0;
        strcpy(g_ctx_menu.items[3].label, "Paste");      g_ctx_menu.items[3].action_id = 6;
        g_ctx_menu.items[3].enabled = clip_active;
    }
    else if (type == 1) { // File/Icon
        g_ctx_menu.item_count = 6; // Open, Open With, -, Copy, Rename, Delete
        strcpy(g_ctx_menu.items[0].label, "Open");             g_ctx_menu.items[0].action_id = 10;
        
        // "Open With" submenu
        strcpy(g_ctx_menu.items[1].label, "Open With >");
        g_ctx_menu.items[1].action_id = 99; // Special ID for submenu
        g_ctx_menu.items[1].has_submenu = 1;
        g_ctx_menu.items[1].submenu_count = open_with_count;
        for (int i = 0; i < open_with_count; i++) {
            strcpy(g_ctx_menu.items[1].submenu_items[i], open_with_apps[i]);
        }
        
        strcpy(g_ctx_menu.items[2].label, "-");                g_ctx_menu.items[2].action_id = 0;
        strcpy(g_ctx_menu.items[3].label, "Copy");             g_ctx_menu.items[3].action_id = 5;
        strcpy(g_ctx_menu.items[4].label, "Rename");           g_ctx_menu.items[4].action_id = 3;
        strcpy(g_ctx_menu.items[5].label, "Delete");           g_ctx_menu.items[5].action_id = 4;
    }

    if (g_ctx_menu.x + g_ctx_menu.w > 1024) g_ctx_menu.x = 1024 - g_ctx_menu.w;
    if (g_ctx_menu.x < 0) g_ctx_menu.x = 0;
    g_ctx_menu.h = g_ctx_menu.item_count * 24 + 10;
    if (g_ctx_menu.y + g_ctx_menu.h > 768) g_ctx_menu.y = 768 - g_ctx_menu.h;
    if (g_ctx_menu.y < 0) g_ctx_menu.y = 0;
    g_ctx_menu.active = 1;
}

// --- CLOCK WIDGET ---
void draw_system_clock() {
    int h, m, s;
    sys_get_time(&h, &m, &s);

    char time_str[16];
    char buf[4];

    // Format HH:MM
    // Simple logic for AM/PM if desired, keeping 24h for simplicity code
    int_to_str(h, buf);
    strcpy(time_str, (h<10) ? "0" : "");
    strcat(time_str, buf);
    strcat(time_str, ":");

    int_to_str(m, buf);
    strcat(time_str, (m<10) ? "0" : "");
    strcat(time_str, buf);

    int w = measure_text_width(time_str);
    int x = 1024 - w - 15; // Right align

    sys_gfx_string(x, 8, time_str, 0xFF000000);
}

// --- Logic Helpers ---

void win_minimize(window_t* w) {
    if(!w) return;
    w->state = WIN_STATE_MINIMIZED;
    w->is_focused = 0;
    active_win = 0;
    w->anim_state = 3; // Minimizing
    w->anim_t = 0.0f;
}

void win_maximize(window_t* w) {
    if(!w) return;

    if (w->state == WIN_STATE_MAXIMIZED) {
        // Restore
        w->x = w->saved_x;
        w->y = w->saved_y;
        w->width = w->saved_w;
        w->height = w->saved_h;
        w->state = WIN_STATE_NORMAL;
    } else {
        // Save current state
        w->saved_x = w->x;
        w->saved_y = w->y;
        w->saved_w = w->width;
        w->saved_h = w->height;

        // Maximize (account for Header bar and Dock)
        w->x = 0;
        w->y = HEADER_HEIGHT;
        w->width = 1024;
        w->height = 768 - HEADER_HEIGHT - 70; // Leave room for dock
        w->state = WIN_STATE_MAXIMIZED;
    }
}

// --- Logic Helpers ---

// --- REFINED WINDOW SNAPPING LOGIC ---
void handle_window_snapping(window_t* w, int mx, int my) {
    int screen_w = 1024;
    int screen_h = 768;

    snap_preview_active = 0;

    // Check edges based on mouse pointer, not just window rect
    if (mx < SNAP_MARGIN) {
        // Left Half
        snap_preview_active = 1;
        snap_preview_rect.x = 0; snap_preview_rect.y = HEADER_HEIGHT;
        snap_preview_rect.w = screen_w/2; snap_preview_rect.h = screen_h - HEADER_HEIGHT - 70;
    }
    else if (mx > screen_w - SNAP_MARGIN) {
        // Right Half
        snap_preview_active = 1;
        snap_preview_rect.x = screen_w/2; snap_preview_rect.y = HEADER_HEIGHT;
        snap_preview_rect.w = screen_w/2; snap_preview_rect.h = screen_h - HEADER_HEIGHT - 70;
    }
    else if (my < SNAP_MARGIN + HEADER_HEIGHT && my > HEADER_HEIGHT) {
        // Top Edge -> Maximize
        snap_preview_active = 1;
        snap_preview_rect.x = 0; snap_preview_rect.y = HEADER_HEIGHT;
        snap_preview_rect.w = screen_w; snap_preview_rect.h = screen_h - HEADER_HEIGHT - 70;
    }
}

void apply_snap(window_t* w) {
    if (!snap_preview_active) return;

    w->saved_x = w->x; w->saved_y = w->y;
    w->saved_w = w->width; w->saved_h = w->height;

    w->x = snap_preview_rect.x;
    w->y = snap_preview_rect.y;
    w->width = snap_preview_rect.w;
    w->height = snap_preview_rect.h;

    snap_preview_active = 0;
}

// --- Logic Helpers ---

// --- Drawing Helpers ---

void draw_window_animated(window_t* w, int mx, int my) {
    // Skip if not visible or fully minimized without animation
    if (!w->is_visible && w->anim_state == 0) return;
    if (w->state == WIN_STATE_MINIMIZED && w->anim_state == 0) return;

    // Handle animation progress
    if (w->anim_state != 0) {
        w->anim_t += 0.1f; // Speed
        if (w->anim_t >= 1.0f) {
            w->anim_t = 1.0f;
            // Finish state transitions
            if (w->anim_state == 2) { 
                // === FIX: Destroy window on close to reset app state ===
                ws_destroy_window(w); 
                return; 
            }
            if (w->anim_state == 3) { w->state = WIN_STATE_MINIMIZED; } // Minimize
            w->anim_state = 0;
        }
    }

    // Draw based on state
    if (w->state == WIN_STATE_MINIMIZED && w->anim_state == 0) {
        return; // Don't draw fully minimized windows
    }

    if (w->anim_state != 0) {
        // Animated Draw (Genie / Fade)
        rect_t src = {w->x, w->y, w->width, w->height};
        rect_t dest;
        dock_get_window_rect(w, &dest.x, &dest.y, &dest.w, &dest.h);

        rect_t curr;
        int fp_t = (int)(w->anim_t * 65536); 
        if (w->anim_state == 3) { // Minimizing: Window -> Dock
            anim_genie_calc(src, dest, fp_t, &curr);
        } else if (w->anim_state == 2) { // Closing: Fade out / Shrink to center
             // Simple shrink logic for closing
             curr.x = w->x + (w->width/2 * fp_t)/65536;
             curr.y = w->y + (w->height/2 * fp_t)/65536;
             curr.w = w->width * (65536 - fp_t)/65536;
             curr.h = w->height * (65536 - fp_t)/65536;
        } else { // Restoring: Dock -> Window
            anim_genie_calc(dest, src, anim_ease_out_back(fp_t), &curr);
        }

        // Draw frame at calculated rect
        ui_draw_window_frame_ex(&g_kernel_api, curr.x, curr.y, curr.w, curr.h, w->title, (w == active_win), mx, my);

        // Draw Content if mostly visible
        if (w->anim_t > 0.8f && w->anim_state != 2) {
            if(w->paint_callback) {
                typedef void (*pcb)(int,int,int,int);
                gfx_set_clip(curr.x, curr.y + 30, curr.w, curr.h - 30);
                ((pcb)w->paint_callback)(curr.x, curr.y + 30, curr.w, curr.h - 30);
                gfx_reset_clip();
            }
        }
    } else {
        // Standard Draw using compositor
        compositor_draw_window(w);
        // Call paint callback for content
        if(w->paint_callback) {
            typedef void (*pcb)(int,int,int,int);
            gfx_set_clip(w->x, w->y + 30, w->width, w->height - 30);
            ((pcb)w->paint_callback)(w->x, w->y + 30, w->width, w->height - 30);
            gfx_reset_clip();
        }
    }
}

void draw_dropdown(int x, int y, char** items, int count, int is_app_menu, window_t* app_win) {
    int w = 160; 
    int h = count * 20 + 6;
    menu_rect_x = x; menu_rect_y = y; menu_rect_w = w; menu_rect_h = h;

    sys_gfx_rect(x+4, y+4, w, h, 0x40000000);
    sys_gfx_rect(x, y, w, h, 0xF2F2F2F2); 
    sys_gfx_rect(x, y, w, 1, 0xFF888888);
    sys_gfx_rect(x, y+h-1, w, 1, 0xFF888888);
    sys_gfx_rect(x, y, 1, h, 0xFF888888);
    sys_gfx_rect(x+w-1, y, 1, h, 0xFF888888);

    int mx, my, lb; sys_mouse_read(&mx, &my, &lb);

    for(int i=0; i<count; i++) {
        int iy = y + 3 + (i * 20);
        char* label = (is_app_menu && app_win) ? app_win->menus[open_menu_id].items[i].label : items[i];
        
        if(strcmp(label, "-") == 0) {
            sys_gfx_rect(x+5, iy+10, w-10, 1, 0xFFCCCCCC);
            continue;
        }

        if (mx >= x && mx < x+w && my >= iy && my < iy+20) {
            sys_gfx_rect(x, iy, w, 20, 0xFF3D89D6);
            sys_gfx_string(x + 15, iy + 6, label, 0xFFFFFFFF);
        } else {
            sys_gfx_string(x + 15, iy + 6, label, 0xFF000000);
        }
    }
}

// Global Menu Bar (Aqua White Gradient)
int process_global_bar(int mx, int my, int click) {
    for(int i=0; i<HEADER_HEIGHT; i++) {
        uint32_t col = (i < HEADER_HEIGHT/2) ? 0xFFF8F8F8 : 0xFFE8E8E8;
        sys_gfx_rect(0, i, 1024, 1, col);
    }
    sys_gfx_rect(0, HEADER_HEIGHT, 1024, 1, 0xFF888888); 

    int cur_x = 15;
    int target_menu = -3; 

    // If context menu is active, don't draw menu bar dropdowns (they would
    // overlap and cause visual corruption / flickering).
    int suppress_dropdowns = g_ctx_menu.active;

    // 1. Apple/Camel Logo
    int w = measure_text_width("Camel") + 20;
    
    sys_gfx_string(cur_x + 10, 8, "Camel", 0xFF000000);
    sys_gfx_string(cur_x + 11, 8, "Camel", 0xFF000000); 

    if (mx >= cur_x && mx < cur_x + w && my < HEADER_HEIGHT) {
        if(click) target_menu = -1;
    }

    if (open_menu_id == -1 && !suppress_dropdowns) {
        sys_gfx_rect(cur_x, 0, w, HEADER_HEIGHT, 0xFF3D89D6); 
        sys_gfx_string(cur_x + 10, 8, "Camel", 0xFFFFFFFF);
        draw_dropdown(cur_x, HEADER_HEIGHT, sys_menu_items, sys_menu_count, 0, 0);
    }
    cur_x += w;

    // 2. App Name (Bold)
    const char* app_name = (active_win) ? active_win->title : "Finder";
    w = measure_text_width(app_name) + 20;
    sys_gfx_string(cur_x + 10, 8, app_name, 0xFF000000);
    sys_gfx_string(cur_x + 11, 8, app_name, 0xFF000000);
    cur_x += w;

    // 3. Menus
    int menu_count = (active_win && active_win->menu_count > 0) ? active_win->menu_count : 5;
    
    for(int i=0; i<menu_count; i++) {
        const char* m_name = (active_win && active_win->menu_count > 0) ? active_win->menus[i].name : def_menus[i];
        w = measure_text_width(m_name) + 20;
        
        if (mx >= cur_x && mx < cur_x + w && my < HEADER_HEIGHT) {
            if (click) target_menu = i;
        }

        if (open_menu_id == i && !suppress_dropdowns) {
            sys_gfx_rect(cur_x, 0, w, HEADER_HEIGHT, 0xFF3D89D6);
            sys_gfx_string(cur_x + 10, 8, m_name, 0xFFFFFFFF);
            if (active_win) draw_dropdown(cur_x, HEADER_HEIGHT, 0, active_win->menus[i].item_count, 1, active_win);
            else draw_dropdown(cur_x, HEADER_HEIGHT, 0, 0, 0, 0); 
        } else {
            sys_gfx_string(cur_x + 10, 8, m_name, 0xFF000000);
        }
        cur_x += w;
    }

    if (click && target_menu != -3) {
        if (open_menu_id == target_menu) open_menu_id = -2;
        else open_menu_id = target_menu;
        return 1;
    }

    // 3. System Clock (Right Side)
    draw_system_clock();

    return 0;
}

void handle_dropdown_click(int mx, int my) {
    if (open_menu_id == -2) return;
    int rel_y = my - menu_rect_y - 3;
    if (rel_y < 0) return;
    int idx = rel_y / 20;

    if (open_menu_id == -1) {
        if (idx == 0) {
            // "About Camel OS" - open Settings app to About tab
            extern void init_settings_app(void);
            init_settings_app();
        }
        else if (idx == 2) sys_reboot();
        else if (idx == 3) sys_shutdown();
    } else if (active_win && active_win->on_menu_action) {
        typedef void (*mcb)(int,int);
        ((mcb)active_win->on_menu_action)(open_menu_id, idx);
    }
    open_menu_id = -2;
}

void ctx_menu_draw() {
    if (!g_ctx_menu.active) return;

    // Draw using Kernel API style
    extern kernel_api_t g_kernel_api;
    const char* labels[10];
    for(int i=0; i<g_ctx_menu.item_count; i++) labels[i] = g_ctx_menu.items[i].label;

    // Reuse existing UI draw from camel_ui.c, but we need to handle hover manually here
    int mx, my, lb; sys_mouse_read(&mx, &my, &lb);
    int hover = -1;
    if (mx >= g_ctx_menu.x && mx <= g_ctx_menu.x + g_ctx_menu.w &&
        my >= g_ctx_menu.y && my <= g_ctx_menu.y + g_ctx_menu.h) {
        hover = (my - (g_ctx_menu.y + 4)) / 24;
    }

    ui_draw_context_menu(&g_kernel_api, g_ctx_menu.x, g_ctx_menu.y, labels, g_ctx_menu.item_count, hover);

    // Draw submenu if active
    if (g_ctx_menu.submenu_active) {
        int submenu_idx = g_ctx_menu.submenu_parent_idx;
        if (submenu_idx >= 0 && submenu_idx < g_ctx_menu.item_count && g_ctx_menu.items[submenu_idx].has_submenu) {
            ContextMenuItem* parent_item = &g_ctx_menu.items[submenu_idx];
            
            // Position submenu to the right of the parent item
            g_ctx_menu.submenu_x = g_ctx_menu.x + g_ctx_menu.w;
            g_ctx_menu.submenu_y = g_ctx_menu.y + (submenu_idx * 24) + 4;
            
            // Draw submenu
            ui_draw_context_menu(&g_kernel_api, g_ctx_menu.submenu_x, g_ctx_menu.submenu_y,
                                (const char**)parent_item->submenu_items, parent_item->submenu_count, -1);
        }
    }
}

// === MODIFIED: Handle Open With Actions and submenu clicks ===
void ctx_menu_handle_click(int mx, int my) {
    if (!g_ctx_menu.active) return;

    // Check if click is outside the main menu
    if (mx < g_ctx_menu.x || mx > g_ctx_menu.x + g_ctx_menu.w ||
        my < g_ctx_menu.y || my > g_ctx_menu.y + g_ctx_menu.h) {
        // Check if click is inside the submenu
        if (g_ctx_menu.submenu_active) {
            int submenu_idx = g_ctx_menu.submenu_parent_idx;
            if (submenu_idx >= 0 && submenu_idx < g_ctx_menu.item_count && g_ctx_menu.items[submenu_idx].has_submenu) {
                ContextMenuItem* parent_item = &g_ctx_menu.items[submenu_idx];
                int submenu_x = g_ctx_menu.submenu_x;
                int submenu_y = g_ctx_menu.submenu_y;
                int submenu_w = 160;
                int submenu_h = parent_item->submenu_count * 24 + 10;
                
                if (mx >= submenu_x && mx <= submenu_x + submenu_w &&
                    my >= submenu_y && my <= submenu_y + submenu_h) {
                    int sub_idx = (my - (submenu_y + 4)) / 24;
                    if (sub_idx >= 0 && sub_idx < parent_item->submenu_count) {
                        char* target_name = (char*)g_ctx_menu.target_obj;
                        extern int wrap_exec_with_args(const char*, const char*);
                        extern int wrap_exec(const char*);
                        
                        // Null-check the target name
                        if (!target_name || !target_name[0]) {
                            g_ctx_menu.active = 0;
                            return;
                        }
                        
                        // Handle submenu item actions
                        if (strcmp(parent_item->submenu_items[sub_idx], "TextEdit") == 0) {
                            const char* app = resolve_app_path("TextEdit.app");
                            if (sys_fs_exists(app)) wrap_exec_with_args(app, target_name);
                        } else if (strcmp(parent_item->submenu_items[sub_idx], "Terminal") == 0) {
                            const char* app = resolve_app_path("Terminal.app");
                            if (sys_fs_exists(app)) wrap_exec_with_args(app, target_name);
                        } else if (strcmp(parent_item->submenu_items[sub_idx], "Files") == 0) {
                            const char* app = resolve_app_path("Files.app");
                            if (sys_fs_exists(app)) wrap_exec_with_args(app, target_name);
                        }
                    }
                }
            }
        }
        g_ctx_menu.active = 0; // Close if clicked outside
        return;
    }

    int idx = (my - (g_ctx_menu.y + 4)) / 24;
    if (idx < 0 || idx >= g_ctx_menu.item_count) return;

    // Check if the clicked item has a submenu
    if (g_ctx_menu.items[idx].has_submenu) {
        g_ctx_menu.submenu_active = 1;
        g_ctx_menu.submenu_parent_idx = idx;
        return;
    }

    int action = g_ctx_menu.items[idx].action_id;
    char* target_name = (char*)g_ctx_menu.target_obj;

    // Null-check target_name for file operations
    if (!target_name && action != 1 && action != 2 && action != 6) {
        g_ctx_menu.active = 0;
        return;
    }

    extern int wrap_exec_with_args(const char*, const char*);

    switch(action) {
        case 1: { /* New Folder */
            char new_path[256];
            char new_name[64] = "New Folder";
            // Find unique name
            int counter = 1;
            while (1) {
                strcpy(new_path, g_desktop_path);
                strcat(new_path, "/");
                strcat(new_path, new_name);
                if (!sys_fs_exists(new_path)) break;
                counter++;
                strcpy(new_name, "New Folder (");
                char num[10];
                int_to_str(counter, num);
                strcat(new_name, num);
                strcat(new_name, ")");
            }
            strcpy(new_path, g_desktop_path);
            strcat(new_path, "/");
            strcat(new_path, new_name);
            int res = sys_fs_create(new_path, 1);
            if (res == 0 || res == -5) {
                desktop_refresh();
                // Start inline rename for the newly created folder
                // Find the index of the new item
                for (int fi = 0; fi < desk_count; fi++) {
                    if (strcmp(desk_entries[fi].filename, new_name) == 0) {
                        renaming_mode = 1;
                        rename_target_idx = fi;
                        desktop_rename_active = 1;
                        desktop_rename_idx = fi;
                        strncpy(desktop_rename_buf, new_name, 63);
                        desktop_rename_buf[63] = 0;
                        desktop_rename_cursor = strlen(new_name);
                        // Select all text by default for easy replacement
                        break;
                    }
                }
            }
        } break;
        case 2: /* New File */ {
            char dp[256];
            char new_name[64] = "New_Text.txt";
            int counter = 1;
            while (1) {
                strcpy(dp, g_desktop_path);
                strcat(dp, "/");
                strcat(dp, new_name);
                if (!sys_fs_exists(dp)) break;
                counter++;
                strcpy(new_name, "New_Text (");
                char num[10];
                int_to_str(counter, num);
                strcat(new_name, num);
                strcat(new_name, ").txt");
            }
            strcpy(dp, g_desktop_path);
            strcat(dp, "/");
            strcat(dp, new_name);
            sys_fs_create(dp, 0);
            desktop_refresh();
            // Start inline rename for the newly created file
            for (int fi = 0; fi < desk_count; fi++) {
                if (strcmp(desk_entries[fi].filename, new_name) == 0) {
                    renaming_mode = 1;
                    rename_target_idx = fi;
                    desktop_rename_active = 1;
                    desktop_rename_idx = fi;
                    strncpy(desktop_rename_buf, new_name, 63);
                    desktop_rename_buf[63] = 0;
                    desktop_rename_cursor = strlen(new_name);
                    break;
                }
            }
        } break;
        case 3: /* Rename */ {
            // Find the desktop icon index for this target
            int rename_idx = -1;
            for (int ri = 0; ri < 32; ri++) {
                if (desk_entries[ri].filename[0] == 0) continue;
                static char check_path[128];
                strcpy(check_path, g_desktop_path);
                strcat(check_path, "/");
                strcat(check_path, desk_entries[ri].filename);
                if (target_name && strcmp(check_path, target_name) == 0) {
                    rename_idx = ri;
                    break;
                }
            }
            
            if (rename_idx >= 0) {
                // Set up inline rename mode (rendered below the icon in desktop.c)
                renaming_mode = 1;
                rename_target_idx = rename_idx;
                strncpy(rename_buffer, desk_entries[rename_idx].filename, 63);
                rename_buffer[63] = 0;
                rename_cursor = strlen(rename_buffer);
                
                // Export state for desktop.c inline rendering
                desktop_rename_active = 1;
                desktop_rename_idx = rename_idx;
                strncpy(desktop_rename_buf, desk_entries[rename_idx].filename, 63);
                desktop_rename_buf[63] = 0;
                desktop_rename_cursor = strlen(desktop_rename_buf);
            }
            g_ctx_menu.active = 0;
        } break;
        case 4: /* Delete */ sys_fs_delete_recursive(target_name); desktop_refresh(); break;
        case 5: /* Copy */ strcpy(clip_file_path, target_name); clip_is_cut = 0; clip_active = 1; break;
        case 6: /* Paste */ if (clip_active) { char dest[128]; strcpy(dest, g_desktop_path); strcat(dest, "/Copy_of_File"); sys_fs_copy(clip_file_path, dest); desktop_refresh(); } break;
        case 7: /* Cut */ break; // TODO
        case 10: /* Open (Default) */ desktop_execute_item(target_name, 0); break;
    }
    g_ctx_menu.active = 0;
}

// --- INPUT HANDLING ---
void handle_input(int mx, int my, int lb, int rb) {
    int click = (lb && !prev_lb);

    // Priority -1: If renaming and user clicks outside the rename area, commit the rename
    if (renaming_mode && click) {
        // Commit rename on click anywhere (treat like pressing Enter)
        if (strlen(desktop_rename_buf) > 0 && desktop_rename_idx >= 0 && desktop_rename_idx < 32) {
            extern int desk_count;
            extern pfs32_direntry_t desk_entries[];
            char old_path[256], new_path[256];
            extern char g_desktop_path[128];
            strcpy(old_path, g_desktop_path);
            strcat(old_path, "/");
            strcat(old_path, desk_entries[desktop_rename_idx].filename);
            strcpy(new_path, g_desktop_path);
            strcat(new_path, "/");
            strcat(new_path, desktop_rename_buf);
            if (strcmp(old_path, new_path) != 0) {
                extern int sys_fs_rename(const char*, const char*);
                sys_fs_rename(old_path, new_path);
            }
        }
        renaming_mode = 0;
        desktop_rename_active = 0;
        desktop_rename_idx = -1;
        desktop_refresh();
        // Don't process the click further
        prev_lb = lb;
        return;
    }

    // Priority 0: Unified Context Menu (Replaces previous modal check)
    if (g_ctx_menu.active && click) {
        ctx_menu_handle_click(mx, my);
        return; // Block other inputs
    }

    // 0. Modal Desktop Context Check
    // If the desktop context menu is open, it captures the next click (anywhere)
    // to close itself. This prevents clicks passing through to windows.
    if (desktop_is_ctx_open()) {
        desktop_on_mouse(mx, my, lb, rb);
        return;
    }

    // 1. Resizing (Highest Priority Drag)
    if (resize_win) {
        if (lb) {
            int dx = mx - resize_mx;
            int dy = my - resize_my;
            int new_w = resize_orig_w + dx;
            int new_h = resize_orig_h + dy;

            if (new_w < resize_win->min_w) new_w = resize_win->min_w;
            if (new_h < resize_win->min_h) new_h = resize_win->min_h;

            resize_win->width = new_w;
            resize_win->height = new_h;
            return;
        } else {
            // Resize released - notify the window via resize_callback
            if (resize_win && resize_win->resize_callback) {
                typedef void (*rcb)(int, int);
                ((rcb)resize_win->resize_callback)(resize_win->width, resize_win->height);
            }
            resize_win = 0; // Release
        }
    }

    // 2. Menu Interactions
    if (open_menu_id != -2 && click) {
        if (mx >= menu_rect_x && mx <= menu_rect_x + menu_rect_w &&
            my >= menu_rect_y && my <= menu_rect_y + menu_rect_h) {
            handle_dropdown_click(mx, my);
        } else if (my > HEADER_HEIGHT) {
            open_menu_id = -2; 
        }
        return;
    }

    // 3. Header Bar
    if (my < HEADER_HEIGHT && click) {
        if (process_global_bar(mx, my, 1)) return;
    }

    // 4. Dock Interactions
    if (my > 768 - 100) {
        if (click) {
            if (dock_handle_click(mx, my, 1024, 768)) return;
        }
    }

    // 5. Window Dragging
    if (drag_win) {
        if (lb) {
            drag_win->x = mx - drag_off_x;
            drag_win->y = my - drag_off_y;

            // Check for snapping preview
            handle_window_snapping(drag_win, mx, my);

            return;
        } else {
            // Mouse Released
            if (snap_preview_active) {
                apply_snap(drag_win);
            }
            // Mark drag just released so the render loop can do one final
            // dirty-region repaint instead of a slower full-screen redraw.
            drag_just_released = 1;
            drag_win = 0;
            snap_preview_active = 0;
        }
    }

    // 6. Window Logic
    if (click || (rb && !prev_rb)) {
        int btn = click ? 1 : 2; 
        
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            window_t* w = ws_get_window_at_index(i);
            // Ignore non-visible windows
            if (!w || !w->is_visible) continue;
            // Skip fully minimized windows (unless animating)
            if (w->state == WIN_STATE_MINIMIZED && w->anim_state == 0) continue;

            if (mx >= w->x && mx <= w->x+w->width && my >= w->y && my <= w->y+w->height) {
                ws_bring_to_front(w);
                active_win = w;

                int lx = mx - w->x;
                int ly = my - w->y;

                // Title Bar (Drag)
                if (ly < 28 && click) {
                    // Traffic Lights Logic (Circular areas)
                    // Red: Center ~16,14. R=6 -> 10..22
                    if (lx >= 10 && lx <= 22) { w->anim_state = 2; return; }
                    // Yellow: Center ~36,14 -> 30..42
                    if (lx >= 30 && lx <= 42) { win_minimize(w); return; }
                    // Green: Center ~56,14 -> 50..62
                    if (lx >= 50 && lx <= 62) { win_maximize(w); return; }

                    // Start Drag
                    // FIX: Snap Restore Logic
                    if (w->state == WIN_STATE_SNAPPED || w->state == WIN_STATE_MAXIMIZED) {
                        // Restore size
                        w->width = w->saved_w; w->height = w->saved_h;
                        // Center on mouse X, keep mouse Y relative to top
                        w->x = mx - (w->width / 2);
                        w->y = my - ly;
                        w->state = WIN_STATE_NORMAL;
                        // Recalculate drag offset since window moved under mouse
                        lx = mx - w->x;
                        ly = my - w->y;
                    }
                    drag_win = w;
                    drag_off_x = lx;
                    drag_off_y = ly;
                    return;
                }

                // Check Resize Grip (Bottom Right)
                if (lx >= w->width - RESIZE_MARGIN && ly >= w->height - RESIZE_MARGIN && click) {
                    resize_win = w;
                    resize_orig_w = w->width;
                    resize_orig_h = w->height;
                    resize_mx = mx;
                    resize_my = my;
                    return;
                }

                // Content Click
                if (w->mouse_callback) {
                    typedef void (*mcb)(int,int,int);
                    ((mcb)w->mouse_callback)(lx, ly - 30, btn);
                }
                return; 
            }
        }
        
        // Clicked Desktop
        // If we get here, no window trapped the click.
        active_win = 0;

        // Double Click Detection
        if (click) {
            // Check double click OR click-again-on-select
            int hit_idx = -1;
            int x = 30; int y = 60; // GRID_START matches desktop.c
            for(int i=0; i<32; i++) {
                if (desk_entries[i].filename[0] == 0) continue;
                if (mx >= x && mx <= x+48 && my >= y && my <= y+60) {
                    hit_idx = i; break;
                }
                y += 100; if(y>600) { y=60; x+=100; }
            }

            if (hit_idx != -1) {
                // If clicking the ALREADY selected item -> Trigger open
                if (hit_idx == last_select_idx) {
                    char path[128]; 
                    strcpy(path, g_desktop_path);
                    strcat(path, "/");
                    strcat(path, desk_entries[hit_idx].filename);
                    desktop_execute_item(path, (desk_entries[hit_idx].attributes & 0x10));
                    last_select_idx = -1; // Reset
                    return;
                }
                last_select_idx = hit_idx;
            } else {
                last_select_idx = -1;
            }
        }

        desktop_on_mouse(mx, my, lb, rb);
    }
}

// Screenlock + Welcome Setup state
static int g_setup_mode = 0;   // 1 = showing welcome setup wizard
static int g_first_boot_lock = 0;  // 1 = need to lock screen after setup

void start_bubble_view() {
    sys_gfx_init();
    
    // Explicitly pass kernel API
    cm_init(&g_kernel_api);
    
    ws_init();
    dock_init();
    
    sys_print("[GUI] Framework Initialized.\n");
    
    // Mute VGA console logging in GUI mode to prevent annoying text overlay
    // when apps are opened. Debug output still goes to serial port.
    extern void vga_mute_log(int enable);
    vga_mute_log(1);
    
    // Initialize screenlock and load user from config
    screenlock_init();
    screenlock_load_user();
    
    // Initialize welcome setup
    welcome_setup_init();
    
    // Apply saved timezone offset and keyboard layout from config
    {
        extern void sys_set_tz_offset(int);
        extern void kbd_set_layout(int);
        SystemConfig* cfg = welcome_setup_get_config();
        if (cfg && cfg->is_configured) {
            sys_set_tz_offset(cfg->timezone.offset_minutes);
            kbd_set_layout(cfg->kbd_layout);
        }
    }
    
    desktop_init();
    
    // Explicitly reset menu state
    g_ctx_menu.active = 0;
    frames_drawn = 0;

    // Check if we need to run the welcome setup wizard
    if (welcome_setup_needs_setup()) {
        g_setup_mode = 1;
        welcome_setup_start();
        sys_print("[GUI] First boot detected - showing welcome setup.\n");
    } else {
        // User is configured - show lock screen at boot
        g_setup_mode = 0;
        screenlock_lock();
        g_first_boot_lock = 1;
        if (screenlock_get_user()->has_password) {
            sys_print("[GUI] Screen locked at boot - waiting for authentication.\n");
        } else {
            sys_print("[GUI] Screen locked at boot - no password, click/key to unlock.\n");
        }
    }

    int mx = 0, my = 0;
    last_fs_gen = sys_get_fs_generation();

    // Force an initial clear to blue
    uint32_t* buffer = gfx_get_active_buffer();
    if (!buffer) {
        sys_print("[GUI] CRITICAL: No graphics buffer!\n");
        while(1);
    }

    while(1) {
        int dummy = 0;

        int mask = sys_mouse_read(&mx, &my, &dummy);

        int lb = mask & 1;
        int rb = (mask & 2) >> 1;

        // --- FIX: Startup Grace Period ---
        if (frames_drawn < STARTUP_GRACE_FRAMES) {
            lb = 0; rb = 0; // Force release
        }

        // Polling
        char k = sys_get_key();
        int click = (lb && !prev_lb);

        // ============================================
        // WELCOME SETUP MODE - First boot configuration
        // ============================================
        // NOTE: Scroll wheel handling is deferred to after mode checks.
        // In welcome setup mode, the setup code reads mouse_scroll_delta directly,
        // so we must NOT consume it via sys_mouse_scroll() before that.
        if (g_setup_mode && welcome_setup_is_active()) {
            // Update
            welcome_setup_update(0.02f);
            
            // Handle input
            if (k) welcome_setup_handle_key(k);
            welcome_setup_handle_mouse(mx, my, click, 0);
            // Consume scroll delta after welcome setup has had a chance to read it
            // (welcome_setup reads mouse_scroll_delta directly in its render functions)
            sys_mouse_scroll();
            
            // Render
            buffer = gfx_get_active_buffer();
            welcome_setup_render(buffer, 1024, 768, mx, my);
            
            // Draw cursor on top
            draw_cursor(mx, my);
            
            gfx_swap_buffers();
            sys_vsync();
            
            // Check if setup is complete
            if (!welcome_setup_is_active()) {
                g_setup_mode = 0;
                // Lock screen after setup if password was set
                if (screenlock_get_user()->has_password) {
                    screenlock_lock();
                    g_first_boot_lock = 1;
                }
            }
            
            prev_lb = lb;
            prev_rb = rb;
            frames_drawn++;
            continue;
        }

        // ============================================
        // SCREENLOCK MODE - Password entry / Lock screen
        // ============================================
        // Filter all events through screenlock when locked
        {
            int key_arg = k;
            int mx_arg = mx, my_arg = my, click_arg = click;
            if (screenlock_filter_event(&key_arg, &mx_arg, &my_arg, &click_arg)) {
                // Screen is locked - consume the event
                k = 0;  // Don't pass key to other handlers
                
                // Update screenlock animation
                screenlock_update(0.02f);
                
                // Render lock screen ON TOP of everything
                buffer = gfx_get_active_buffer();
                desktop_draw(buffer);
                
                for(int i=0; i<MAX_WINDOWS; i++) {
                    window_t* w = ws_get_window_at_index(i);
                    if(!w || !w->is_visible) continue;
                    draw_window_animated(w, mx, my);
                }
                
                // Draw lock screen overlay
                screenlock_render(buffer, 1024, 768, mx, my);
                
                gfx_swap_buffers();
                sys_vsync();
                
                prev_lb = lb;
                prev_rb = rb;
                frames_drawn++;
                continue;
            }
            // If we get here, screen is unlocked - k was already consumed if needed
        }

        // ============================================
        // NORMAL GUI MODE - Desktop interaction
        // ============================================

        // Scroll wheel handling - only in normal GUI mode
        // (welcome setup and screenlock modes consume scroll themselves above)
        // Dispatch scroll to the topmost window under the cursor, not just active_win.
        // This ensures scrollable areas work even when the cursor is over a
        // non-active window (e.g. Finder behind a Terminal).
        {
            int scroll_delta = sys_mouse_scroll();
            if (scroll_delta != 0) {
                // Find the topmost visible window under the cursor
                int scroll_handled = 0;
                for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                    window_t* w = ws_get_window_at_index(i);
                    if (w && w->is_visible && w->is_active &&
                        w->state != WIN_STATE_MINIMIZED) {
                        if (mx >= w->x && mx < w->x + w->width &&
                            my >= w->y && my < w->y + w->height) {
                            if (w->scroll_callback) {
                                typedef void (*scb)(int);
                                ((scb)w->scroll_callback)(scroll_delta);
                            }
                            scroll_handled = 1;
                            break;  // Only one window gets the scroll
                        }
                    }
                }
                // Fallback: if no window under cursor, try active_win
                if (!scroll_handled && active_win && active_win->scroll_callback) {
                    typedef void (*scb)(int);
                    ((scb)active_win->scroll_callback)(scroll_delta);
                }
            }
        }

        // --- FLICKER FIX: Save old drag position BEFORE input updates it ---
        if (drag_win) {
            drag_prev_x = drag_win->x;
            drag_prev_y = drag_win->y;
        }

        // --- FLICKER FIX: Process input BEFORE rendering ---
        // Previously handle_input was called AFTER gfx_swap_buffers(), which meant
        // the window position was always one frame behind the visual output.
        handle_input(mx, my, lb, rb);

        // App Switcher Logic
        int ctrl = 0, shift = 0, alt = 0;
        sys_kbd_state(&ctrl, &shift, &alt);

        if (ctrl) {
            if (k == '\t') app_switcher_handle_key(15, 1, shift);
        } else {
            if (app_switcher_is_active()) app_switcher_release();
        }

        // Dispatch key to active window's input callback
        // Skip if rename mode is active (key goes to rename handler instead)
        if (k != 0 && !renaming_mode && active_win && active_win->input_callback) {
             typedef void (*icb)(int);
             ((icb)active_win->input_callback)((int)k);
        }

        // --- FLICKER FIX: Dirty-region optimisation for window dragging ---
        // When a window is being dragged, only repaint the bounding box of its
        // old and new positions instead of the full screen.  This cuts the
        // wallpaper memcpy from ~3 MB to a small fraction, keeping each frame
        // well within the vsync interval.
        // Detect cursor type based on mouse position
        cursor_type = 0; // default arrow
        if (!resize_win && !drag_win) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                window_t* w = ws_get_window_at_index(i);
                if (!w || !w->is_visible || w->state == WIN_STATE_MINIMIZED) continue;
                int lx = mx - w->x;
                int ly = my - w->y;
                if (lx >= 0 && lx < w->width && ly >= 0 && ly < w->height) {
                    if (lx >= w->width - RESIZE_MARGIN && ly >= w->height - RESIZE_MARGIN) {
                        cursor_type = 1; // resize cursor
                    }
                    break;
                }
            }
        }
        if (resize_win) cursor_type = 1; // keep resize cursor while dragging

        buffer = gfx_get_active_buffer();

        if (drag_was_active && (drag_win || drag_just_released) && !resize_win) {
            // Compute dirty rect = union of old position + new position + shadow pad
            int ox = drag_prev_x, oy = drag_prev_y;
            int nx, ny, ww, wh;
            if (drag_just_released && !drag_win) {
                // Drag was released this frame — use the last known position
                nx = drag_prev_x; ny = drag_prev_y;
                // We need the window's size but it might be snapped.
                // Use the saved prev size from the last drag frame.
                // Since drag_win is now NULL, we can't get size from it.
                // Fall through to full redraw for the release frame.
                drag_just_released = 0;
                drag_was_active = 0;
                // Full redraw is safest on release
                desktop_draw(buffer);
                for(int i=0; i<MAX_WINDOWS; i++) {
                    window_t* w = ws_get_window_at_index(i);
                    if(!w || !w->is_visible) continue;
                    draw_window_animated(w, mx, my);
                }
            } else {
                nx = drag_win->x;   ny = drag_win->y;
                ww = drag_win->width, wh = drag_win->height;

                int dx1 = (ox < nx ? ox : nx) - DRAG_SHADOW_PAD;
                int dy1 = (oy < ny ? oy : ny) - DRAG_SHADOW_PAD;
                int dx2 = ((ox + ww > nx + ww ? ox : nx) + ww) + DRAG_SHADOW_PAD;
                int dy2 = ((oy + wh > ny + wh ? oy : ny) + wh) + DRAG_SHADOW_PAD;

                // Restore wallpaper in dirty region from cache (fast per-row memcpy)
                desktop_fill_wallpaper_region(buffer, dx1, dy1, dx2 - dx1, dy2 - dy1);
                // Redraw desktop icons (they may have been uncovered)
                desktop_draw_icons(buffer);

                // Redraw all windows (those outside the dirty rect paint over
                // unchanged pixels — harmless and cheaper than overlap testing)
                for(int i=0; i<MAX_WINDOWS; i++) {
                    window_t* w = ws_get_window_at_index(i);
                    if(!w || !w->is_visible) continue;
                    draw_window_animated(w, mx, my);
                }
            }
        } else {
            // Full redraw (wallpaper cache makes this a fast memcpy)
            desktop_draw(buffer);

            for(int i=0; i<MAX_WINDOWS; i++) {
                window_t* w = ws_get_window_at_index(i);
                if(!w || !w->is_visible) continue;
                draw_window_animated(w, mx, my);
            }
        }

        // Update drag state for next frame's dirty-region decision
        drag_was_active = (drag_win != 0) ? 1 : 0;
        drag_just_released = 0;

        frame_counter++;

        // Auto Refresh - check filesystem generation OR periodic fallback (every 2s)
        // Skip refresh while renaming to avoid disrupting the inline editor
        uint32_t gen = sys_get_fs_generation();
        if (!renaming_mode && (gen != last_fs_gen || (frame_counter % 120 == 0))) {
            desktop_refresh();
            last_fs_gen = gen;
        }

        // Handle rename mode input (inline editor rendered by desktop.c)
        // Use 'k' from the main loop - do NOT call sys_get_key() again,
        // the key was already consumed at the top of the loop.
        if (renaming_mode && k) {
                if (k == 13 || k == '\n') {
                    // Commit rename
                    if (strlen(desktop_rename_buf) > 0 && desktop_rename_idx >= 0 && desktop_rename_idx < 32) {
                        // Check for duplicate name - append (1), (2), etc.
                        char final_name[64];
                        strncpy(final_name, desktop_rename_buf, 63);
                        final_name[63] = 0;
                        
                        if (strcmp(desk_entries[desktop_rename_idx].filename, final_name) != 0) {
                            // Name changed - check for duplicates
                            int dup_count = 0;
                            char test_name[64];
                            strncpy(test_name, final_name, 63);
                            test_name[63] = 0;
                            
                            while (1) {
                                int is_dup = 0;
                                for (int di = 0; di < desk_count; di++) {
                                    if (di != desktop_rename_idx && 
                                        strcmp(desk_entries[di].filename, test_name) == 0) {
                                        is_dup = 1;
                                        break;
                                    }
                                }
                                if (!is_dup) break;
                                dup_count++;
                                // Append (N) before extension or at end
                                char base[64];
                                strncpy(base, final_name, 63);
                                base[63] = 0;
                                // Check for extension
                                char* dot = strrchr(base, '.');
                                if (dot && dot != base) {
                                    char ext[16];
                                    strncpy(ext, dot, 15);
                                    ext[15] = 0;
                                    *dot = 0;
                                    char num_str[8];
                                    int_to_str(dup_count, num_str);
                                    strcpy(test_name, base);
                                    strcat(test_name, " (");
                                    strcat(test_name, num_str);
                                    strcat(test_name, ")");
                                    strcat(test_name, ext);
                                } else {
                                    char num_str[8];
                                    int_to_str(dup_count, num_str);
                                    strcpy(test_name, base);
                                    strcat(test_name, " (");
                                    strcat(test_name, num_str);
                                    strcat(test_name, ")");
                                }
                            }
                            strncpy(final_name, test_name, 63);
                            final_name[63] = 0;
                        }
                        
                        char old_path[128], new_path[128];
                        strcpy(old_path, g_desktop_path);
                        strcat(old_path, "/");
                        strcat(old_path, desk_entries[desktop_rename_idx].filename);
                        strcpy(new_path, g_desktop_path);
                        strcat(new_path, "/");
                        strcat(new_path, final_name);
                        sys_fs_rename(old_path, new_path);
                        desktop_refresh();
                    }
                    renaming_mode = 0;
                    desktop_rename_active = 0;
                    desktop_rename_idx = -1;
                } else if (k == 27) { // Escape - cancel
                    renaming_mode = 0;
                    desktop_rename_active = 0;
                    desktop_rename_idx = -1;
                } else if (k == 8 || k == '\b') {
                    if (desktop_rename_cursor > 0) {
                        desktop_rename_cursor--;
                        // Remove char at cursor position
                        int len = strlen(desktop_rename_buf);
                        for (int ci = desktop_rename_cursor; ci < len; ci++) {
                            desktop_rename_buf[ci] = desktop_rename_buf[ci + 1];
                        }
                    }
                } else if (k >= 32 && k <= 126 && desktop_rename_cursor < 63) {
                    // Insert char at cursor position
                    int len = strlen(desktop_rename_buf);
                    if (len < 63) {
                        for (int ci = len; ci > desktop_rename_cursor; ci--) {
                            desktop_rename_buf[ci] = desktop_rename_buf[ci - 1];
                        }
                        desktop_rename_buf[desktop_rename_cursor++] = (char)k;
                        desktop_rename_buf[len + 1] = 0;
                    }
                }
        }

        // Draw Snap Preview Overlay
        if (snap_preview_active) {
            gfx_fill_rounded_rect(snap_preview_rect.x, snap_preview_rect.y,
                                  snap_preview_rect.w, snap_preview_rect.h,
                                  SNAP_PREVIEW_COLOR, 15);
        }

        dock_render(buffer, 1024, 768, mx, my);
        process_global_bar(mx, my, (lb && !prev_lb));
        draw_cursor(mx, my);

        ctx_menu_draw();

        // Render Overlays
        if (app_switcher_is_active()) {
            app_switcher_render(1024, 768);
        }

        gfx_swap_buffers();
        sys_vsync();

        // NOTE: handle_input was already called BEFORE rendering (line above).
        // Do NOT call it again here — duplicate calls cause double-processing
        // of mouse events and window position updates after the buffer swap,
        // which results in visible flickering.

        prev_lb = lb;
        prev_rb = rb;
        frames_drawn++;
    }
}
