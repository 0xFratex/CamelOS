// usr/lib/camel_ui.c - Enhanced Window Rendering with Life and Animations
#include "camel_ui.h"
#include "../../sys/cdl_defs.h"

// ============================================================================
// MATH HELPERS - Simple sin/cos for kernel mode (no libm available)
// ============================================================================

// Fast sin approximation using Taylor series (good enough for visuals)
static float fast_sin(float x) {
    // Normalize to [-PI, PI]
    while (x > 3.14159f) x -= 6.28318f;
    while (x < -3.14159f) x += 6.28318f;
    
    // Taylor series: x - x^3/6 + x^5/120
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    return x - x3 / 6.0f + x5 / 120.0f;
}

static float fast_cos(float x) {
    // cos(x) = sin(x + PI/2)
    return fast_sin(x + 1.5708f);
}

// ============================================================================
// COLOR PALETTE - macOS Big Sur Inspired
// ============================================================================
#define C_WIN_BG           0xFFF5F5F7
#define C_WIN_HEADER       0xFFF6F6F6
#define C_WIN_BORDER       0xFFD1D1D6
#define C_WIN_BORDER_LIGHT 0xFFE5E5EA

// Modern traffic light colors with gradients
#define C_BTN_RED          0xFFFF5F57
#define C_BTN_RED_DARK     0xFFE0443E
#define C_BTN_YEL          0xFFFFBD2E
#define C_BTN_YEL_DARK     0xFFDEA123
#define C_BTN_GRN          0xFF28C940
#define C_BTN_GRN_DARK     0xFF1AAA2E

// Symbol colors
#define C_SYMBOL_CLOSE     0xFF4A0C09
#define C_SYMBOL_MIN       0xFF995700
#define C_SYMBOL_MAX       0xFF006400

// Shadow colors
#define C_SHADOW_SOFT      0x15000000
#define C_SHADOW_MEDIUM    0x25000000
#define C_SHADOW_STRONG    0x35000000

// ============================================================================
// ANIMATION STATE
// ============================================================================
static int animation_tick = 0;
static float window_glow_phase = 0.0f;

void ui_animation_tick(void) {
    animation_tick++;
    window_glow_phase += 0.05f;
    if (window_glow_phase > 6.28f) window_glow_phase = 0.0f;
}

// ============================================================================
// DRAWING PRIMITIVES
// ============================================================================

void ui_draw_circle(kernel_api_t* api, int cx, int cy, int r, uint32_t color) {
    for(int y = -r; y <= r; y++) {
        for(int x = -r; x <= r; x++) {
            if(x*x + y*y <= r*r) {
                api->draw_rect(cx+x, cy+y, 1, 1, color);
            }
        }
    }
}

void ui_draw_circle_aa(kernel_api_t* api, int cx, int cy, int r, uint32_t color) {
    if(api->draw_rect_rounded)
        api->draw_rect_rounded(cx - r, cy - r, r*2, r*2, color, r);
}

// Draw a gradient circle for buttons
void ui_draw_gradient_circle(kernel_api_t* api, int cx, int cy, int r, 
                             uint32_t top_color, uint32_t bottom_color) {
    for(int y = -r; y <= r; y++) {
        for(int x = -r; x <= r; x++) {
            if(x*x + y*y <= r*r) {
                // Blend based on y position
                int blend = ((y + r) * 256) / (r * 2);
                uint32_t rt = (top_color >> 16) & 0xFF;
                uint32_t gt = (top_color >> 8) & 0xFF;
                uint32_t bt = top_color & 0xFF;
                uint32_t rb = (bottom_color >> 16) & 0xFF;
                uint32_t gb = (bottom_color >> 8) & 0xFF;
                uint32_t bb = bottom_color & 0xFF;
                
                uint32_t r_final = rt + ((rb - rt) * blend) / 256;
                uint32_t g_final = gt + ((gb - gt) * blend) / 256;
                uint32_t b_final = bt + ((bb - bt) * blend) / 256;
                
                api->draw_rect(cx+x, cy+y, 1, 1, 
                    0xFF000000 | (r_final << 16) | (g_final << 8) | b_final);
            }
        }
    }
}

