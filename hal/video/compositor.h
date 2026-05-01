#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "../../core/window_server.h"

// Draw a soft multi-layer shadow for a window
void compositor_draw_shadow(int x, int y, int w, int h, int radius, int active);

// Draw a complete window frame (shadow, body, header, traffic lights, title)
// Respects per-window: opacity, corner_radius, has_shadow, shadow_radius, background_color
void compositor_draw_window(window_t* win);

// Draw a frosted glass backdrop (reads from pre-blurred wallpaper buffer)
void compositor_draw_blur_backdrop(int x, int y, int w, int h);

// Generate a box-blurred version of the current framebuffer into the blur buffer
// Call this once after drawing the wallpaper, before drawing windows
void compositor_generate_blur_buffer(void);

// Draw a dock-style reflection effect below a window
void compositor_draw_reflection(int x, int y, int w, int h, float opacity);

#endif
