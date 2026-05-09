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
// Pre-computed gradient or loaded BMP image eliminates per-pixel arithmetic every frame.
// This is the #1 fix for window flickering on move.
static uint32_t* wallpaper_cache = 0;
int wallpaper_cache_w = 0;
static int wallpaper_cache_h = 0;

// Static fallback for wallpaper cache when kmalloc fails (heap too fragmented
// for a 3MB+ contiguous allocation).  Supports up to 1024x768@32bpp.
#define STATIC_WALLPAPER_SIZE (1024 * 768 * 4)
static uint32_t static_wallpaper[STATIC_WALLPAPER_SIZE / 4] __attribute__((aligned(4096)));
static int using_static_wallpaper = 0;

// --- BMP Wallpaper Loader ---
// Loads a 24-bit or 32-bit BMP file from the filesystem into the wallpaper cache.
// BMP format: 14-byte file header + 40-byte info header + pixel data (bottom-up).
// Returns 1 on success, 0 on failure.
static int wallpaper_load_bmp(const char* path, int screen_w, int screen_h) {
    // Read the BMP file header (54 bytes minimum)
    uint8_t hdr[54];
    extern int sys_fs_read(const char*, char*, int);
    int hdr_len = sys_fs_read(path, (char*)hdr, 54);
    if (hdr_len < 54) return 0;

    // Verify BMP signature
    if (hdr[0] != 'B' || hdr[1] != 'M') return 0;

    // Parse BMP info header
    uint32_t data_offset = *(uint32_t*)(hdr + 10);
    int32_t  bmp_w       = *(int32_t*)(hdr + 18);
    int32_t  bmp_h       = *(int32_t*)(hdr + 22);
    uint16_t bpp         = *(uint16_t*)(hdr + 28);
    uint32_t compression = *(uint32_t*)(hdr + 30);

    // Only support uncompressed 24-bit or 32-bit BMPs
    if (compression != 0) return 0;
    if (bpp != 24 && bpp != 32) return 0;
    if (bmp_w <= 0 || bmp_h <= 0) return 0;

    // BMP height can be negative (top-down storage)
    int top_down = 0;
    if (bmp_h < 0) { bmp_h = -bmp_h; top_down = 1; }

    // Allocate wallpaper cache if needed
    if (!wallpaper_cache) {
        wallpaper_cache = (uint32_t*)kmalloc(screen_w * screen_h * 4);
        if (!wallpaper_cache) return 0;
        wallpaper_cache_w = screen_w;
        wallpaper_cache_h = screen_h;
    }

    // Read the entire pixel data section
    int row_bytes = ((bmp_w * bpp + 31) / 32) * 4;  // BMP rows are padded to 4 bytes
    int pixel_data_size = row_bytes * bmp_h;

    // Read in chunks (sys_fs_read may have a size limit)
    uint8_t* pixel_buf = (uint8_t*)kmalloc(pixel_data_size);
    if (!pixel_buf) return 0;

    // We need to seek to data_offset — read from the start and skip
    // Since sys_fs_read always starts at offset 0, read the whole file
    // and copy the pixel portion
    int total_to_read = data_offset + pixel_data_size;
    uint8_t* file_buf = (uint8_t*)kmalloc(total_to_read);
    if (!file_buf) { kfree(pixel_buf); return 0; }

    int bytes_read = sys_fs_read(path, (char*)file_buf, total_to_read);
    if (bytes_read < (int)(data_offset + pixel_data_size)) {
        // Might not have read everything — try with what we have
        if (bytes_read <= (int)data_offset) {
            kfree(pixel_buf); kfree(file_buf); return 0;
        }
        // Partial read — copy what we can
        int available = bytes_read - data_offset;
        if (available > 0) memcpy(pixel_buf, file_buf + data_offset, available);
    } else {
        memcpy(pixel_buf, file_buf + data_offset, pixel_data_size);
    }
    kfree(file_buf);

    // Copy BMP pixel data into wallpaper_cache, scaling to screen size
    // using bilinear interpolation for smooth rendering (matches the
    // quality of the embedded wallpaper scaling above).
    for (int sy = 0; sy < screen_h; sy++) {
        int src_y_fp = (sy * (bmp_h - 1) * 65536) / (screen_h > 1 ? screen_h - 1 : 1);
        int by0 = src_y_fp >> 16;
        int fy  = src_y_fp & 0xFFFF;
        int by1 = by0 + 1;
        if (by1 >= bmp_h) by1 = bmp_h - 1;

        int row0 = top_down ? by0 : (bmp_h - 1 - by0);
        int row1 = top_down ? by1 : (bmp_h - 1 - by1);
        uint8_t* pix_row0 = &pixel_buf[row0 * row_bytes];
        uint8_t* pix_row1 = &pixel_buf[row1 * row_bytes];
        int px_bytes = bpp / 8;

        for (int sx = 0; sx < screen_w; sx++) {
            int src_x_fp = (sx * (bmp_w - 1) * 65536) / (screen_w > 1 ? screen_w - 1 : 1);
            int bx0 = src_x_fp >> 16;
            int fx  = src_x_fp & 0xFFFF;
            int bx1 = bx0 + 1;
            if (bx1 >= bmp_w) bx1 = bmp_w - 1;

            uint8_t* p00 = &pix_row0[bx0 * px_bytes];
            uint8_t* p10 = &pix_row0[bx1 * px_bytes];
            uint8_t* p01 = &pix_row1[bx0 * px_bytes];
            uint8_t* p11 = &pix_row1[bx1 * px_bytes];

            // Bilinear blend per channel
            #define BMP_BLERP(arr) do {                                          \
                int top = (int)(arr[0]) + (((int)(arr[1]) - (int)(arr[0])) * fx) / 65536;  \
                int bot = (int)(arr[2]) + (((int)(arr[3]) - (int)(arr[2])) * fx) / 65536;  \
                int val = top + ((bot - top) * fy) / 65536;                      \
                if (val < 0) val = 0; if (val > 255) val = 255;                 \
                ch = (uint8_t)val;                                                \
            } while(0)

            uint8_t ch;
            // Blue channel
            uint8_t b_arr[4] = {p00[0], p10[0], p01[0], p11[0]};
            BMP_BLERP(b_arr); uint8_t b_val = ch;
            // Green channel
            uint8_t g_arr[4] = {p00[1], p10[1], p01[1], p11[1]};
            BMP_BLERP(g_arr); uint8_t g_val = ch;
            // Red channel
            uint8_t r_arr[4] = {p00[2], p10[2], p01[2], p11[2]};
            BMP_BLERP(r_arr); uint8_t r_val = ch;
            #undef BMP_BLERP

            wallpaper_cache[sy * screen_w + sx] =
                0xFF000000 | ((uint32_t)r_val << 16) | ((uint32_t)g_val << 8) | b_val;
        }
    }

    kfree(pixel_buf);
    return 1;
}

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

