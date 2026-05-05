// hal/video/compositor.c - CamelOS Window Compositor
// Enhanced: 4-layer soft shadows, AA rounded corners, per-window shadow/corner/opacity,
//           traffic lights with hover icons, smooth gradient header, frosted glass backdrop

#include "compositor.h"
#include "gfx_hal.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../core/window_server.h"

// ============================================================================
// Soft Shadow Drawing (macOS-style multi-layer shadow)
// ============================================================================

void compositor_draw_shadow(int x, int y, int w, int h, int radius, int active) {
    // 4-layer shadow for natural depth (macOS-style)
    
    // Layer 1: Outermost soft shadow (large offset, very light)
    uint32_t shadow_col1 = active ? 0x0C000000 : 0x08000000;
    int offset1 = active ? 8 : 4;
    gfx_fill_rounded_rect_aa(x - 3 + offset1, y - 3 + offset1, w + 6, h + 6, shadow_col1, radius + 6);
    
    // Layer 2: Middle shadow
    uint32_t shadow_col2 = active ? 0x18000000 : 0x12000000;
    int offset2 = active ? 5 : 3;
    gfx_fill_rounded_rect_aa(x - 2 + offset2, y - 2 + offset2, w + 4, h + 4, shadow_col2, radius + 4);
    
    // Layer 3: Inner shadow (darkest, closest)
    uint32_t shadow_col3 = active ? 0x28000000 : 0x1C000000;
    int offset3 = active ? 3 : 1;
    gfx_fill_rounded_rect_aa(x - 1 + offset3, y - 1 + offset3, w + 2, h + 2, shadow_col3, radius + 2);
    
    // Layer 4: Contact shadow (very dark, minimal offset - ground effect)
    if (active) {
        gfx_fill_rounded_rect_aa(x, y + h, w, 4, 0x15000000, 2);
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
    uint32_t bg_color = win->background_color ? win->background_color : 0xFFF6F6F6;
    
    // Apply window opacity
    if (win->opacity < 1.0f) {
        uint8_t a = (uint8_t)(win->opacity * 255);
        bg_color = (a << 24) | (bg_color & 0x00FFFFFF);
    }
    
    // Use AA rounded rect for smooth edges
    gfx_fill_rounded_rect_aa(win->x, win->y, win->width, win->height, bg_color, corner_radius);
    
    // 3. Header Bar with smooth gradient (macOS-style)
    // The header must respect the same circular arc used by
    // gfx_fill_rounded_rect_aa so that the gradient aligns perfectly
    // with the window body's rounded top corners.  Previously the inset
    // was computed as (corner_radius - row), which is a linear ramp that
    // doesn't match the circle equation, causing visible corner artifacts
    // (body color bleeding through where the header should cover).
    for (int i = 0; i < 28; i++) {
        // Smooth gradient from 0xFFF0F0F0 to 0xFFE8E8E8
        float t = (float)i / 27.0f;
        uint8_t gray = (uint8_t)(0xF0 + (0xE8 - 0xF0) * t);
        uint32_t header_col = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;

        // Only fill within the rounded top corners
        if (i < corner_radius) {
            // Use the same circle equation as gfx_fill_rounded_rect_aa:
            //   circle center is at (R-1, R-1) relative to top-left
            //   at row i, the horizontal offset from the circle edge is:
            //     R - sqrt(R^2 - (R-1-i)^2)
            //   which gives the inset from the window edge.
            int dy = corner_radius - 1 - i;  // distance from circle center
            int r2 = corner_radius * corner_radius;
            // Integer square-root approximation for the inset
            int inset = 0;
            if (dy < corner_radius) {
                // Compute inset = R - sqrt(R^2 - dy^2) using integer math
                // Use a simple iterative sqrt approximation
                int d2 = dy * dy;
                int diff = r2 - d2;
                if (diff >= 0) {
                    // Fast integer sqrt via Newton's method
                    int sq = diff;
                    // Initial guess
                    int est = sq;
                    if (est > 0) {
                        // Newton iterations (3 is enough for R<=20)
                        for (int iter = 0; iter < 3; iter++) {
                            est = (est + sq / est) / 2;
                        }
                        inset = corner_radius - est;
                    } else {
                        inset = corner_radius;
                    }
                }
            }
            if (inset > 0) {
                gfx_fill_rect(win->x + inset, win->y + i, win->width - 2 * inset, 1, header_col);
            } else {
                gfx_fill_rect(win->x, win->y + i, win->width, 1, header_col);
            }
        } else {
            gfx_fill_rect(win->x, win->y + i, win->width, 1, header_col);
        }
    }
    
    // Header separator line (thin, subtle)
    gfx_draw_line(win->x + 1, win->y + 28, win->x + win->width - 1, win->y + 28, 0xFFD4D4D4);
    
    // 4. Traffic Lights (macOS-style with hover icons)
    int traffic_y = win->y + 10;
    int traffic_spacing = 8;
    int traffic_size = 12;
    
    // Get mouse for hover state
    int mx, my, dummy;
    sys_mouse_read(&mx, &my, &dummy);
    int in_traffic_area = (mx >= win->x && mx < win->x + 70 &&
                          my >= win->y + 6 && my < win->y + 22);
    
    // Close button (red)
    uint32_t close_col = in_traffic_area ? 0xFFFF5F57 : 0xFFFF3B30;
    gfx_fill_rounded_rect(win->x + traffic_spacing, traffic_y, traffic_size, traffic_size, close_col, 6);
    gfx_draw_rect(win->x + traffic_spacing, traffic_y, traffic_size, traffic_size, 0xFFD4D4D4);
    if (in_traffic_area) {
        // Draw X icon inside
        int bx = win->x + traffic_spacing + 3;
        int by = traffic_y + 3;
        gfx_draw_line(bx, by, bx + 5, by + 5, 0xFF4D0000);
        gfx_draw_line(bx + 5, by, bx, by + 5, 0xFF4D0000);
    }
    
    // Minimize button (yellow/amber)
    int min_x = win->x + traffic_spacing * 2 + traffic_size;
    uint32_t min_col = in_traffic_area ? 0xFFFFBD4E : 0xFFFFFFBD;
    gfx_fill_rounded_rect(min_x, traffic_y, traffic_size, traffic_size, min_col, 6);
    gfx_draw_rect(min_x, traffic_y, traffic_size, traffic_size, 0xFFD4D4D4);
    if (in_traffic_area) {
        // Draw - icon inside
        gfx_fill_rect(min_x + 3, traffic_y + 5, 6, 2, 0xFF9A6900);
    }
    
    // Maximize/fullscreen button (green)
    int max_x = win->x + traffic_spacing * 3 + traffic_size * 2;
    uint32_t max_col = in_traffic_area ? 0xFF28C840 : 0xFF34C759;
    gfx_fill_rounded_rect(max_x, traffic_y, traffic_size, traffic_size, max_col, 6);
    gfx_draw_rect(max_x, traffic_y, traffic_size, traffic_size, 0xFFD4D4D4);
    if (in_traffic_area) {
        // Draw expand icon inside
        int bx2 = max_x + 3;
        int by2 = traffic_y + 3;
        gfx_draw_line(bx2, by2 + 2, bx2 + 5, by2 + 2, 0xFF006400);
        gfx_draw_line(bx2 + 2, by2, bx2 + 2, by2 + 5, 0xFF006400);
    }
    
    // 5. Title text (centered in header with shadow, focus-aware color)
    if (win->title[0]) {
        int title_w = strlen(win->title) * 8;
        int title_x = win->x + (win->width - title_w) / 2;
        int title_y = win->y + 9;
        
        // Title shadow (subtle)
        gfx_draw_string(title_x + 1, title_y + 1, win->title, 0x30000000);
        // Title text - gray when unfocused
        uint32_t title_col = win->is_focused ? 0xFF333333 : 0xFF999999;
        gfx_draw_string(title_x, title_y, win->title, title_col);
    }
    
    // 6. Window border stroke (rounded to match the window body corners)
    // Focused windows get a subtle shadow/border, unfocused get a lighter border
    if (win->state != WIN_STATE_MAXIMIZED) {
        if (win->is_focused) {
            // Focused: subtle rounded border with slight shadow effect
            gfx_stroke_rounded_rect(win->x, win->y, win->width, win->height, 0xFFB8B8B8, corner_radius, 1);
        } else {
            // Unfocused: lighter rounded border
            gfx_stroke_rounded_rect(win->x, win->y, win->width, win->height, 0xFFD0D0D0, corner_radius, 1);
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

void compositor_draw_reflection(int x, int y, int w, int h, float opacity) {
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
