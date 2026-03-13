#ifndef CAMEL_UI_H
#define CAMEL_UI_H

#include "../../hal/video/gfx_hal.h"
#include "../../sys/cdl_defs.h"

// UX Configuration
#define UI_CORNER_RADIUS 5
#define UI_SHADOW_OFFSET 4
#define UI_SHADOW_ALPHA  0x60000000 // Transparent Black

// Theme Colors (ARGB)
#define UI_COL_WIN_BG    0xFFF0F0F0
#define UI_COL_HEADER    0xFF2D2D2D
#define UI_COL_ACCENT    0xFF007AFF // macOS Blue
#define UI_COL_TEXT      0xFF000000
#define UI_COL_TEXT_W    0xFFFFFFFF
#define UI_COL_BORDER    0xFF888888

// Functions
void ui_draw_window_frame(kernel_api_t* api, int x, int y, int w, int h, const char* title, int active);
void ui_draw_window_frame_ex(kernel_api_t* api, int x, int y, int w, int h, const char* title, int active, int mx, int my);
void ui_draw_button(int x, int y, int w, int h, const char* label, int pressed);
void ui_draw_desktop_bg(int w, int h);
void ui_draw_rounded_rect(int x, int y, int w, int h, uint32_t color, int r);
void ui_draw_context_menu(kernel_api_t* api, int x, int y, const char** items, int count, int hover_idx);

#endif