// --- Desktop Icon Drag ---
// Per-icon grid positions (col, row). If -1, uses auto-layout.
static int icon_grid_col[32];
static int icon_grid_row[32];

// Drag state
static int icon_drag_idx = -1;       // Index of icon being dragged (-1 = none)
static int icon_drag_off_x = 0;      // Mouse offset from icon top-left
static int icon_drag_off_y = 0;
static int icon_drag_start_col = -1;  // Original position (for cancel)
static int icon_drag_start_row = -1;
static int icon_drag_active = 0;      // 1 = mouse has moved past threshold (visual drag)
static int icon_drag_start_mx = 0;    // Mouse position when drag was initiated
static int icon_drag_start_my = 0;
#define ICON_DRAG_THRESHOLD 6         // Pixels of mouse movement before drag starts

// Get the screen position for desktop icon at index i.
// Uses icon_grid_col/row if set, otherwise auto-layout.
static void desktop_icon_pos(int i, int* out_x, int* out_y) {
    if (icon_grid_col[i] >= 0 && icon_grid_row[i] >= 0) {
        *out_x = GRID_START_X + icon_grid_col[i] * ICON_SPACING_X;
        *out_y = GRID_START_Y + icon_grid_row[i] * ICON_SPACING_Y;
    } else {
        // Auto-layout: column = i % max_rows, row = i / max_rows
        // Icons go down first, then right (macOS-style)
        int max_rows = (700 - GRID_START_Y) / ICON_SPACING_Y;
        if (max_rows < 1) max_rows = 1;
        *out_x = GRID_START_X + (i / max_rows) * ICON_SPACING_X;
        *out_y = GRID_START_Y + (i % max_rows) * ICON_SPACING_Y;
    }
}

