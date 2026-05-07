// usr/apps/image_viewer.c - CamelOS Image Viewer App
// macOS Preview-like image viewer with zoom, pan, and directory navigation
// Supports PNG and JPEG images via CGImageLoadPNG / CGImageLoadJPEG

#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/video/cgcontext.h"
#include "../../core/window_server.h"
#include "../../core/png_decoder.h"
#include "../dock.h"
#include "../../fs/pfs32.h"
#include "../../hal/drivers/serial.h"

// ========================================================================
// Layout constants
// ========================================================================
#define TOOLBAR_H       36
#define PAD              4
#define CHAR_W           8
#define CHAR_H          16
#define CHECKER_SIZE     8       // Size of transparency checker square

// ========================================================================
// Zoom levels
// ========================================================================
#define ZOOM_FIT        0        // Fit to window
#define ZOOM_ACTUAL     1        // 1:1 (100%)
#define ZOOM_200        2        // 2:1 (200%)
#define ZOOM_50         3        // 1:2 (50%)
#define ZOOM_COUNT      4

static const char* zoom_labels[ZOOM_COUNT] = { "Fit", "100%", "200%", "50%" };

// ========================================================================
// Per-instance state (multi-instance support)
// ========================================================================
#define MAX_IV_INSTANCES 4
#define IV_MAX_DIR_ENTRIES 64

typedef struct {
    int active;
    int window_id;

    // Current image
    CGImageRef image;           // Loaded CGImage (NULL if none)
    char current_path[256];     // Full path of current image
    int img_w, img_h;           // Original image dimensions

    // Zoom / pan state
    int zoom_mode;              // ZOOM_FIT, ZOOM_ACTUAL, ZOOM_200, ZOOM_50
    float zoom_factor;          // Computed zoom factor
    int pan_x, pan_y;           // Pan offset in viewport pixels

    // Drag-to-pan state
    int is_panning;
    int pan_start_x, pan_start_y;
    int pan_start_off_x, pan_start_off_y;

    // Directory navigation
    char dir_path[256];                     // Directory of current image
    char dir_entries[IV_MAX_DIR_ENTRIES][64]; // Filenames in directory
    int  dir_count;                         // Number of entries
    int  dir_index;                         // Index of current image in dir_entries
    int  dir_has_prev, dir_has_next;

    // Window dimensions
    int win_w, win_h;
} IVInstance;

static IVInstance iv_instances[MAX_IV_INSTANCES];
static IVInstance* iv_cur = 0;

// Forward declarations
static void iv_update_title(IVInstance* inst, window_t* win);
static void iv_compute_zoom(IVInstance* inst);
static void iv_load_directory(IVInstance* inst);
static void iv_find_dir_index(IVInstance* inst);
static void iv_update_nav(IVInstance* inst);

// ========================================================================
// Instance management (follows files.c pattern)
// ========================================================================

static IVInstance* iv_find_instance(int window_id) {
    for (int i = 0; i < MAX_IV_INSTANCES; i++) {
        if (iv_instances[i].active && iv_instances[i].window_id == window_id)
            return &iv_instances[i];
    }
    return 0;
}

static IVInstance* iv_alloc_instance(int window_id) {
    for (int i = 0; i < MAX_IV_INSTANCES; i++) {
        if (!iv_instances[i].active) {
            memset(&iv_instances[i], 0, sizeof(IVInstance));
            iv_instances[i].active = 1;
            iv_instances[i].window_id = window_id;
            iv_instances[i].zoom_mode = ZOOM_FIT;
            iv_instances[i].win_w = 600;
            iv_instances[i].win_h = 450;
            return &iv_instances[i];
        }
    }
    return 0;
}

static void iv_set_current_for(window_t* win) {
    if (win && win->user_data) {
        iv_cur = (IVInstance*)win->user_data;
        return;
    }
    extern window_t* active_win;
    if (active_win) {
        IVInstance* inst = iv_find_instance(active_win->id);
        if (inst) { iv_cur = inst; return; }
    }
    for (int i = 0; i < MAX_IV_INSTANCES; i++) {
        if (iv_instances[i].active) { iv_cur = &iv_instances[i]; return; }
    }
    iv_cur = 0;
}

