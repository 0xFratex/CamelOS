#include "camel_ui.h"
#include "../../sys/cdl_defs.h"

extern kernel_api_t* sys;

#define C_WIN_BG     0xFFF6F6F6
#define C_WIN_BORDER 0xFF888888 // Crisp border color

// Modern macOS-like traffic light colors
#define C_BTN_RED    0xFFFF5F57
#define C_BTN_YEL    0xFFFFBD2E
#define C_BTN_GRN    0xFF28C940

#define C_BTN_RED_D  0xFFE0443E
#define C_BTN_YEL_D  0xFFE0A028
#define C_BTN_GRN_D  0xFF1CAC2F

#define C_SYMBOL     0xFF4A0C09

// Helper to draw pixel symbols inside buttons
void ui_draw_traffic_icon(kernel_api_t* api, int cx, int cy, int type) {
    // type: 0=close(x), 1=min(-), 2=max(+)
    // Center point cx,cy.

    if (type == 0) { // X
        // crude pixel X
        api->draw_rect(cx-2, cy-2, 5, 1, C_SYMBOL); // \ (simulated)
        api->draw_rect(cx-2, cy+2, 5, 1, C_SYMBOL); // / (simulated)
        // Better cross
        api->draw_text(cx-3, cy-4, "x", C_SYMBOL);
    }
    else if (type == 1) { // -
        api->draw_rect(cx-3, cy, 7, 2, C_SYMBOL);
    }
    else if (type == 2) { // +
        api->draw_rect(cx-3, cy, 7, 2, C_SYMBOL); // Horz
        api->draw_rect(cx, cy-3, 2, 7, C_SYMBOL); // Vert
    }
}

// Helper: Filled Circle (Midpoint algorithm simplified for small radii)
void ui_draw_circle(kernel_api_t* api, int cx, int cy, int r, uint32_t color) {
    // Basic scanline circle for small UI elements
    for(int y = -r; y <= r; y++) {
        for(int x = -r; x <= r; x++) {
            if(x*x + y*y <= r*r) {
                api->draw_rect(cx+x, cy+y, 1, 1, color);
            }
        }
    }
}

void ui_draw_circle_aa(kernel_api_t* api, int cx, int cy, int r, uint32_t color) {
    // Simple filled circle with the new AA rect primitive logic or custom
    // For small buttons, we can just use the rounded rect logic with r = width/2
    if(api->draw_rect_rounded)
        api->draw_rect_rounded(cx - r, cy - r, r*2, r*2, color, r);
}

void ui_draw_window_frame_ex(kernel_api_t* api, int x, int y, int w, int h, const char* title, int active, int mx, int my) {
    if (!api) return;

    // 1. Shadow (Larger, softer)
    if(active) {
        // Double pass for "softness"
        api->draw_rect_rounded(x+4, y+8, w, h, 0x10000000, 15);
        api->draw_rect_rounded(x+2, y+4, w, h, 0x20000000, 15);
    } else {
        api->draw_rect_rounded(x+2, y+4, w, h, 0x15000000, 15);
    }

    // 2. Main Window (White/Grey with slight transparency)
    uint32_t bg_col = active ? 0xFFF6F6F6 : 0xFFF0F0F0;
    api->draw_rect_rounded(x, y, w, h, bg_col, 12);
    
    // 3. Thin Border (Stroke) - Simulated by drawing inner
    // Not implemented in API yet, skipping for clean look.

    // 4. Header Separator
    api->draw_rect(x, y+32, w, 1, 0xFFD8D8D8);

    // 5. Traffic Lights (AA Circles)
    int btn_y = y + 16;
    
    // Hover State for Symbols
    int hover = (mx >= x+10 && mx <= x+70 && my >= y+6 && my <= y+26);
    
    ui_draw_circle_aa(api, x+20, btn_y, 6, active ? C_BTN_RED : 0xFFCECECE);
    ui_draw_circle_aa(api, x+40, btn_y, 6, active ? C_BTN_YEL : 0xFFCECECE);
    ui_draw_circle_aa(api, x+60, btn_y, 6, active ? C_BTN_GRN : 0xFFCECECE);

    // Symbols on Hover
    if (active && hover) {
        // X
        api->draw_text(x+17, btn_y-4, "x", 0xFF500000);
        // -
        api->draw_rect(x+37, btn_y, 6, 1, 0xFF503000);
        // +
        api->draw_rect(x+57, btn_y, 6, 1, 0xFF003000);
        api->draw_rect(x+59, btn_y-2, 2, 5, 0xFF003000);
    }

    // 6. Title
    if (title) {
        int tlen = api->strlen(title) * 7;
        int tx = x + (w - tlen) / 2;
        uint32_t tcol = active ? 0xFF3E3E3E : 0xFF999999;
        api->draw_text(tx, y+10, title, tcol);
    }
}

// Draw a standardized Context Menu
void ui_draw_context_menu(kernel_api_t* api, int x, int y, const char** items, int count, int hover_idx) {
    if(!api) return;

    int w = 160;
    int h = count * 24 + 6;

    // Bounds check to keep menu on screen
    if(x + w > 1024) x = 1024 - w;
    if(y + h > 768) y = 768 - h;

    // Drop Shadow
    api->draw_rect_rounded(x+4, y+4, w, h, 0x30000000, 6);

    // Background
    api->draw_rect_rounded(x, y, w, h, 0xFFF8F8F8, 6);

    // Crisp Border
    api->draw_rect(x, y, w, 1, 0xFFBBBBBB);
    api->draw_rect(x, y+h-1, w, 1, 0xFFBBBBBB);
    api->draw_rect(x, y, 1, h, 0xFFBBBBBB);
    api->draw_rect(x+w-1, y, 1, h, 0xFFBBBBBB);

    for(int i=0; i<count; i++) {
        int iy = y + 4 + (i * 24);

        if (api->strcmp(items[i], "-") == 0) {
            api->draw_rect(x+10, iy+11, w-20, 1, 0xFFD0D0D0);
            continue;
        }

        if (i == hover_idx) {
            // Highlight Blue with rounded corners inside
            api->draw_rect_rounded(x+4, iy, w-8, 22, 0xFF007AFF, 4);
            api->draw_text(x+15, iy+7, items[i], 0xFFFFFFFF);
        } else {
            api->draw_text(x+15, iy+7, items[i], 0xFF000000);
        }
    }
}
