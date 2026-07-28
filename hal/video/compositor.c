// hal/video/compositor.c - CamelOS Window Compositor
// Classic Aqua refresh: pinstriped title bars, glossy candy traffic lights,
// multi-layer soft shadows, AA rounded corners, frosted-glass backdrop.
// All chrome colors are sourced from core/theme.h so a single theme_set()
// flips the entire UI light/dark.

#include "compositor.h"
#include "gfx_hal.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../core/window_server.h"
#include "../../core/theme.h"

// ============================================================================
// Soft Shadow Drawing (macOS-style multi-layer shadow)
// ============================================================================

void compositor_draw_shadow(int x, int y, int w, int h, int radius, int active) {
    // Theme-driven 4-layer shadow for natural depth (Classic Aqua style)
    const theme_t* t = theme_get_current();

    // Layer 1: Outermost soft shadow (large offset, very light)
    uint32_t shadow_col1 = active ? t->shadow_soft : theme_alpha(t->shadow_soft, 0x08);
    int offset1 = active ? 8 : 4;
    gfx_fill_rounded_rect_aa(x - 3 + offset1, y - 3 + offset1, w + 6, h + 6, shadow_col1, radius + 6);

    // Layer 2: Middle shadow
    uint32_t shadow_col2 = active ? t->shadow_medium : theme_alpha(t->shadow_medium, 0x06);
    int offset2 = active ? 5 : 3;
    gfx_fill_rounded_rect_aa(x - 2 + offset2, y - 2 + offset2, w + 4, h + 4, shadow_col2, radius + 4);

    // Layer 3: Inner shadow (darkest, closest)
    uint32_t shadow_col3 = active ? theme_alpha(t->shadow_medium, 0x40)
                                  : theme_alpha(t->shadow_medium, 0x20);
    int offset3 = active ? 3 : 1;
    gfx_fill_rounded_rect_aa(x - 1 + offset3, y - 1 + offset3, w + 2, h + 2, shadow_col3, radius + 2);

    // Layer 4: Contact shadow (very dark, minimal offset - ground effect)
    if (active) {
        gfx_fill_rounded_rect_aa(x, y + h, w, 4, t->shadow_soft, 2);
    }
}

// ============================================================================
// Window Drawing with Enhanced Visuals
// ============================================================================

