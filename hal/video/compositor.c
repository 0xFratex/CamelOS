// hal/video/compositor.c
#include "compositor.h"
#include "gfx_hal.h"
#include "../../core/window_server.h"

// Fast shadow drawing using alpha blending on edges
void compositor_draw_shadow(int x, int y, int w, int h, int radius, int active) {
    uint32_t shadow_col = 0x40000000; // 25% Black
    if (active) shadow_col = 0x60000000; // Darker for active window

    // Bottom-Right heavy shadow for depth perception
    int offset_y = active ? 8 : 4;
    int offset_x = active ? 0 : 0; // Centered horz, dropped vert

    // Draw localized shadow rects (simulating blur via multiple rects would be too slow in software)
    // We draw a simplified translucent box behind the window

    // Main shadow body
    gfx_fill_rounded_rect(x + offset_x, y + offset_y, w, h, shadow_col, radius + 2);
}

// Draw a window frame with support for focus state and opacity
void compositor_draw_window(window_t* win) {
    if (!win->is_visible) return;

    // 1. Shadows (only if not maximized)
    if (win->state != WIN_STATE_MAXIMIZED) {
        compositor_draw_shadow(win->x, win->y, win->width, win->height, 10, win->is_focused);
    }

    // 2. Main Window Body
    // Check if we need to draw transparently (ghosting during drag or animation)
    uint32_t bg_color = 0xFFF6F6F6; // Default macOS-like gray

    if (win->opacity < 1.0f) {
        // Software alpha blending for the whole window is expensive.
        // We implement a "screen door" transparency effect or simple alpha on background only.
        // For this implementation, we assume opaque body for performance,
        // but blend the border/header.
    }

    gfx_fill_rounded_rect(win->x, win->y, win->width, win->height, bg_color, 10);

    // 3. Header Separator
    gfx_draw_line(win->x, win->y + 28, win->x + win->width, win->y + 28, 0xFFD4D4D4);

    // 4. Traffic Lights
    int traffic_y = win->y + 10;
    int traffic_size = 12;
    int traffic_spacing = 8;

    // Close button (red)
    gfx_fill_rect(win->x + traffic_spacing, traffic_y, traffic_size, traffic_size, 0xFFFF3B30);
    gfx_draw_rect(win->x + traffic_spacing, traffic_y, traffic_size, traffic_size, 0xFF000000);

    // Minimize button (yellow)
    gfx_fill_rect(win->x + traffic_spacing * 2 + traffic_size, traffic_y, traffic_size, traffic_size, 0xFFFFFFBD);
    gfx_draw_rect(win->x + traffic_spacing * 2 + traffic_size, traffic_y, traffic_size, traffic_size, 0xFF000000);

    // Maximize button (green)
    gfx_fill_rect(win->x + traffic_spacing * 3 + traffic_size * 2, traffic_y, traffic_size, traffic_size, 0xFF34C759);
    gfx_draw_rect(win->x + traffic_spacing * 3 + traffic_size * 2, traffic_y, traffic_size, traffic_size, 0xFF000000);
}

void compositor_draw_blur_backdrop(int x, int y, int w, int h) {
    // Placeholder for blur effect
    // In software: Read pixels, average them, write back.
    // Extremely slow for realtime. We skip for now or use a dither pattern.
    gfx_fill_rect(x, y, w, h, 0x80FFFFFF); // Milky glass overlay
}