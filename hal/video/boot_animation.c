// hal/video/boot_animation.c - Boot Loading Animation System
// Displays progress based on actual system load status

#include "boot_animation.h"
#include "gfx_hal.h"
#include "../../core/string.h"
#include "../../kernel/assets.h"

// External screen dimensions
extern int screen_w;
extern int screen_h;

// Animation state
static BootAnimationState g_boot_state;

// Design constants
#define C_BG_TOP          0xFF1A1A2E
#define C_BG_BOTTOM       0xFF0F0F1A
#define C_LOGO_COLOR      0xFF007AFF
#define C_TEXT            0xFFFFFFFF
#define C_TEXT_DIM        0xFF8E8E93
#define C_PROGRESS_BG     0xFF2C2C3E
#define C_PROGRESS_FILL   0xFF007AFF

// Camel logo pixels (simplified)
static const uint8_t camel_logo[32][32] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,1,1,2,2,2,2,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,1,2,2,2,0,0,0,0,0,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,2,2,2,0,0,0,0,0,0,0,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,2,2,2,0,0,0,0,0,0,0,0,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,2,2,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,1,0,0,0,0,0,0},
    {0,0,0,0,0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,1,0,0,0,0},
    {0,0,0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,1,0,0},
    {0,0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,1,0,0},
    {0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,1,0},
    {0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,1,0,0},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,1,0,0,0},
    {0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,1,0,0,0,0},
    {0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0},
    {0,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0},
    {0,0,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0},
    {0,0,0,0,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0}
};

// Progress step names
static const char* boot_step_names[] = {
    "Initializing Hardware",
    "Loading Kernel",
    "Setting up Memory",
    "Initializing Filesystem",
    "Loading Drivers",
    "Starting Services",
    "Loading Applications",
    "Starting Desktop"
};

// ============================================================================
// INITIALIZATION
// ============================================================================

void boot_animation_init(void) {
    memset(&g_boot_state, 0, sizeof(g_boot_state));
    g_boot_state.current_step = 0;
    g_boot_state.total_steps = 8;
    g_boot_state.overall_progress = 0;
    g_boot_state.is_complete = 0;
    g_boot_state.animation_tick = 0;
    g_boot_state.logo_scale = 0.5f;
    g_boot_state.show_progress = 1;
    g_boot_state.spinner_angle = 0;
    
    // Initialize individual step progress
    for (int i = 0; i < 8; i++) {
        g_boot_state.step_progress[i] = 0;
        g_boot_state.step_complete[i] = 0;
    }
}

// ============================================================================
// PROGRESS UPDATES
// ============================================================================

void boot_animation_set_step(int step) {
    if (step >= 0 && step < g_boot_state.total_steps) {
        // Mark previous step as complete
        if (g_boot_state.current_step < step) {
            for (int i = g_boot_state.current_step; i < step; i++) {
                g_boot_state.step_complete[i] = 1;
                g_boot_state.step_progress[i] = 100;
            }
        }
        g_boot_state.current_step = step;
    }
}

void boot_animation_set_step_progress(int step, int progress) {
    if (step >= 0 && step < g_boot_state.total_steps) {
        g_boot_state.step_progress[step] = progress;
        if (progress >= 100) {
            g_boot_state.step_complete[step] = 1;
        }
        boot_animation_update_overall();
    }
}

void boot_animation_update_overall(void) {
    int total = 0;
    for (int i = 0; i < g_boot_state.total_steps; i++) {
        total += g_boot_state.step_progress[i];
    }
    g_boot_state.overall_progress = total / g_boot_state.total_steps;
}

void boot_animation_complete_step(int step) {
    boot_animation_set_step_progress(step, 100);
}

void boot_animation_finish(void) {
    for (int i = 0; i < g_boot_state.total_steps; i++) {
        g_boot_state.step_complete[i] = 1;
        g_boot_state.step_progress[i] = 100;
    }
    g_boot_state.overall_progress = 100;
    g_boot_state.is_complete = 1;
}

// ============================================================================
// RENDERING
// ============================================================================

static void draw_gradient_bg(int w, int h) {
    for (int y = 0; y < h; y++) {
        uint8_t blend = (y * 255) / h;
        uint8_t r1 = (C_BG_TOP >> 16) & 0xFF;
        uint8_t g1 = (C_BG_TOP >> 8) & 0xFF;
        uint8_t b1 = C_BG_TOP & 0xFF;
        uint8_t r2 = (C_BG_BOTTOM >> 16) & 0xFF;
        uint8_t g2 = (C_BG_BOTTOM >> 8) & 0xFF;
        uint8_t b2 = C_BG_BOTTOM & 0xFF;
        
        uint8_t r = r1 + ((r2 - r1) * blend) / 255;
        uint8_t g = g1 + ((g2 - g1) * blend) / 255;
        uint8_t b = b1 + ((b2 - b1) * blend) / 255;
        
        gfx_fill_rect(0, y, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }
}

static void draw_logo(int cx, int cy, float scale) {
    int size = (int)(32 * scale);
    int start_x = cx - size / 2;
    int start_y = cy - size / 2;
    
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            if (camel_logo[y][x] == 1) {
                // Border
                int px = start_x + (x * size) / 32;
                int py = start_y + (y * size) / 32;
                int pw = (size + 31) / 32;
                gfx_fill_rect(px, py, pw, pw, C_LOGO_COLOR);
            } else if (camel_logo[y][x] == 2) {
                // Fill
                int px = start_x + (x * size) / 32;
                int py = start_y + (y * size) / 32;
                int pw = (size + 31) / 32;
                gfx_fill_rect(px, py, pw, pw, C_LOGO_COLOR);
            }
        }
    }
}