// ========================================================================
// Zoom computation
// ========================================================================

static void iv_compute_zoom(IVInstance* inst) {
    if (!inst || !inst->image) return;

    int view_w = inst->win_w;
    int view_h = inst->win_h - TOOLBAR_H;

    switch (inst->zoom_mode) {
    case ZOOM_FIT: {
        float scale_x = (float)view_w / (float)inst->img_w;
        float scale_y = (float)view_h / (float)inst->img_h;
        inst->zoom_factor = scale_x < scale_y ? scale_x : scale_y;
        if (inst->zoom_factor > 4.0f) inst->zoom_factor = 4.0f;
        break;
    }
    case ZOOM_ACTUAL:
        inst->zoom_factor = 1.0f;
        break;
    case ZOOM_200:
        inst->zoom_factor = 2.0f;
        break;
    case ZOOM_50:
        inst->zoom_factor = 0.5f;
        break;
    default:
        inst->zoom_factor = 1.0f;
        break;
    }

    // In fit mode, center the image
    if (inst->zoom_mode == ZOOM_FIT) {
        int disp_w = (int)(inst->img_w * inst->zoom_factor);
        int disp_h = (int)(inst->img_h * inst->zoom_factor);
        inst->pan_x = (view_w - disp_w) / 2;
        inst->pan_y = (view_h - disp_h) / 2;
        if (inst->pan_x < 0) inst->pan_x = 0;
        if (inst->pan_y < 0) inst->pan_y = 0;
    }
}

// ========================================================================
// Directory navigation
// ========================================================================

static int iv_is_image_file(const char* name) {
    int len = strlen(name);
    if (len < 5) return 0;
    // PNG
    if (name[len-4] == '.' &&
        (name[len-3] == 'p' || name[len-3] == 'P') &&
        (name[len-2] == 'n' || name[len-2] == 'N') &&
        (name[len-1] == 'g' || name[len-1] == 'G'))
        return 1;
    // JPEG / JPG
    if (len >= 6 &&
        name[len-5] == '.' &&
        (name[len-4] == 'j' || name[len-4] == 'J') &&
        (name[len-3] == 'p' || name[len-3] == 'P') &&
        (name[len-2] == 'e' || name[len-2] == 'E') &&
        (name[len-1] == 'g' || name[len-1] == 'G'))
        return 1;
    if (name[len-4] == '.' &&
        (name[len-3] == 'j' || name[len-3] == 'J') &&
        (name[len-2] == 'p' || name[len-2] == 'P') &&
        (name[len-1] == 'g' || name[len-1] == 'G'))
        return 1;
    return 0;
}

static void iv_load_directory(IVInstance* inst) {
    if (!inst) return;
    inst->dir_count = 0;
    inst->dir_index = -1;

    if (inst->current_path[0] == 0) return;

    // Extract directory from current_path
    strcpy(inst->dir_path, inst->current_path);
    char* last_slash = strrchr(inst->dir_path, '/');
    if (last_slash && last_slash != inst->dir_path) {
        *last_slash = 0;
    } else {
        strcpy(inst->dir_path, "/");
    }

    // List directory entries
    pfs32_direntry_t entries[IV_MAX_DIR_ENTRIES];
    memset(entries, 0, sizeof(entries));
    int raw = sys_fs_list_dir(inst->dir_path, entries, IV_MAX_DIR_ENTRIES);

    for (int i = 0; i < raw && inst->dir_count < IV_MAX_DIR_ENTRIES; i++) {
        if (entries[i].filename[0] == 0 || entries[i].filename[0] == '.') continue;
        if (iv_is_image_file(entries[i].filename)) {
            strncpy(inst->dir_entries[inst->dir_count], entries[i].filename, 63);
            inst->dir_entries[inst->dir_count][63] = 0;
            inst->dir_count++;
        }
    }

    iv_find_dir_index(inst);
}

