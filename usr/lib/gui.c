// usr/lib/gui.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

// Colors
#define C_PRIMARY  0xFF007AFF
#define C_PRESS    0xFF0056B3
#define C_TEXT_W   0xFFFFFFFF
#define C_TEXT_B   0xFF000000
#define C_BG_LIGHT 0xFFE0E0E0

void gui_draw_button(int x, int y, int w, int h, const char* label, int pressed) {
    if (!sys) return;

    uint32_t color = pressed ? C_PRESS : C_PRIMARY;

    // Shadow
    if (!pressed) {
        sys->draw_rect(x+2, y+2, w, h, 0x40000000);
    }

    // Main body (simulated rounded corners by not drawing corners pixels)
    // Using raw rects for now as userspace doesn't have access to the advanced HAL round rect
    // unless we expose it in API. We'll do a blocky style for now.
    sys->draw_rect(x, y, w, h, color);

    // Highlight top
    sys->draw_rect(x, y, w, 1, 0x40FFFFFF);

    // Text centering
    int tlen = sys->strlen(label) * 6;
    int tx = x + (w - tlen) / 2;
    int ty = y + (h - 7) / 2;

    if (pressed) { tx++; ty++; } // Shift text on press

    sys->draw_text(tx, ty, label, C_TEXT_W);
}

void gui_draw_progress(int x, int y, int w, int h, int percent, uint32_t bar_col) {
    if (!sys) return;
    // Bg
    sys->draw_rect(x, y, w, h, 0xFFCCCCCC);
    // Fill
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    int fill = (w * percent) / 100;
    sys->draw_rect(x, y, fill, h, bar_col);
    // Border
    sys->draw_rect(x, y, w, 1, 0xFF888888);
    sys->draw_rect(x, y+h-1, w, 1, 0xFF888888);
    sys->draw_rect(x, y, 1, h, 0xFF888888);
    sys->draw_rect(x+w-1, y, 1, h, 0xFF888888);
}

static cdl_symbol_t my_symbols[] = {
    { "btn", (void*)gui_draw_button },
    { "prog", (void*)gui_draw_progress }
};

static cdl_exports_t my_exports = {
    .lib_name = "CamelGUI", .version = 1, .symbol_count = 2, .symbols = my_symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &my_exports;
}