void compositor_draw_window(window_t* win) {
    if (!win->is_visible) return;
    
    // Read window visual properties (respect per-window settings)
    int corner_radius = win->corner_radius > 0 ? win->corner_radius : 10;
    int has_shadow = win->has_shadow;  // 0=default=yes, explicit 0=no shadow
    if (has_shadow == 0 && win->state != WIN_STATE_MAXIMIZED) {
        // Default: draw shadow unless explicitly disabled or maximized
        // has_shadow=0 means "use default" which IS to draw shadows
        // To explicitly disable, set has_shadow=-1
    }
    int draw_shadow = (win->state != WIN_STATE_MAXIMIZED) && (win->has_shadow >= 0);
    
    // 1. Shadows (respect per-window shadow properties)
    if (draw_shadow) {
        int shadow_radius = win->shadow_radius > 0 ? win->shadow_radius : 10;
        compositor_draw_shadow(win->x, win->y, win->width, win->height,
                              shadow_radius, win->is_focused);
    }
    
    // 2. Main Window Body with rounded corners
    const theme_t* theme = theme_get_current();
    uint32_t bg_color = win->background_color ? win->background_color : theme->window_body;
    
    // Apply window opacity
    if (win->opacity < 1.0f) {
        uint8_t a = (uint8_t)(win->opacity * 255);
        bg_color = (a << 24) | (bg_color & 0x00FFFFFF);
    }
    
    // Use AA rounded rect for smooth edges
    gfx_fill_rounded_rect_aa(win->x, win->y, win->width, win->height, bg_color, corner_radius);
    
    // 3. Header Bar — Classic Aqua pinstriped title bar with smooth gradient.
    // The header must respect the same circular arc used by
    // gfx_fill_rounded_rect_aa so that the gradient aligns perfectly
    // with the window body's rounded top corners.
    // We use the EXACT same per-pixel circle test as gfx_fill_rounded_rect_aa:
    //   circle center at (R-1, R-1) relative to the corner, pixel (dx,dy) is
    //   inside if (R-1-dx)^2 + (R-1-dy)^2 <= R^2
    int header_h = AQUA_TITLEBAR_HEIGHT; // Must match HEADER_HEIGHT in bubbleview.c
    {
        int r = corner_radius;
        int r2 = r * r;

        for (int row = 0; row < header_h; row++) {
            // Smooth gradient using theme titlebar colors
            uint32_t top_col = win->is_focused ? theme->window_titlebar : theme->window_titlebar_unfocused;
            uint32_t bot_col = theme->window_titlebar_unfocused;
            // Interpolate
            uint8_t top_r = (top_col >> 16) & 0xFF, top_g = (top_col >> 8) & 0xFF, top_b = top_col & 0xFF;
            uint8_t bot_r = (bot_col >> 16) & 0xFF, bot_g = (bot_col >> 8) & 0xFF, bot_b = bot_col & 0xFF;
            uint8_t gray_r = top_r + ((bot_r - top_r) * row) / (header_h - 1);
            uint8_t gray_g = top_g + ((bot_g - top_g) * row) / (header_h - 1);
            uint8_t gray_b = top_b + ((bot_b - top_b) * row) / (header_h - 1);
            uint32_t header_col = (0xFF << 24) | (gray_r << 16) | (gray_g << 8) | gray_b;
            // Classic Aqua pinstripe: every other row gets a subtle overlay
            if (row % 2 == 0) {
                header_col = theme_alpha(header_col, 0xFF);
                // Blend with pinstripe overlay color
                uint32_t ps = theme->pinstripe;
                uint8_t pa = (ps >> 24) & 0xFF;
                if (pa > 0) {
                    uint8_t hr = (header_col >> 16) & 0xFF;
                    uint8_t hg = (header_col >> 8) & 0xFF;
                    uint8_t hb = header_col & 0xFF;
                    uint8_t pr = (ps >> 16) & 0xFF;
                    uint8_t pg = (ps >> 8) & 0xFF;
                    uint8_t pb = ps & 0xFF;
                    hr = (hr * (255 - pa) + pr * pa) / 255;
                    hg = (hg * (255 - pa) + pg * pa) / 255;
                    hb = (hb * (255 - pa) + pb * pa) / 255;
                    header_col = (0xFF << 24) | (hr << 16) | (hg << 8) | hb;
                }
            }

            if (row < r) {
                // Use the exact same per-pixel circle test as gfx_fill_rounded_rect_aa
                // to find the left and right inset for this row
                int left_inset = r;  // Default: full corner width
                int right_inset = r;

                for (int dx = 0; dx < r; dx++) {
                    int cx = r - 1 - dx;
                    int cy = r - 1 - row;
                    if (cx * cx + cy * cy <= r2) {
                        left_inset = dx;
                        break;
                    }
                }
                for (int dx = 0; dx < r; dx++) {
                    int cx = r - 1 - dx;
                    int cy = r - 1 - row;
                    if (cx * cx + cy * cy <= r2) {
                        right_inset = dx;
                        break;
                    }
                }

                int row_w = win->width - left_inset - right_inset;
                if (row_w > 0) {
                    gfx_fill_rect(win->x + left_inset, win->y + row, row_w, 1, header_col);
                }
            } else {
                gfx_fill_rect(win->x, win->y + row, win->width, 1, header_col);
            }
        }

        // Aqua gloss highlight across the top ~40% of the title bar
        // (the candy-button sheen — makes the titlebar look like glossy plastic)
        int gloss_h = header_h * 2 / 5;
        if (gloss_h > 1 && corner_radius > 0) {
            // Inset by corner_radius on the top so the gloss respects rounded corners
            gfx_fill_rounded_rect(win->x + 2, win->y + 1,
                                  win->width - 4, gloss_h,
                                  theme_alpha(0xFFFFFF, win->is_focused ? 0x33 : 0x18),
                                  corner_radius - 1 > 0 ? corner_radius - 1 : 1);
        }
    }

    // Header separator line (thin, subtle) — uses theme
    gfx_draw_line(win->x + 1, win->y + header_h - 1, win->x + win->width - 1, win->y + header_h - 1, theme->separator);
    
    // 4. Traffic Lights — Classic Aqua glossy candy pills (12x12 rounded squares).
    // Colors come from theme so they flip with light/dark mode.
    //
    // Helper: draw one candy pill with shadow + body + border + top gloss.
    // Glyph drawing is left to the caller (passed via glyph_fn 0/1/2 = X/-/+).
    int traffic_y = win->y + (header_h - 12) / 2;  // Vertically center in header
    int traffic_spacing = 8;
    int traffic_size = 12;
    int tl_r = 6;

    // Get mouse for hover state
    int mx, my, dummy;
    sys_mouse_read(&mx, &my, &dummy);
    int in_traffic_area = (mx >= win->x && mx < win->x + 70 &&
                          my >= traffic_y - 2 && my < traffic_y + traffic_size + 2);

    /* draw_candy_pill: draws one pill. glyph: 0=none, 1=X, 2=minus, 3=plus */
    auto void draw_candy_pill(int fx, int fy, uint32_t col_hover, uint32_t col_idle,
                              uint32_t glyph_col, int glyph);
    auto void draw_candy_pill(int fx, int fy, uint32_t col_hover, uint32_t col_idle,
                              uint32_t glyph_col, int glyph) {
        uint32_t col = in_traffic_area ? col_hover : col_idle;
        /* drop shadow */
        gfx_fill_rounded_rect(fx - 1, fy + 1, traffic_size + 2, traffic_size + 2,
                              theme->shadow_soft, tl_r);
        /* body */
        gfx_fill_rounded_rect(fx, fy, traffic_size, traffic_size, col, tl_r);
        /* border */
        gfx_stroke_rounded_rect(fx, fy, traffic_size, traffic_size,
                                theme->tl_border, tl_r, 1);
        /* top-half gloss */
        gfx_fill_rounded_rect(fx + 1, fy + 1, traffic_size - 2, traffic_size / 2 - 1,
                              theme->gloss_highlight, tl_r - 1 > 0 ? tl_r - 1 : 1);
        /* glyph on hover */
        if (in_traffic_area && glyph) {
            if (glyph == 1) {  /* X */
                int bx = fx + 3, by = fy + 3;
                gfx_draw_line(bx, by, bx + 5, by + 5, glyph_col);
                gfx_draw_line(bx + 5, by, bx, by + 5, glyph_col);
            } else if (glyph == 2) {  /* - */
                gfx_fill_rect(fx + 3, fy + 5, 6, 2, glyph_col);
            } else if (glyph == 3) {  /* + */
                int bx2 = fx + 3, by2 = fy + 3;
                gfx_draw_line(bx2, by2 + 2, bx2 + 5, by2 + 2, glyph_col);
                gfx_draw_line(bx2 + 2, by2, bx2 + 2, by2 + 5, glyph_col);
            }
        }
    }

    // Close (red) — X glyph
    draw_candy_pill(win->x + traffic_spacing, traffic_y,
                    theme->tl_close, theme->tl_close_idle, 0xFF4D0000, 1);
    // Minimize (yellow) — minus glyph
    draw_candy_pill(win->x + traffic_spacing * 2 + traffic_size, traffic_y,
                    theme->tl_min, theme->tl_min_idle, 0xFF9A6900, 2);
    // Maximize (green) — plus glyph
    draw_candy_pill(win->x + traffic_spacing * 3 + traffic_size * 2, traffic_y,
                    theme->tl_max, theme->tl_max_idle, 0xFF006400, 3);
    
    // 5. Title text (centered in header with shadow, focus-aware color)
    if (win->title[0]) {
        int title_w = strlen(win->title) * 8;
        int title_x = win->x + (win->width - title_w) / 2;
        int title_y = win->y + (header_h - 8) / 2 + 1;  // Center text in header

        // Title shadow — Classic Aqua used a subtle white halo on light headers
        // for embossed feel. We use theme->shadow_title.
        gfx_draw_string(title_x + 1, title_y + 1, win->title, theme->shadow_title);
        // Title text - uses theme colors
        uint32_t title_col = win->is_focused ? theme->window_title_text : theme->window_title_text_unfocused;
        gfx_draw_string(title_x, title_y, win->title, title_col);
    }
    
    // 6. Window border stroke (rounded to match the window body corners)
    // Focused windows get a subtle shadow/border, unfocused get a lighter border
    if (win->state != WIN_STATE_MAXIMIZED) {
        if (win->is_focused) {
            // Focused: subtle rounded border with slight shadow effect
            gfx_stroke_rounded_rect(win->x, win->y, win->width, win->height, theme->window_border, corner_radius, 1);
        } else {
            // Unfocused: lighter rounded border
            gfx_stroke_rounded_rect(win->x, win->y, win->width, win->height, theme->window_border_unfocused, corner_radius, 1);
        }
    }
}