static void iv_find_dir_index(IVInstance* inst) {
    if (!inst) return;
    inst->dir_index = -1;

    const char* fname = strrchr(inst->current_path, '/');
    if (fname) fname++; else fname = inst->current_path;

    for (int i = 0; i < inst->dir_count; i++) {
        if (strcmp(inst->dir_entries[i], fname) == 0) {
            inst->dir_index = i;
            break;
        }
    }

    iv_update_nav(inst);
}

static void iv_update_nav(IVInstance* inst) {
    if (!inst) return;
    inst->dir_has_prev = (inst->dir_index > 0) ? 1 : 0;
    inst->dir_has_next = (inst->dir_index >= 0 && inst->dir_index < inst->dir_count - 1) ? 1 : 0;
}

// ========================================================================
// Image loading
// ========================================================================

static void iv_open_image(IVInstance* inst, const char* path) {
    if (!inst) return;

    // Free previous image
    if (inst->image) {
        CGImageDestroy(inst->image);
        inst->image = (CGImageRef)0;
    }

    // Determine file type and load
    int len = strlen(path);
    int is_png = 0, is_jpeg = 0;

    if (len >= 4) {
        const char* ext = path + len - 4;
        if ((ext[0] == '.' && (ext[1] == 'p' || ext[1] == 'P') &&
             (ext[2] == 'n' || ext[2] == 'N') && (ext[3] == 'g' || ext[3] == 'G'))) {
            is_png = 1;
        }
        if ((ext[0] == '.' && (ext[1] == 'j' || ext[1] == 'J') &&
             (ext[2] == 'p' || ext[2] == 'P') && (ext[3] == 'g' || ext[3] == 'G'))) {
            is_jpeg = 1;
        }
    }
    if (len >= 5) {
        const char* ext = path + len - 5;
        if ((ext[0] == '.' && (ext[1] == 'j' || ext[1] == 'J') &&
             (ext[2] == 'p' || ext[2] == 'P') &&
             (ext[3] == 'e' || ext[3] == 'E') &&
             (ext[4] == 'g' || ext[4] == 'G'))) {
            is_jpeg = 1;
        }
    }

    if (is_png) {
        inst->image = CGImageLoadPNG(path);
    } else if (is_jpeg) {
        inst->image = CGImageLoadJPEG(path);
    }

    if (inst->image) {
        inst->img_w = inst->image->width;
        inst->img_h = inst->image->height;
        strncpy(inst->current_path, path, sizeof(inst->current_path) - 1);
        inst->current_path[sizeof(inst->current_path) - 1] = 0;

        // Reset pan/zoom
        inst->zoom_mode = ZOOM_FIT;
        inst->pan_x = 0;
        inst->pan_y = 0;
        iv_compute_zoom(inst);
        iv_load_directory(inst);
    } else {
        s_printf("[ImageViewer] Failed to load: %s\n", path);
    }
}

// ========================================================================
// Navigate to prev/next image in directory
// ========================================================================

static void iv_navigate_prev(IVInstance* inst) {
    if (!inst || !inst->dir_has_prev) return;
    int new_idx = inst->dir_index - 1;

    char full_path[256];
    strcpy(full_path, inst->dir_path);
    if (strcmp(inst->dir_path, "/") != 0) strcat(full_path, "/");
    strcat(full_path, inst->dir_entries[new_idx]);

    iv_open_image(inst, full_path);
}

static void iv_navigate_next(IVInstance* inst) {
    if (!inst || !inst->dir_has_next) return;
    int new_idx = inst->dir_index + 1;

    char full_path[256];
    strcpy(full_path, inst->dir_path);
    if (strcmp(inst->dir_path, "/") != 0) strcat(full_path, "/");
    strcat(full_path, inst->dir_entries[new_idx]);

    iv_open_image(inst, full_path);
}

// ========================================================================
// Title bar update
// ========================================================================

