// hal/video/compositor.c
#include "compositor.h"
#include "gfx_hal.h"
#include "../../core/string.h"
#include "../../core/window_server.h"

// Fast shadow drawing using alpha blending on edges
void compositor_draw_shadow(int x, int y, int w, int h, int radius, int active) {
    // Multi-layer shadow for depth (macOS-like soft shadow effect)
    // Layer 1: Outer light shadow (larger offset, lighter)
    uint32_t shadow_col1 = active ? 0x20000000 : 0x15000000;
    int offset1 = active ? 6 : 3;
    gfx_fill_rounded_rect_aa(x - 2 + offset1, y - 2 + offset1, w + 4, h + 4, shadow_col1, radius + 4);
    
    // Layer 2: Inner darker shadow (smaller offset, darker)
    uint32_t shadow_col2 = active ? 0x35000000 : 0x25000000;
    int offset2 = active ? 3 : 1;
    gfx_fill_rounded_rect_aa(x - 1 + offset2, y - 1 + offset2, w + 2, h + 2, shadow_col2, radius + 2);
}

// Apply per-pixel alpha blending for a rectangular region using a global opacity
static void compositor_blend_region(int x, int y, int w, int h, float opacity) {
    if (opacity >= 1.0f || opacity <= 0.0f) return;
    
    uint32_t* buf = gfx_get_active_buffer();
    int sw = gfx_get_width();
    uint8_t alpha = (uint8_t)(opacity * 255);
    uint32_t inv_a = 256 - alpha;
    
    // Blend each pixel against the wallpaper/destination
    for (int row = y; row < y + h && row < 768; row++) {
        if (row < 0) continue;
        for (int col = x; col < x + w && col < 1024; col++) {
            if (col < 0) continue;
            uint32_t* px = &buf[row * sw + col];
            uint32_t c = *px;
            // Reduce the pixel's alpha by the opacity factor
            // This makes the window semi-transparent by blending toward the background
            uint32_t rb = (c & 0xFF00FF) * inv_a >> 8;
            uint32_t g  = (c & 0x00FF00) * inv_a >> 8;
            *px = 0xFF000000 | (rb & 0xFF00FF) | (g & 0x00FF00);
        }
    }
}

// Draw a window frame with support for focus state, opacity, and macOS-like styling
void compositor_draw_window(window_t* win) {
    if (!win->is_visible) return;

    // 1. Shadows (only if not maximized)
    if (win->state != WIN_STATE_MAXIMIZED) {
        compositor_draw_shadow(win->x, win->y, win->width, win->height, 10, win->is_focused);
    }

    // 2. Main Window Body with rounded corners (macOS-style)
    uint32_t bg_color = 0xFFF6F6F6; // Default macOS-like gray
    int corner_radius = win->corner_radius > 0 ? win->corner_radius : 10;

    // Apply window opacity to the background color
    if (win->opacity < 1.0f) {
        uint8_t a = (uint8_t)(win->opacity * 255);
        bg_color = (a << 24) | (bg_color & 0x00FFFFFF);
    }

    gfx_fill_rounded_rect(win->x, win->y, win->width, win->height, bg_color, corner_radius);

    // 3. Header Bar with subtle gradient
    // Draw a light gradient in the title bar area
    for (int i = 0; i < 28; i++) {
        // Top of header is slightly lighter, bottom is slightly darker
        uint32_t header_col = (i < 14) ? 0xFFF0F0F0 : 0xFFE8E8E8;
        // Only fill within the rounded top corners
        if (i < corner_radius) {
            // Use rounded rect clipping approximation for the top rows
            int inset = corner_radius - i;
            if (inset > 0) {
                gfx_fill_rect(win->x + inset, win->y + i, win->width - 2 * inset, 1, header_col);
            } else {
                gfx_fill_rect(win->x, win->y + i, win->width, 1, header_col);
            }
        } else {
            gfx_fill_rect(win->x, win->y + i, win->width, 1, header_col);
        }
    }

    // Header separator line
    gfx_draw_line(win->x, win->y + 28, win->x + win->width, win->y + 28, 0xFFD4D4D4);

    // 4. Traffic Lights (macOS-style circular buttons with hover states)
    int traffic_y = win->y + 10;
    int traffic_spacing = 8;
    int traffic_size = 12;
    int traffic_center_x, traffic_center_y;
    
    // Get mouse for hover state
    int mx, my, dummy;
    sys_mouse_read(&mx, &my, &dummy);
    int in_traffic_area = (mx >= win->x && mx < win->x + 70 &&
                          my >= win->y + 6 && my < win->y + 22);

    // Close button (red) - slightly larger circle with inner highlight
    traffic_center_x = win->x + traffic_spacing + traffic_size / 2;
    traffic_center_y = traffic_y + traffic_size / 2;
    uint32_t close_col = in_traffic_area ? 0xFFFF5F57 : 0xFFFF3B30;
    gfx_fill_rounded_rect(win->x + traffic_spacing, traffic_y, traffic_size, traffic_size, close_col, 6);
    gfx_draw_rect(win->x + traffic_spacing, traffic_y, traffic_size, traffic_size, 0xFFD4D4D4);
    // Inner highlight
    if (in_traffic_area) {
        gfx_fill_rect(win->x + traffic_spacing + 3, traffic_y + 5, 6, 2, 0x80FFFFFF);
    }

    // Minimize button (yellow)
    int min_x = win->x + traffic_spacing * 2 + traffic_size;
    traffic_center_x = min_x + traffic_size / 2;
    uint32_t min_col = in_traffic_area ? 0xFFFFBD4E : 0xFFFFFFBD;
    gfx_fill_rounded_rect(min_x, traffic_y, traffic_size, traffic_size, min_col, 6);
    gfx_draw_rect(min_x, traffic_y, traffic_size, traffic_size, 0xFFD4D4D4);
    if (in_traffic_area) {
        gfx_fill_rect(min_x + 3, traffic_y + 5, 6, 2, 0x80FFFFFF);
    }

    // Maximize button (green)
    int max_x = win->x + traffic_spacing * 3 + traffic_size * 2;
    traffic_center_x = max_x + traffic_size / 2;
    uint32_t max_col = in_traffic_area ? 0xFF28C840 : 0xFF34C759;
    gfx_fill_rounded_rect(max_x, traffic_y, traffic_size, traffic_size, max_col, 6);
    gfx_draw_rect(max_x, traffic_y, traffic_size, traffic_size, 0xFFD4D4D4);
    if (in_traffic_area) {
        gfx_fill_rect(max_x + 3, traffic_y + 5, 6, 2, 0x80FFFFFF);
    }
    
    // 5. Title text (centered in header, semi-bold look via double-draw)
    if (win->title[0]) {
        int title_w = strlen(win->title) * 8;
        int title_x = win->x + (win->width - title_w) / 2;
        int title_y = win->y + 9;
        // Shadow
        gfx_draw_string(title_x + 1, title_y + 1, win->title, 0x40000000);
        // Main text
        gfx_draw_string(title_x, title_y, win->title, 0xFF333333);
    }
}

void compositor_draw_blur_backdrop(int x, int y, int w, int h) {
    // Placeholder for blur effect
    // In software: Read pixels, average them, write back.
    // Extremely slow for realtime. We skip for now or use a dither pattern.
    gfx_fill_rect(x, y, w, h, 0x80FFFFFF); // Milky glass overlay
}