// Draw traffic light button with hover effect
void ui_draw_traffic_light(kernel_api_t* api, int cx, int cy, int type, 
                          int is_hovered, int is_active) {
    int r = 6;
    
    if (!is_active) {
        // Dimmed state for inactive windows
        ui_draw_circle_aa(api, cx, cy, r, 0xFFCECECE);
        return;
    }
    
    uint32_t top_col, bot_col;
    
    switch (type) {
        case 0: // Close (Red)
            top_col = is_hovered ? 0xFFFF6B6B : 0xFFFF5F57;
            bot_col = is_hovered ? 0xFFE04545 : 0xFFE0443E;
            break;
        case 1: // Minimize (Yellow)
            top_col = is_hovered ? 0xFFFFD93D : 0xFFFFBD2E;
            bot_col = is_hovered ? 0xFFDEB227 : 0xFFDEA123;
            break;
        case 2: // Maximize (Green)
            top_col = is_hovered ? 0xFF4ADE80 : 0xFF28C940;
            bot_col = is_hovered ? 0xFF22B03A : 0xFF1AAA2E;
            break;
        default:
            return;
    }
    
    // Draw gradient circle
    ui_draw_gradient_circle(api, cx, cy, r, top_col, bot_col);
    
    // Draw symbol on hover
    if (is_hovered) {
        switch (type) {
            case 0: // X symbol
                for (int i = -3; i <= 3; i++) {
                    api->draw_rect(cx + i - 1, cy + i - 1, 2, 2, C_SYMBOL_CLOSE);
                    api->draw_rect(cx - i - 1, cy + i - 1, 2, 2, C_SYMBOL_CLOSE);
                }
                break;
            case 1: // - symbol
                api->draw_rect(cx - 4, cy - 1, 8, 2, C_SYMBOL_MIN);
                break;
            case 2: // + symbol (or chevron for maximize)
                api->draw_rect(cx - 3, cy - 1, 6, 2, C_SYMBOL_MAX);
                api->draw_rect(cx - 1, cy - 3, 2, 6, C_SYMBOL_MAX);
                break;
        }
    }
}

// ============================================================================
// ENHANCED WINDOW FRAME
// ============================================================================

void ui_draw_window_frame_ex(kernel_api_t* api, int x, int y, int w, int h, 
                            const char* title, int active, int mx, int my) {
    if (!api) return;
    
    // 1. LAYERED SHADOWS - Creates depth
    if (active) {
        // Outer shadow (soft)
        api->draw_rect_rounded(x + 6, y + 10, w, h, C_SHADOW_SOFT, 18);
        // Middle shadow
        api->draw_rect_rounded(x + 4, y + 6, w, h, C_SHADOW_MEDIUM, 16);
        // Inner shadow (sharp)
        api->draw_rect_rounded(x + 2, y + 3, w, h, C_SHADOW_STRONG, 14);
    } else {
        // Subtle shadow for inactive
        api->draw_rect_rounded(x + 3, y + 5, w, h, C_SHADOW_SOFT, 14);
    }
    
    // 2. WINDOW BACKGROUND with subtle gradient effect
    uint32_t bg_col = active ? C_WIN_BG : 0xFFEBEBEB;
    api->draw_rect_rounded(x, y, w, h, bg_col, 12);
    
    // 3. HEADER with gradient
    int header_h = 38;
    if (active) {
        // Animated glow for active window header
        int glow = (int)(4.0f * (1.0f + 0.3f * fast_sin(window_glow_phase)));
        for (int row = 0; row < header_h; row++) {
            uint32_t intensity = 0xF6 + (row * 2) / header_h;
            uint32_t col = 0xFF000000 | (intensity << 16) | (intensity << 8) | intensity;
            api->draw_rect(x + 2, y + row, w - 4, 1, col);
        }
    } else {
        api->draw_rect(x, y, w, header_h, 0xFFEBEBEB);
    }
    
    // 4. BORDER - Thin elegant border
    api->draw_rect(x, y, w, 1, C_WIN_BORDER);          // Top
    api->draw_rect(x, y + h - 1, w, 1, C_WIN_BORDER);  // Bottom
    api->draw_rect(x, y, 1, h, C_WIN_BORDER);          // Left
    api->draw_rect(x + w - 1, y, 1, h, C_WIN_BORDER);  // Right
    
    // 5. CORNER HIGHLIGHTS
    api->draw_rect(x + 12, y, w - 24, 1, C_WIN_BORDER_LIGHT);
    api->draw_rect(x, y + 12, 1, h - 24, C_WIN_BORDER_LIGHT);
    
    // 6. HEADER SEPARATOR LINE
    api->draw_rect(x, y + header_h - 1, w, 1, C_WIN_BORDER);
    
    // 7. TRAFFIC LIGHTS with hover detection
    int btn_y = y + 19;
    int btn_hover_x = (mx >= x + 12 && mx <= x + 78);
    int btn_hover_y = (my >= y + 10 && my <= y + 28);
    int btn_hover = btn_hover_x && btn_hover_y;
    
    // Calculate which button is hovered
    int close_hover = btn_hover && (mx >= x + 12 && mx <= x + 26);
    int min_hover = btn_hover && (mx >= x + 32 && mx <= x + 46);
    int max_hover = btn_hover && (mx >= x + 52 && mx <= x + 66);
    
    ui_draw_traffic_light(api, x + 20, btn_y, 0, close_hover, active);
    ui_draw_traffic_light(api, x + 38, btn_y, 1, min_hover, active);
    ui_draw_traffic_light(api, x + 56, btn_y, 2, max_hover, active);
    
    // 8. DOCUMENT ICON (optional, for document windows)
    // Draw a small document icon next to traffic lights if needed
    
    // 9. TITLE with shadow for active windows
    if (title) {
        int tlen = api->strlen(title) * 7;
        int tx = x + (w - tlen) / 2;
        uint32_t tcol = active ? 0xFF1D1D1F : 0xFF86868B;
        
        if (active) {
            // Subtle shadow
            api->draw_text(tx + 1, y + 12, title, 0x20000000);
        }
        api->draw_text(tx, y + 11, title, tcol);
    }
    
    // 10. WINDOW RESIZE INDICATOR (bottom-right corner)
    if (active) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j <= i; j++) {
                api->draw_rect(x + w - 14 + j * 4, y + h - 6 - i * 4, 2, 2, C_WIN_BORDER);
            }
        }
    }
}