static void iv_update_title(IVInstance* inst, window_t* win) {
    if (!inst || !win) return;

    char title[80];
    const char* fname = strrchr(inst->current_path, '/');
    if (fname) fname++; else fname = inst->current_path[0] ? inst->current_path : "No Image";

    strcpy(title, fname);

    if (inst->image) {
        // Append dimensions and zoom
        char info[40];
        // Compute zoom percentage
        int pct = (int)(inst->zoom_factor * 100 + 0.5f);
        // Simple int to string for percentage
        char pct_str[8];
        int pi = 0;
        if (pct >= 1000) { pct_str[pi++] = '0' + (pct / 1000) % 10; }
        if (pct >= 100)  { pct_str[pi++] = '0' + (pct / 100) % 10; }
        if (pct >= 10)   { pct_str[pi++] = '0' + (pct / 10) % 10; }
        pct_str[pi++] = '0' + pct % 10;
        pct_str[pi] = 0;

        // Dimensions
        char w_str[12], h_str[12];
        int_to_str(inst->img_w, w_str);
        int_to_str(inst->img_h, h_str);

        // Build info string: " 800x600 100%"
        strcpy(info, " ");
        strcat(info, w_str);
        strcat(info, "x");
        strcat(info, h_str);
        strcat(info, " ");
        strcat(info, pct_str);
        strcat(info, "%");

        if (strlen(title) + strlen(info) < 78) {
            strcat(title, info);
        }
    }

    ws_set_title(win, title);
}

// ========================================================================
// Drawing: transparency checkerboard
// ========================================================================

static void iv_draw_checker(int x, int y, int w, int h) {
    for (int cy = 0; cy < h; cy += CHECKER_SIZE) {
        for (int cx = 0; cx < w; cx += CHECKER_SIZE) {
            int row = (cy / CHECKER_SIZE) & 1;
            int col = (cx / CHECKER_SIZE) & 1;
            uint32_t color = (row ^ col) ? 0xFFCCCCCC : 0xFFFFFFFF;
            int draw_w = CHECKER_SIZE;
            int draw_h = CHECKER_SIZE;
            if (cx + draw_w > w) draw_w = w - cx;
            if (cy + draw_h > h) draw_h = h - cy;
            gfx_fill_rect(x + cx, y + cy, draw_w, draw_h, color);
        }
    }
}

// ========================================================================
// Main paint callback
// ========================================================================