// Find which desktop icon is at screen position (mx, my). Returns -1 if none.
int desktop_icon_at(int mx, int my) {
    for (int i = 0; i < desk_count; i++) {
        int ix, iy;
        desktop_icon_pos(i, &ix, &iy);
        if (mx >= ix && mx <= ix + 48 && my >= iy && my <= iy + 60) {
            return i;
        }
    }
    return -1;
}

// Check if a grid cell is occupied by any icon except the one being dragged
static int desktop_grid_occupied(int col, int row, int skip_idx) {
    for (int i = 0; i < desk_count; i++) {
        if (i == skip_idx) continue;
        if (icon_grid_col[i] == col && icon_grid_row[i] == row) return 1;
    }
    return 0;
}

// Find the nearest free grid cell to a screen position
static void desktop_nearest_free_cell(int mx, int my, int skip_idx, int* out_col, int* out_row) {
    int col = (mx - GRID_START_X + ICON_SPACING_X / 2) / ICON_SPACING_X;
    int row = (my - GRID_START_Y + ICON_SPACING_Y / 2) / ICON_SPACING_Y;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col > 9) col = 9;
    if (row > 6) row = 6;

    // If the target cell is free, use it
    if (!desktop_grid_occupied(col, row, skip_idx)) {
        *out_col = col;
        *out_row = row;
        return;
    }

    // Search nearby cells in a spiral pattern
    for (int dist = 1; dist < 8; dist++) {
        for (int dy = -dist; dy <= dist; dy++) {
            for (int dx = -dist; dx <= dist; dx++) {
                if (abs(dx) != dist && abs(dy) != dist) continue;
                int c = col + dx, r = row + dy;
                if (c < 0 || r < 0 || c > 9 || r > 6) continue;
                if (!desktop_grid_occupied(c, r, skip_idx)) {
                    *out_col = c;
                    *out_row = r;
                    return;
                }
            }
        }
    }
    *out_col = col;
    *out_row = row;
}

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
    // Initialize icon positions to auto-layout (-1)
    for (int i = 0; i < 32; i++) { icon_grid_col[i] = -1; icon_grid_row[i] = -1; }
    
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
        strcpy(g_desktop_path, "/Users/user/Desktop");
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
    
    // Create default desktop icons (as .app bundles) ONLY on first boot
    // (before the system is configured).  On subsequent boots the desktop
    // should only show files the user has placed there — creating shortcuts
    // every time causes unwanted icons to reappear after reboot.
    {
        extern int welcome_setup_needs_setup(void);
        if (welcome_setup_needs_setup()) {
            const char* default_apps[] = {
                "Calculator.app",
                "MacTest.app",
                "About.app",
                NULL
            };
            char app_path[256];
            for (int i = 0; default_apps[i]; i++) {
                snprintf(app_path, sizeof(app_path), "%s/%s", g_desktop_path, default_apps[i]);
                if (!sys_fs_exists(app_path)) {
                    sys_fs_create(app_path, 1);  // Create as directory (.app bundle)
                }
            }
        }
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

    uint32_t needed = w * h * 4;

    // Free old heap-allocated cache if dimensions changed (but not static buffer)
    if (wallpaper_cache && !using_static_wallpaper) {
        kfree(wallpaper_cache);
        wallpaper_cache = 0;
    }

    // Try heap allocation first
    wallpaper_cache = (uint32_t*)kmalloc(needed);
    if (wallpaper_cache) {
        using_static_wallpaper = 0;
        wallpaper_cache_w = w;
        wallpaper_cache_h = h;
    } else if (needed <= STATIC_WALLPAPER_SIZE) {
        // Heap failed but static buffer is large enough
        wallpaper_cache = static_wallpaper;
        using_static_wallpaper = 1;
        wallpaper_cache_w = w;
        wallpaper_cache_h = h;
    } else {
        // Neither works — fallback to per-pixel rendering in desktop_draw
        wallpaper_cache = 0;
        wallpaper_cache_w = 0;
        wallpaper_cache_h = 0;
        return;
    }

    // Try loading embedded wallpaper image first (compiled into the kernel)
    // The image is stored at a reduced resolution and scaled up at runtime.
    {
        extern const int wallpaper_img_w;
        extern const int wallpaper_img_h;
        extern const uint32_t wallpaper_img_data[];
        int img_w = wallpaper_img_w;
        int img_h = wallpaper_img_h;

        if (img_w > 0 && img_h > 0 && wallpaper_img_data[0] != 0) {
            // Scale the embedded image to screen resolution using
            // bilinear interpolation for smooth quality (fixes
            // blocky/pixelated wallpaper when the source image is
            // only 320x240 but the screen is 1024x768).
            // Uses integer math only — no floating-point needed.
            for (int sy = 0; sy < h; sy++) {
                // Map screen Y to source Y in 16.16 fixed-point
                int src_y_fp = (sy * (img_h - 1) * 65536) / (h > 1 ? h - 1 : 1);
                int iy0 = src_y_fp >> 16;
                int fy  = src_y_fp & 0xFFFF;
                int iy1 = iy0 + 1;
                if (iy1 >= img_h) iy1 = img_h - 1;

                for (int sx = 0; sx < w; sx++) {
                    // Map screen X to source X in 16.16 fixed-point
                    int src_x_fp = (sx * (img_w - 1) * 65536) / (w > 1 ? w - 1 : 1);
                    int ix0 = src_x_fp >> 16;
                    int fx  = src_x_fp & 0xFFFF;
                    int ix1 = ix0 + 1;
                    if (ix1 >= img_w) ix1 = img_w - 1;

                    // Sample 4 neighbours
                    uint32_t c00 = wallpaper_img_data[iy0 * img_w + ix0];
                    uint32_t c10 = wallpaper_img_data[iy0 * img_w + ix1];
                    uint32_t c01 = wallpaper_img_data[iy1 * img_w + ix0];
                    uint32_t c11 = wallpaper_img_data[iy1 * img_w + ix1];

                    // Bilinear blend per channel (ARGB)
                    // fx and fy are 0..65535, we need (fx*fy)/65536 etc.
                    // Weight:  w00 = (65536-fx)*(65536-fy), w10 = fx*(65536-fy),
                    //          w01 = (65536-fx)*fy,          w11 = fx*fy
                    // All divided by 65536*65536 to normalise.
                    // To avoid 64-bit overflow we compute per-channel in two steps.
                    #define BLERP_CH(shft) do {                                     \
                        int v00 = (c00 >> shft) & 0xFF;                             \
                        int v10 = (c10 >> shft) & 0xFF;                             \
                        int v01 = (c01 >> shft) & 0xFF;                             \
                        int v11 = (c11 >> shft) & 0xFF;                             \
                        int top = v00 + ((v10 - v00) * fx) / 65536;                \
                        int bot = v01 + ((v11 - v01) * fx) / 65536;                \
                        int val = top + ((bot - top) * fy) / 65536;                \
                        if (val < 0) val = 0; if (val > 255) val = 255;           \
                        ch = (uint8_t)val;                                          \
                    } while(0)

                    uint8_t ra, ga, ba, aa;
                    uint8_t ch;  // temporary for BLERP_CH macro
                    BLERP_CH(16); ra = ch;
                    BLERP_CH(8);  ga = ch;
                    BLERP_CH(0);  ba = ch;
                    BLERP_CH(24); aa = ch;
                    #undef BLERP_CH

                    wallpaper_cache[sy * w + sx] =
                        ((uint32_t)aa << 24) | ((uint32_t)ra << 16) |
                        ((uint32_t)ga << 8)  | ba;
                }
            }
            return;  // Wallpaper image loaded successfully
        }
    }

    // Fallback: Try loading a BMP wallpaper from the filesystem
    // Check /System/Wallpaper.bmp, then /Library/Wallpaper.bmp
    int bmp_loaded = 0;
    extern int sys_fs_exists(const char*);
    if (sys_fs_exists("/System/Wallpaper.bmp")) {
        bmp_loaded = wallpaper_load_bmp("/System/Wallpaper.bmp", w, h);
    }
    if (!bmp_loaded && sys_fs_exists("/Library/Wallpaper.bmp")) {
        bmp_loaded = wallpaper_load_bmp("/Library/Wallpaper.bmp", w, h);
    }

    if (bmp_loaded) return;  // BMP loaded successfully — skip gradient

    // Final fallback: Fill cached gradient using theme desktop_bg color
    // Per-channel gradient with proper clamping to prevent underflow.
    // The old formula (col = base - y/4) caused channel bleeding in dark
    // mode: e.g. 0xFF1C1C1E - 31 = 0xFF1C1BFD (blue wraps, borrows from green).
    const theme_t* theme = theme_get_current();
    uint32_t base = theme->desktop_bg;
    uint8_t base_r = (base >> 16) & 0xFF;
    uint8_t base_g = (base >> 8) & 0xFF;
    uint8_t base_b = base & 0xFF;
    for(int y=0; y<h; y++) {
        int shift = y / 4;
        uint8_t nr = (base_r > shift) ? (base_r - shift) : 0;
        uint8_t ng = (base_g > shift) ? (base_g - shift) : 0;
        uint8_t nb = (base_b > shift) ? (base_b - shift) : 0;
        uint32_t col = 0xFF000000 | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
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
        // Per-channel gradient with proper clamping (same as wallpaper_cache_ensure)
        const theme_t* theme_fb = theme_get_current();
        uint32_t base_fb = theme_fb->desktop_bg;
        uint8_t fb_r = (base_fb >> 16) & 0xFF;
        uint8_t fb_g = (base_fb >> 8) & 0xFF;
        uint8_t fb_b = base_fb & 0xFF;
        for(int y=ry; y<ry+rh && y<h; y++) {
            int shift = y / 4;
            uint8_t nr = (fb_r > shift) ? (fb_r - shift) : 0;
            uint8_t ng = (fb_g > shift) ? (fb_g - shift) : 0;
            uint8_t nb = (fb_b > shift) ? (fb_b - shift) : 0;
            uint32_t col = 0xFF000000 | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
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
        // Get icon position — either from grid or auto-layout
        if (i == icon_drag_idx && icon_drag_active) {
            // Draw dragged icon at mouse position (get mouse from kernel API)
            int mmx, mmy, dummy;
            sys_mouse_read(&mmx, &mmy, &dummy);
            x = mmx - icon_drag_off_x;
            y = mmy - icon_drag_off_y;
            // Semi-transparent highlight behind dragged icon
            gfx_fill_rect(x - 4, y - 4, 56, 68, 0x40007AFF);
        } else {
            desktop_icon_pos(i, &x, &y);
        }

        // Always repaint the icon's background from the wallpaper cache first.
        desktop_fill_wallpaper_region(buffer, x - 10, y - 5, 68, 80);

        // Selection Highlight
        if(desk_selected[i] && !(desktop_rename_active && desktop_rename_idx == i)) {
            sys_gfx_rect(x-10, y-5, 68, 80, 0x40FFFFFF);
        }

        const char* icon = (desk_entries[i].attributes & 0x10) ? "folder" : "file";
        // Check for .app extension - show as app icon (not installer)
        int len = strlen(desk_entries[i].filename);
        if(len > 4 && strcmp(desk_entries[i].filename + len - 4, ".app") == 0) {
            icon = "terminal";  // Default app icon
            // App-specific icons — each app gets a visually distinct icon
            if (strncmp(desk_entries[i].filename, "Calculator", 10) == 0) icon = "calculator";
            else if (strncmp(desk_entries[i].filename, "About", 5) == 0) icon = "about";
            else if (strncmp(desk_entries[i].filename, "MacTest", 7) == 0) icon = "mactest";
            else if (strncmp(desk_entries[i].filename, "Settings", 8) == 0) icon = "settings";
            else if (strncmp(desk_entries[i].filename, "Terminal", 8) == 0) icon = "terminal";
            else if (strncmp(desk_entries[i].filename, "Browser", 7) == 0) icon = "browser";
        }
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
        // Per-channel gradient with proper clamping (same as wallpaper_cache_ensure)
        const theme_t* theme_dd = theme_get_current();
        uint32_t base_dd = theme_dd->desktop_bg;
        uint8_t dd_r = (base_dd >> 16) & 0xFF;
        uint8_t dd_g = (base_dd >> 8) & 0xFF;
        uint8_t dd_b = base_dd & 0xFF;
        for(int y=0; y<h; y++) {
            int shift = y / 4;
            uint8_t nr = (dd_r > shift) ? (dd_r - shift) : 0;
            uint8_t ng = (dd_g > shift) ? (dd_g - shift) : 0;
            uint8_t nb = (dd_b > shift) ? (dd_b - shift) : 0;
            uint32_t col = 0xFF000000 | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
            for(int x=0; x<w; x++) buffer[y*w+x] = col;
        }
    }

    // 2. Draw Icons
    desktop_draw_icons(buffer);

    // 3. Selection box is drawn AFTER windows in bubbleview.c render loop
    //    so it appears on top of the wallpaper and any overlapping windows.
}