// ============================================================================
// CONTEXT MENU WITH ANIMATIONS
// ============================================================================

void ui_draw_context_menu(kernel_api_t* api, int x, int y, const char** items, 
                         int count, int hover_idx) {
    if(!api) return;

    int w = 180;
    int h = count * 24 + 8;

    // Bounds check
    if(x + w > 1024) x = 1024 - w;
    if(y + h > 768) y = 768 - h;

    // Multi-layer shadow
    api->draw_rect_rounded(x + 5, y + 5, w, h, 0x30000000, 8);
    api->draw_rect_rounded(x + 3, y + 3, w, h, 0x20000000, 8);

    // Vibrancy background (light blur effect simulation)
    api->draw_rect_rounded(x, y, w, h, 0xFCF8F8F8, 8);
    
    // Border
    api->draw_rect(x, y, w, 1, 0xFFD1D1D6);
    api->draw_rect(x, y+h-1, w, 1, 0xFFD1D1D6);
    api->draw_rect(x, y, 1, h, 0xFFD1D1D6);
    api->draw_rect(x+w-1, y, 1, h, 0xFFD1D1D6);

    // Items
    for(int i=0; i<count; i++) {
        int iy = y + 4 + (i * 24);

        if (api->strcmp(items[i], "-") == 0) {
            // Separator
            api->draw_rect(x + 12, iy + 11, w - 24, 1, 0xFFE5E5EA);
            continue;
        }

        if (i == hover_idx) {
            // Rounded highlight
            api->draw_rect_rounded(x + 4, iy, w - 8, 22, 0xFF007AFF, 5);
            api->draw_text(x + 16, iy + 7, items[i], 0xFFFFFFFF);
        } else {
            api->draw_text(x + 16, iy + 7, items[i], 0xFF1D1D1F);
        }
    }
}

// ============================================================================
// DRAWING HELPERS FOR NEW COMPONENTS
// ============================================================================

void ui_draw_status_badge(kernel_api_t* api, int x, int y, const char* text, 
                          uint32_t bg_color, int active) {
    int w = api->strlen(text) * 8 + 16;
    int h = 20;
    
    // Shadow
    api->draw_rect_rounded(x + 1, y + 1, w, h, 0x20000000, 10);
    
    // Background
    uint32_t col = active ? bg_color : 0xFFE5E5EA;
    api->draw_rect_rounded(x, y, w, h, col, 10);
    
    // Text
    uint32_t text_col = active ? 0xFFFFFFFF : 0xFF8E8E93;
    api->draw_text(x + 8, y + 5, text, text_col);
}

void ui_draw_progress_ring(kernel_api_t* api, int cx, int cy, int r, 
                          int progress, uint32_t color) {
    // Draw circular progress indicator
    // Background circle
    for (int angle = 0; angle < 360; angle++) {
        float rad = angle * 3.14159f / 180.0f;
        int px = cx + (int)(r * fast_cos(rad));
        int py = cy + (int)(r * fast_sin(rad));
        api->draw_rect(px, py, 2, 2, 0xFFE5E5EA);
    }
    
    // Progress arc
    int progress_angle = (progress * 360) / 100;
    for (int angle = 0; angle < progress_angle; angle++) {
        float rad = angle * 3.14159f / 180.0f;
        int px = cx + (int)(r * fast_cos(rad - 1.57f));
        int py = cy + (int)(r * fast_sin(rad - 1.57f));
        api->draw_rect(px, py, 3, 3, color);
    }
}

void ui_draw_shimmer_effect(kernel_api_t* api, int x, int y, int w, int h) {
    // Draw animated shimmer/loading effect
    int offset = (animation_tick * 2) % (w + 40);
    
    for (int i = 0; i < 20; i++) {
        int pos = offset - 20 + i;
        if (pos >= 0 && pos < w) {
            uint32_t alpha = 0x10 + (i < 10 ? i * 8 : (20 - i) * 8);
            for (int row = 0; row < h; row++) {
                api->draw_rect(x + pos, y + row, 1, 1, alpha << 24);
            }
        }
    }
}