static void image_viewer_on_paint(window_t* win, int x, int y, int w, int h) {
    iv_set_current_for(win);
    IVInstance* inst = iv_cur;
    if (!inst) {
        gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
        return;
    }

    // Background
    gfx_fill_rect(x, y, w, h, 0xFFF0F0F5);

    // ---- Toolbar ----
    gfx_fill_rect(x, y, w, TOOLBAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + TOOLBAR_H - 1, w, 1, 0xFFC6C6C8);

    int bx = x + 6;
    int btn_y = y + 6;
    int btn_h = 24;

    // Prev button
    {
        int bw = 28;
        uint32_t bg = inst->dir_has_prev ? 0xFFE8E8ED : 0xFFD8D8DD;
        gfx_fill_rounded_rect(bx, btn_y, bw, btn_h, bg, 4);
        gfx_draw_string(bx + 9, btn_y + 5, "<", inst->dir_has_prev ? 0xFF555555 : 0xFF999999);
        bx += bw + 4;
    }

    // Next button
    {
        int bw = 28;
        uint32_t bg = inst->dir_has_next ? 0xFFE8E8ED : 0xFFD8D8DD;
        gfx_fill_rounded_rect(bx, btn_y, bw, btn_h, bg, 4);
        gfx_draw_string(bx + 9, btn_y + 5, ">", inst->dir_has_next ? 0xFF555555 : 0xFF999999);
        bx += bw + 8;
    }

    // Zoom out button
    {
        int bw = 28;
        gfx_fill_rounded_rect(bx, btn_y, bw, btn_h, 0xFFE8E8ED, 4);
        gfx_draw_string(bx + 8, btn_y + 5, "-", 0xFF555555);
        bx += bw + 4;
    }

    // Zoom in button
    {
        int bw = 28;
        gfx_fill_rounded_rect(bx, btn_y, bw, btn_h, 0xFFE8E8ED, 4);
        gfx_draw_string(bx + 8, btn_y + 5, "+", 0xFF555555);
        bx += bw + 4;
    }

    // Fit button
    {
        int bw = 28;
        uint32_t bg = (inst->zoom_mode == ZOOM_FIT) ? 0xFF007AFF : 0xFFE8E8ED;
        uint32_t fg = (inst->zoom_mode == ZOOM_FIT) ? 0xFFFFFFFF : 0xFF555555;
        gfx_fill_rounded_rect(bx, btn_y, bw, btn_h, bg, 4);
        gfx_draw_string(bx + 3, btn_y + 5, "Fit", fg);
        bx += bw + 4;
    }

    // Actual size (1:1) button
    {
        int bw = 34;
        uint32_t bg = (inst->zoom_mode == ZOOM_ACTUAL) ? 0xFF007AFF : 0xFFE8E8ED;
        uint32_t fg = (inst->zoom_mode == ZOOM_ACTUAL) ? 0xFFFFFFFF : 0xFF555555;
        gfx_fill_rounded_rect(bx, btn_y, bw, btn_h, bg, 4);
        gfx_draw_string(bx + 2, btn_y + 5, "1:1", fg);
        bx += bw + 8;
    }

    // Zoom level display
    if (inst->image) {
        int pct = (int)(inst->zoom_factor * 100 + 0.5f);
        char pct_str[16];
        int_to_str(pct, pct_str);
        strcat(pct_str, "%");
        gfx_draw_string(bx, btn_y + 5, pct_str, 0xFF555555);
    }

    // Filename on right side of toolbar
    if (inst->current_path[0]) {
        const char* fname = strrchr(inst->current_path, '/');
        if (fname) fname++; else fname = inst->current_path;
        int fname_w = strlen(fname) * CHAR_W;
        int right_x = x + w - fname_w - 8;
        if (right_x > bx + 60) {
            gfx_draw_string(right_x, btn_y + 5, fname, 0xFF555555);
        }
    }

    // ---- Image area ----
    int img_area_y = y + TOOLBAR_H;
    int img_area_h = h - TOOLBAR_H;

    if (!inst->image) {
        // No image loaded — show placeholder
        gfx_fill_rect(x, img_area_y, w, img_area_h, 0xFFFFFFFF);
        const char* msg = inst->current_path[0] ? "Failed to load image" : "No image open";
        gfx_draw_string(x + w / 2 - strlen(msg) * 4, img_area_y + img_area_h / 2 - 8,
                        msg, 0xFF999999);

        // Show hint
        const char* hint = "Drop an image file or use Open";
        gfx_draw_string(x + w / 2 - strlen(hint) * 4, img_area_y + img_area_h / 2 + 12,
                        hint, 0xFFBBBBBB);
        return;
    }

    // Draw checkerboard background for the image area
    int disp_w = (int)(inst->img_w * inst->zoom_factor);
    int disp_h = (int)(inst->img_h * inst->zoom_factor);

    // Image position in window coordinates
    int img_x = x + inst->pan_x;
    int img_y = img_area_y + inst->pan_y;

    // Clip to image area
    gfx_set_clip(x, img_area_y, w, img_area_h);

    // Draw checkerboard only under the image region
    if (disp_w > 0 && disp_h > 0) {
        // Calculate the visible portion of the image
        int vis_x = img_x < x ? x : img_x;
        int vis_y = img_y < img_area_y ? img_area_y : img_y;
        int vis_r = (img_x + disp_w) > (x + w) ? (x + w) : (img_x + disp_w);
        int vis_b = (img_y + disp_h) > (img_area_y + img_area_h) ? (img_area_y + img_area_h) : (img_y + disp_h);
        int vis_w = vis_r - vis_x;
        int vis_h = vis_b - vis_y;

        if (vis_w > 0 && vis_h > 0) {
            iv_draw_checker(vis_x, vis_y, vis_w, vis_h);
        }

        // Draw the scaled image using CGContextDrawImageScaled
        // We draw directly to the screen using gfx_draw_image_scaled
        if (inst->image && inst->image->pixel_data) {
            sys_gfx_draw_image_scaled(img_x, img_y, disp_w, disp_h,
                                      inst->image->pixel_data,
                                      inst->img_w, inst->img_h);
        }
    }

    gfx_reset_clip();

    // Update title with zoom/dimension info
    iv_update_title(inst, win);
}