static void draw_spinner(int cx, int cy, int radius, int angle) {
    // Draw animated spinner
    for (int i = 0; i < 8; i++) {
        int a = angle + i * 45;
        int alpha = 255 - i * 30;
        
        float rad = a * 3.14159f / 180.0f;
        int px = cx + (int)(radius * cos(rad));
        int py = cy + (int)(radius * sin(rad));
        
        uint32_t color = (alpha << 24) | (C_LOGO_COLOR & 0x00FFFFFF);
        gfx_fill_rounded_rect(px - 3, py - 3, 6, 6, color, 3);
    }
}

static void draw_progress_bar(int x, int y, int w, int h, int progress) {
    // Background
    gfx_fill_rounded_rect(x, y, w, h, C_PROGRESS_BG, h/2);
    
    // Fill
    int fill_w = (w * progress) / 100;
    if (fill_w > 0) {
        gfx_fill_rounded_rect(x, y, fill_w, h, C_PROGRESS_FILL, h/2);
    }
    
    // Shimmer effect
    static int shimmer_offset = 0;
    shimmer_offset = (shimmer_offset + 2) % (w + 40);
    
    int shimmer_x = shimmer_offset - 20;
    if (shimmer_x >= 0 && shimmer_x < fill_w) {
        uint32_t shimmer_color = 0x40FFFFFF;
        gfx_fill_rect(x + shimmer_x, y, 4, h, shimmer_color);
    }
}

void boot_animation_render(void) {
    int w = screen_w ? screen_w : 1024;
    int h = screen_h ? screen_h : 768;
    int cx = w / 2;
    int cy = h / 2;
    
    // Background
    draw_gradient_bg(w, h);
    
    // Animated logo
    g_boot_state.logo_scale = 0.5f + 0.05f * sin(g_boot_state.animation_tick * 0.02f);
    draw_logo(cx, cy - 80, g_boot_state.logo_scale * 3);
    
    // Title
    gfx_draw_string_scaled(cx - 80, cy + 40, "Camel OS", C_TEXT, 3);
    
    // Version/tagline
    gfx_draw_string(cx - 80, cy + 90, "Loading your experience...", C_TEXT_DIM);
    
    // Progress bar
    int bar_w = 300;
    int bar_h = 6;
    int bar_x = cx - bar_w / 2;
    int bar_y = cy + 130;
    
    draw_progress_bar(bar_x, bar_y, bar_w, bar_h, g_boot_state.overall_progress);
    
    // Current step text
    const char* step_name = boot_step_names[g_boot_state.current_step];
    gfx_draw_string(cx - strlen(step_name) * 3, bar_y + 20, step_name, C_TEXT_DIM);
    
    // Spinner
    g_boot_state.spinner_angle = (g_boot_state.spinner_angle + 5) % 360;
    draw_spinner(cx, bar_y + 60, 20, g_boot_state.spinner_angle);
    
    // Step progress indicators
    int indicator_y = h - 80;
    int indicator_spacing = 40;
    int start_x = cx - (g_boot_state.total_steps * indicator_spacing) / 2;
    
    for (int i = 0; i < g_boot_state.total_steps; i++) {
        int ix = start_x + i * indicator_spacing;
        
        uint32_t color;
        if (g_boot_state.step_complete[i]) {
            color = 0xFF34C759;  // Green - complete
        } else if (i == g_boot_state.current_step) {
            // Pulsing current step
            int pulse = (int)(128 + 127 * sin(g_boot_state.animation_tick * 0.1f));
            color = 0xFF000000 | (pulse << 16) | 0x7AFF;
        } else {
            color = 0xFF3A3A4A;  // Pending
        }
        
        gfx_fill_rounded_rect(ix - 6, indicator_y - 6, 12, 12, color, 6);
    }
    
    // Percentage
    char pct[8];
    int_to_str(g_boot_state.overall_progress, pct);
    strcat(pct, "%");
    gfx_draw_string(cx - strlen(pct) * 4, bar_y - 25, pct, C_TEXT);
    
    // Animation tick
    g_boot_state.animation_tick++;
}

// ============================================================================
// CHECK STATUS
// ============================================================================

int boot_animation_is_complete(void) {
    return g_boot_state.is_complete;
}

int boot_animation_get_progress(void) {
    return g_boot_state.overall_progress;
}