// ============================================================================
// Blur Backdrop (Frosted Glass Effect)
// ============================================================================

void compositor_draw_blur_backdrop(int x, int y, int w, int h) {
    // Check if blur buffer is available (pre-blurred wallpaper)
    uint32_t* blur_buf = gfx_get_blur_buffer();
    uint32_t* back_buf = gfx_get_active_buffer();
    
    if (!blur_buf || !back_buf) {
        // Fallback: translucent white overlay (milky glass)
        gfx_fill_rect(x, y, w, h, 0x80FFFFFF);
        return;
    }
    
    int screen_w = gfx_get_width();
    int screen_h = gfx_get_height();
    
    // Sample from blur buffer and blend with white tint
    for (int dy = 0; dy < h; dy++) {
        int ly = y + dy;
        if (ly < 0 || ly >= screen_h) continue;
        
        for (int dx = 0; dx < w; dx++) {
            int lx = x + dx;
            if (lx < 0 || lx >= screen_w) continue;
            
            int idx = ly * screen_w + lx;
            uint32_t bg = blur_buf[idx];
            
            // Extract RGB from blurred background
            uint8_t bg_r = (bg >> 16) & 0xFF;
            uint8_t bg_g = (bg >> 8) & 0xFF;
            uint8_t bg_b = bg & 0xFF;
            
            // Blend 40% white over the blurred background (frosted glass tint)
            uint8_t r = (bg_r * 60 + 0xFF * 40) / 100;
            uint8_t g = (bg_g * 60 + 0xFF * 40) / 100;
            uint8_t b = (bg_b * 60 + 0xFF * 40) / 100;
            
            back_buf[idx] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
    
    // Draw subtle white rim for glass edge effect
    gfx_draw_rect(x, y, w, h, 0x40FFFFFF);
}

// ============================================================================
// Box Blur Buffer Generation (3-pass)
// Call once after drawing wallpaper, before drawing windows
// ============================================================================

void compositor_generate_blur_buffer(void) {
    uint32_t* blur_buf = gfx_get_blur_buffer();
    uint32_t* src_buf = gfx_get_active_buffer();
    
    if (!blur_buf || !src_buf) return;
    
    int w = gfx_get_width();
    int h = gfx_get_height();
    
    // Copy source to blur buffer first
    memcpy(blur_buf, src_buf, w * h * 4);
    
    // 3-pass box blur for approximate Gaussian blur
    int blur_radius = 4;
    
    // Allocate temp buffer for one row
    uint32_t* temp_row = (uint32_t*)kmalloc(w * 4);
    if (!temp_row) return;
    
    // Pass 1: Horizontal blur
    for (int y = 0; y < h; y++) {
        uint32_t* row = &blur_buf[y * w];
        memcpy(temp_row, row, w * 4);
        
        for (int x = 0; x < w; x++) {
            int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
            int x_start = x - blur_radius; if (x_start < 0) x_start = 0;
            int x_end = x + blur_radius; if (x_end >= w) x_end = w - 1;
            
            for (int kx = x_start; kx <= x_end; kx++) {
                uint32_t px = temp_row[kx];
                r_sum += (px >> 16) & 0xFF;
                g_sum += (px >> 8) & 0xFF;
                b_sum += px & 0xFF;
                count++;
            }
            row[x] = 0xFF000000 | ((r_sum / count) << 16) | ((g_sum / count) << 8) | (b_sum / count);
        }
    }
    
    // Pass 2: Vertical blur
    uint32_t* temp_col = (uint32_t*)kmalloc(h * 4);
    if (!temp_col) { kfree(temp_row); return; }
    
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) temp_col[y] = blur_buf[y * w + x];
        
        for (int y = 0; y < h; y++) {
            int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
            int y_start = y - blur_radius; if (y_start < 0) y_start = 0;
            int y_end = y + blur_radius; if (y_end >= h) y_end = h - 1;
            
            for (int ky = y_start; ky <= y_end; ky++) {
                uint32_t px = temp_col[ky];
                r_sum += (px >> 16) & 0xFF;
                g_sum += (px >> 8) & 0xFF;
                b_sum += px & 0xFF;
                count++;
            }
            blur_buf[y * w + x] = 0xFF000000 | ((r_sum / count) << 16) | ((g_sum / count) << 8) | (b_sum / count);
        }
    }
    
    // Pass 3: Second horizontal pass for smoother blur
    for (int y = 0; y < h; y++) {
        uint32_t* row = &blur_buf[y * w];
        memcpy(temp_row, row, w * 4);
        
        for (int x = 0; x < w; x++) {
            int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
            int x_start = x - blur_radius; if (x_start < 0) x_start = 0;
            int x_end = x + blur_radius; if (x_end >= w) x_end = w - 1;
            
            for (int kx = x_start; kx <= x_end; kx++) {
                uint32_t px = temp_row[kx];
                r_sum += (px >> 16) & 0xFF;
                g_sum += (px >> 8) & 0xFF;
                b_sum += px & 0xFF;
                count++;
            }
            row[x] = 0xFF000000 | ((r_sum / count) << 16) | ((g_sum / count) << 8) | (b_sum / count);
        }
    }
    
    kfree(temp_row);
    kfree(temp_col);
}