// ========================================================================
// Mouse callback
// ========================================================================

static void image_viewer_on_mouse(window_t* win, int x, int y, int btn) {
    iv_set_current_for(win);
    IVInstance* inst = iv_cur;
    if (!inst) return;

    // ---- End panning ----
    if (btn == 0 && inst->is_panning) {
        inst->is_panning = 0;
        return;
    }

    // ---- Toolbar area ----
    if (y < TOOLBAR_H && btn == 1) {
        int bx = 6;  // Relative to window origin
        int btn_h = 24;

        // Prev
        if (x >= bx && x < bx + 28) {
            iv_navigate_prev(inst);
            iv_update_title(inst, win);
            return;
        }
        bx += 32;

        // Next
        if (x >= bx && x < bx + 28) {
            iv_navigate_next(inst);
            iv_update_title(inst, win);
            return;
        }
        bx += 36;

        // Zoom out
        if (x >= bx && x < bx + 28) {
            // Cycle to smaller zoom
            if (inst->zoom_mode == ZOOM_200) inst->zoom_mode = ZOOM_ACTUAL;
            else if (inst->zoom_mode == ZOOM_ACTUAL) inst->zoom_mode = ZOOM_50;
            else if (inst->zoom_mode == ZOOM_FIT) inst->zoom_mode = ZOOM_50;
            iv_compute_zoom(inst);
            iv_update_title(inst, win);
            return;
        }
        bx += 32;

        // Zoom in
        if (x >= bx && x < bx + 28) {
            if (inst->zoom_mode == ZOOM_50) inst->zoom_mode = ZOOM_ACTUAL;
            else if (inst->zoom_mode == ZOOM_ACTUAL) inst->zoom_mode = ZOOM_200;
            else if (inst->zoom_mode == ZOOM_FIT) inst->zoom_mode = ZOOM_ACTUAL;
            iv_compute_zoom(inst);
            iv_update_title(inst, win);
            return;
        }
        bx += 32;

        // Fit
        if (x >= bx && x < bx + 28) {
            inst->zoom_mode = ZOOM_FIT;
            iv_compute_zoom(inst);
            iv_update_title(inst, win);
            return;
        }
        bx += 32;

        // 1:1
        if (x >= bx && x < bx + 34) {
            inst->zoom_mode = ZOOM_ACTUAL;
            iv_compute_zoom(inst);
            iv_update_title(inst, win);
            return;
        }
        return;
    }

    // ---- Image area: start drag-to-pan ----
    if (y >= TOOLBAR_H && btn == 1 && inst->image) {
        int disp_w = (int)(inst->img_w * inst->zoom_factor);
        int disp_h = (int)(inst->img_h * inst->zoom_factor);
        int view_w = inst->win_w;
        int view_h = inst->win_h - TOOLBAR_H;

        // Only allow panning if image is larger than viewport
        if (disp_w > view_w || disp_h > view_h) {
            inst->is_panning = 1;
            inst->pan_start_x = x;
            inst->pan_start_y = y;
            inst->pan_start_off_x = inst->pan_x;
            inst->pan_start_off_y = inst->pan_y;
        }
        return;
    }

    // ---- Drag-and-drop from Files app (double-click) ----
    if (y >= TOOLBAR_H && btn == 2) {
        // Right-click context: could add open-with support
        return;
    }
}

// ========================================================================
// Input callback
// ========================================================================