// Draw the desktop selection box on top of everything.
// Called from the bubbleview.c render loop AFTER windows are drawn,
// so the rubber-band selection appears above the wallpaper and any
// overlapping windows (fixes "selection box hidden behind windows" bug).
void desktop_draw_selbox() {
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
        // Right-click: cancel any active selection/drag and show context menu
        selbox_cancel(&g_desk_selbox);
        icon_drag_idx = -1;

        int hit_idx = desktop_icon_at(mx, my);

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

        // --- Icon dragging ---
        if (icon_drag_idx >= 0) {
            // Already tracking an icon — check if mouse has moved past threshold
            int dx = mx - icon_drag_start_mx;
            int dy = my - icon_drag_start_my;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx >= ICON_DRAG_THRESHOLD || dy >= ICON_DRAG_THRESHOLD) {
                icon_drag_active = 1;  // Past threshold — visual drag is active
            }
            return;
        }

        // If the selbox is already being dragged, just update it.
        if (g_desk_selbox.state == SELBOX_DRAGGING) {
            selbox_update(&g_desk_selbox, mx, my);
            desktop_apply_selection(&g_desk_selbox);
            return;
        }

        // Check if clicking on an icon to start a drag
        int hit_idx = desktop_icon_at(mx, my);
        if (hit_idx >= 0) {
            // Clicked on an icon — select it and prepare for potential drag.
            // Drag only becomes active after mouse moves past ICON_DRAG_THRESHOLD
            // pixels, so a simple click still works for selection/double-click.
            memset(desk_selected, 0, sizeof(desk_selected));
            desk_selected[hit_idx] = 1;
            icon_drag_idx = hit_idx;
            icon_drag_active = 0;  // Not yet — wait for movement
            icon_drag_start_mx = mx;
            icon_drag_start_my = my;
            int ix, iy;
            desktop_icon_pos(hit_idx, &ix, &iy);
            icon_drag_off_x = mx - ix;
            icon_drag_off_y = my - iy;
            icon_drag_start_col = icon_grid_col[hit_idx];
            icon_drag_start_row = icon_grid_row[hit_idx];
            // Cancel any selbox that might have started
            selbox_cancel(&g_desk_selbox);
            return;
        }

        // Start a new selbox drag from this point.
        selbox_start(&g_desk_selbox, mx, my);
    } else {
        // Mouse button released
        // --- Finish icon drag ---
        if (icon_drag_idx >= 0) {
            if (icon_drag_active) {
                // Real drag — drop the icon at the nearest free grid cell
                int new_col, new_row;
                desktop_nearest_free_cell(mx, my, icon_drag_idx, &new_col, &new_row);
                icon_grid_col[icon_drag_idx] = new_col;
                icon_grid_row[icon_drag_idx] = new_row;
            }
            // If not drag_active, the user just clicked without moving —
            // keep the icon at its original position (already selected above)
            icon_drag_idx = -1;
            icon_drag_active = 0;
            return;
        }

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
                int hit_idx = desktop_icon_at(mx, my);
                if (hit_idx >= 0) {
                    memset(desk_selected, 0, sizeof(desk_selected));
                    desk_selected[hit_idx] = 1;
                    return;
                }
                // Clicked on empty space without dragging — deselect all
                memset(desk_selected, 0, sizeof(desk_selected));
            }
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

    for(int i=0; i<desk_count; i++) {
        int ix, iy;
        desktop_icon_pos(i, &ix, &iy);
        // Icon bounding box: (ix, iy) to (ix+48, iy+60)
        // Check intersection with selection rect
        if (ix < rx + rw && ix + 48 > rx && iy < ry + rh && iy + 60 > ry) {
            desk_selected[i] = 1;
        }
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