// ============================================================================
// Dock-style Reflection Effect
// ============================================================================

static void compositor_draw_reflection_v1(int x, int y, int w, int h, float opacity) {
    uint32_t* back_buf = gfx_get_active_buffer();
    if (!back_buf) return;
    
    int screen_w = gfx_get_width();
    int screen_h = gfx_get_height();
    int refl_h = h / 3;  // Reflection is 1/3 the height
    
    for (int dy = 0; dy < refl_h; dy++) {
        int src_y = y + h - 1 - dy;  // Source from bottom of original
        int dst_y = y + h + dy;       // Destination below
        
        if (src_y < 0 || src_y >= screen_h || dst_y < 0 || dst_y >= screen_h) continue;
        
        float fade = 1.0f - ((float)dy / refl_h);
        uint8_t alpha = (uint8_t)(opacity * fade * 255.0f);
        
        for (int dx = 0; dx < w; dx++) {
            int sx = x + dx;
            if (sx < 0 || sx >= screen_w) continue;
            
            uint32_t src_pixel = back_buf[src_y * screen_w + sx];
            uint32_t dst_pixel = back_buf[dst_y * screen_w + sx];
            
            uint8_t sr = (src_pixel >> 16) & 0xFF;
            uint8_t sg = (src_pixel >> 8) & 0xFF;
            uint8_t sb = src_pixel & 0xFF;
            uint8_t dr = (dst_pixel >> 16) & 0xFF;
            uint8_t dg = (dst_pixel >> 8) & 0xFF;
            uint8_t db = dst_pixel & 0xFF;
            
            uint8_t r = (sr * alpha + dr * (255 - alpha)) / 255;
            uint8_t g = (sg * alpha + dg * (255 - alpha)) / 255;
            uint8_t b = (sb * alpha + db * (255 - alpha)) / 255;
            
            back_buf[dst_y * screen_w + sx] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}