static void image_viewer_on_input(window_t* win, int key) {
    iv_set_current_for(win);
    IVInstance* inst = iv_cur;
    if (!inst) return;

    if (key == 0) return;

    // Arrow left: previous image
    if (key == 130) {  // KEY_LEFT
        iv_navigate_prev(inst);
        iv_update_title(inst, win);
        return;
    }

    // Arrow right: next image
    if (key == 131) {  // KEY_RIGHT
        iv_navigate_next(inst);
        iv_update_title(inst, win);
        return;
    }

    // '+' key: zoom in
    if (key == '+' || key == '=') {
        if (inst->zoom_mode == ZOOM_50) inst->zoom_mode = ZOOM_ACTUAL;
        else if (inst->zoom_mode == ZOOM_ACTUAL) inst->zoom_mode = ZOOM_200;
        else if (inst->zoom_mode == ZOOM_FIT) inst->zoom_mode = ZOOM_ACTUAL;
        iv_compute_zoom(inst);
        iv_update_title(inst, win);
        return;
    }

    // '-' key: zoom out
    if (key == '-') {
        if (inst->zoom_mode == ZOOM_200) inst->zoom_mode = ZOOM_ACTUAL;
        else if (inst->zoom_mode == ZOOM_ACTUAL) inst->zoom_mode = ZOOM_50;
        else if (inst->zoom_mode == ZOOM_FIT) inst->zoom_mode = ZOOM_50;
        iv_compute_zoom(inst);
        iv_update_title(inst, win);
        return;
    }

    // '0' key: fit to window
    if (key == '0') {
        inst->zoom_mode = ZOOM_FIT;
        iv_compute_zoom(inst);
        iv_update_title(inst, win);
        return;
    }

    // '1' key: actual size (1:1)
    if (key == '1') {
        inst->zoom_mode = ZOOM_ACTUAL;
        iv_compute_zoom(inst);
        iv_update_title(inst, win);
        return;
    }
}

// ========================================================================
// Scroll callback — zoom with scroll wheel
// ========================================================================

static void image_viewer_on_scroll(window_t* win, int delta) {
    iv_set_current_for(win);
    IVInstance* inst = iv_cur;
    if (!inst) return;

    // Ctrl+scroll = zoom
    int ctrl = 0, shift = 0, alt = 0;
    sys_kbd_state(&ctrl, &shift, &alt);

    if (ctrl && inst->image) {
        // Zoom in/out with Ctrl+scroll
        if (delta > 0) {
            // Scroll up = zoom in
            if (inst->zoom_mode == ZOOM_50) inst->zoom_mode = ZOOM_ACTUAL;
            else if (inst->zoom_mode == ZOOM_ACTUAL) inst->zoom_mode = ZOOM_200;
            else if (inst->zoom_mode == ZOOM_FIT) inst->zoom_mode = ZOOM_ACTUAL;
        } else {
            // Scroll down = zoom out
            if (inst->zoom_mode == ZOOM_200) inst->zoom_mode = ZOOM_ACTUAL;
            else if (inst->zoom_mode == ZOOM_ACTUAL) inst->zoom_mode = ZOOM_50;
            else if (inst->zoom_mode == ZOOM_FIT) inst->zoom_mode = ZOOM_50;
        }
        iv_compute_zoom(inst);
        iv_update_title(inst, win);
        return;
    }

    // Regular scroll = pan vertically
    if (inst->image) {
        inst->pan_y -= delta * 30;
        // Clamp pan
        int view_h = inst->win_h - TOOLBAR_H;
        int disp_h = (int)(inst->img_h * inst->zoom_factor);
        int max_pan_y = disp_h > view_h ? 0 : (view_h - disp_h) / 2;
        if (inst->pan_y < -(disp_h - view_h)) inst->pan_y = -(disp_h - view_h);
        if (inst->pan_y > max_pan_y) inst->pan_y = max_pan_y;
    }
}

// ========================================================================
// Hscroll callback — pan horizontally
// ========================================================================

static void image_viewer_on_hscroll(window_t* win, int delta) {
    iv_set_current_for(win);
    IVInstance* inst = iv_cur;
    if (!inst || !inst->image) return;

    inst->pan_x -= delta * 30;
    int view_w = inst->win_w;
    int disp_w = (int)(inst->img_w * inst->zoom_factor);
    int max_pan_x = disp_w > view_w ? 0 : (view_w - disp_w) / 2;
    if (inst->pan_x < -(disp_w - view_w)) inst->pan_x = -(disp_w - view_w);
    if (inst->pan_x > max_pan_x) inst->pan_x = max_pan_x;
}

// ========================================================================
// Resize callback
// ========================================================================

static void image_viewer_on_resize(window_t* win, int new_w, int new_h) {
    iv_set_current_for(win);
    IVInstance* inst = iv_cur;
    if (!inst) return;

    inst->win_w = new_w;
    inst->win_h = new_h;

    // Recompute zoom in fit mode
    if (inst->zoom_mode == ZOOM_FIT) {
        iv_compute_zoom(inst);
    }
}

// ========================================================================
// Close callback
// ========================================================================

static void image_viewer_on_close(window_t* win) {
    IVInstance* inst = (IVInstance*)win->user_data;
    if (inst) {
        if (inst->image) {
            CGImageDestroy(inst->image);
            inst->image = (CGImageRef)0;
        }
        inst->active = 0;
        win->user_data = 0;
    }
}

// ========================================================================
// Drag-and-drop handler — called when a file is dropped on the window
// ========================================================================

void image_viewer_open(const char* path) {
    if (!iv_cur) return;
    iv_open_image(iv_cur, path);
}

// ========================================================================
// Public API
// ========================================================================

void image_viewer_init(void) {
    // Default initialization — creates an empty viewer window
    window_t* w = ws_create_window("Image Viewer", 600, 450,
                                    image_viewer_on_paint,
                                    image_viewer_on_input,
                                    image_viewer_on_mouse);

    IVInstance* inst = iv_alloc_instance(w->id);
    if (inst) {
        iv_cur = inst;
        w->user_data = (void*)inst;
    }

    w->scroll_callback = (void*)image_viewer_on_scroll;
    w->hscroll_callback = (void*)image_viewer_on_hscroll;
    w->resize_callback = (void*)image_viewer_on_resize;
    w->close_callback = (void*)image_viewer_on_close;

    w->menu_count = 3;
    strcpy(w->menus[0].name, "File");
    strcpy(w->menus[0].items[0].label, "Open");
    strcpy(w->menus[0].items[1].label, "Close");
    w->menus[0].item_count = 2;

    strcpy(w->menus[1].name, "View");
    strcpy(w->menus[1].items[0].label, "Zoom In");
    strcpy(w->menus[1].items[1].label, "Zoom Out");
    strcpy(w->menus[1].items[2].label, "Fit to Window");
    strcpy(w->menus[1].items[3].label, "Actual Size");
    w->menus[1].item_count = 4;

    strcpy(w->menus[2].name, "Navigate");
    strcpy(w->menus[2].items[0].label, "Previous");
    strcpy(w->menus[2].items[1].label, "Next");
    w->menus[2].item_count = 2;

    dock_register("Image Viewer", 4, w);
}

void init_image_viewer_app(void) {
    // Check for launch arguments (image path passed via "Open With")
    char launch_path[256];
    launch_path[0] = 0;
    extern void wrap_get_args(char* b, int m);
    wrap_get_args(launch_path, sizeof(launch_path) - 1);

    image_viewer_init();

    // If an image path was provided, open it
    if (launch_path[0] && iv_cur) {
        if (iv_is_image_file(launch_path)) {
            iv_open_image(iv_cur, launch_path);
            // Update title
            extern window_t* active_win;
            if (active_win) {
                iv_update_title(iv_cur, active_win);
            }
        }
    }
}

// Render function (called externally if needed)
void image_viewer_render(void) {
    // Currently rendering is handled by the window server paint callback
    // This stub exists for API compatibility
}

// Event handler (called externally if needed)
void image_viewer_handle_event(int event_type, int param1, int param2) {
    // Generic event dispatch — can be extended for IPC events
    // event_type: 0=key, 1=mouse, 2=scroll, 3=drop
    if (!iv_cur) return;

    switch (event_type) {
    case 3:  // Drop event — param1 is a string pointer cast to int
        image_viewer_open((const char*)(uintptr_t)param1);
        break;
    default:
        break;
    }
}
