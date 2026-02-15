// hal/video/loading_animation.c - Loading Animation Implementation
#include "loading_animation.h"
#include "gfx_hal.h"
#include "../../core/timer.h"
#include "../../core/string.h"

// Maximum concurrent animations
#define MAX_ANIMATIONS 8

// Animation storage
static loading_animation_t animations[MAX_ANIMATIONS];
static int anim_count = 0;

// Math helper for sine approximation (no floating point)
static int sin_approx(int angle) {
    // Simple sine approximation using lookup-like calculation
    // angle is 0-255 representing 0-360 degrees
    angle = angle & 0xFF;  // Keep in range
    
    // Simple triangular wave approximation
    if (angle < 64) {
        return angle * 256 / 64;  // 0 to 255
    } else if (angle < 128) {
        return 256 - ((angle - 64) * 256 / 64);  // 255 to 0
    } else if (angle < 192) {
        return -((angle - 128) * 256 / 64);  // 0 to -255
    } else {
        return -256 + ((angle - 192) * 256 / 64);  // -255 to 0
    }
}

// Initialize animation system
void loading_anim_init(void) {
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        animations[i].active = 0;
    }
    anim_count = 0;
}

// Create a loading animation
loading_animation_t* loading_anim_create(int x, int y, int w, int h,
                                          loading_anim_type_t type, uint32_t color) {
    // Find free slot
    for (int i = 0; i < MAX_ANIMATIONS; i++) {
        if (!animations[i].active) {
            loading_animation_t* anim = &animations[i];
            anim->active = 1;
            anim->x = x;
            anim->y = y;
            anim->width = w;
            anim->height = h;
            anim->type = type;
            anim->color = color;
            anim->bg_color = 0xFFFFFFFF;  // White background
            anim->frame = 0;
            anim->max_frames = (type == ANIM_SPINNER) ? 12 : 
                              (type == ANIM_DOTS) ? 24 : 100;
            anim->speed = 50;  // ms per frame
            anim->last_update = 0;
            anim->progress = 0;
            anim->max_progress = 100;
            anim->text[0] = 0;
            anim->show_text = 0;
            anim_count++;
            return anim;
        }
    }
    return 0;  // No free slot
}

// Destroy animation
void loading_anim_destroy(loading_animation_t* anim) {
    if (anim) {
        anim->active = 0;
        anim_count--;
    }
}

// Update animation frame
void loading_anim_update(loading_animation_t* anim) {
    if (!anim || !anim->active) return;
    
    uint32_t now = timer_get_ticks();
    uint32_t elapsed = now - anim->last_update;
    
    if (elapsed >= anim->speed) {
        anim->frame = (anim->frame + 1) % anim->max_frames;
        anim->last_update = now;
    }
}

// Draw a spinner
void draw_spinner(int x, int y, int radius, uint32_t color, int frame) {
    // Draw spinning dots
    int num_dots = 8;
    for (int i = 0; i < num_dots; i++) {
        int angle = (i * 256 / num_dots + frame * 21) & 0xFF;
        
        // Calculate position
        int sin_val = sin_approx(angle);
        int cos_val = sin_approx((angle + 64) & 0xFF);
        
        int dot_x = x + (cos_val * radius / 256);
        int dot_y = y + (sin_val * radius / 256);
        
        // Vary alpha/size based on position in spin
        int dot_size = 2 + (i * 2 / num_dots);
        
        // Fade effect - dots closer to "tail" are dimmer
        uint8_t alpha = 255 - (i * 255 / num_dots);
        uint32_t dot_color = (color & 0xFF000000) | 
                            (((color >> 16) & 0xFF) * alpha / 255) << 16 |
                            (((color >> 8) & 0xFF) * alpha / 255) << 8 |
                            ((color & 0xFF) * alpha / 255);
        
        gfx_fill_rect(dot_x - dot_size/2, dot_y - dot_size/2, dot_size, dot_size, dot_color);
    }
}

// Draw bouncing dots
void draw_bouncing_dots(int x, int y, int count, uint32_t color, int frame) {
    int dot_radius = 4;
    int spacing = 16;
    int start_x = x - (count * spacing) / 2;
    
    for (int i = 0; i < count; i++) {
        // Calculate bounce height for each dot with phase offset
        int phase = (frame + i * 8) % 24;
        int bounce;
        
        if (phase < 12) {
            bounce = phase * 8;  // Going up
        } else {
            bounce = (24 - phase) * 8;  // Coming down
        }
        
        int dot_x = start_x + i * spacing;
        int dot_y = y - bounce;
        
        // Draw dot with shadow
        gfx_fill_rect(dot_x - dot_radius - 1, y + 2, dot_radius * 2 + 2, 2, 0x40000000);
        gfx_fill_rect(dot_x - dot_radius, dot_y - dot_radius, 
                     dot_radius * 2, dot_radius * 2, color);
    }
}

// Draw progress bar
void draw_progress_bar(int x, int y, int w, int h, int progress, int max,
                       uint32_t fg_color, uint32_t bg_color) {
    // Draw background
    gfx_fill_rect(x, y, w, h, bg_color);
    
    // Draw border
    gfx_draw_rect(x, y, w, h, 0xFF888888);
    
    // Draw progress
    if (max > 0) {
        int fill_width = (w - 4) * progress / max;
        if (fill_width > 0) {
            gfx_fill_rect(x + 2, y + 2, fill_width, h - 4, fg_color);
        }
    }
}

// Draw pulsing circle
void draw_pulse(int x, int y, int radius, uint32_t color, int frame) {
    // Pulse between 0.5 and 1.0 scale
    int pulse = sin_approx(frame * 16);
    int scale = 128 + pulse / 4;  // 0.5 to 1.5 scale
    
    int r = radius * scale / 256;
    
    // Draw concentric circles with fading alpha
    for (int i = 3; i >= 0; i--) {
        int ring_r = r - i * 4;
        if (ring_r > 0) {
            uint8_t alpha = 255 - i * 60;
            uint32_t ring_color = 0xFF000000 | 
                                 ((((color >> 16) & 0xFF) * alpha) / 255) << 16 |
                                 ((((color >> 8) & 0xFF) * alpha) / 255) << 8 |
                                 (((color & 0xFF) * alpha) / 255);
            
            // Draw circle as series of lines
            for (int angle = 0; angle < 360; angle += 10) {
                int x1 = x + ring_r * sin_approx(angle * 256 / 360) / 256;
                int y1 = y + ring_r * sin_approx((angle + 64) * 256 / 360) / 256;
                gfx_put_pixel(x1, y1, ring_color);
            }
        }
    }
}

// Draw animation
void loading_anim_draw(loading_animation_t* anim) {
    if (!anim || !anim->active) return;
    
    switch (anim->type) {
        case ANIM_SPINNER:
            draw_spinner(anim->x + anim->width/2, anim->y + anim->height/2,
                        anim->width/4, anim->color, anim->frame);
            break;
            
        case ANIM_DOTS:
            draw_bouncing_dots(anim->x + anim->width/2, anim->y + anim->height/2,
                              3, anim->color, anim->frame);
            break;
            
        case ANIM_PROGRESS:
            draw_progress_bar(anim->x, anim->y + anim->height/2 - 8,
                            anim->width, 16, anim->progress, anim->max_progress,
                            anim->color, anim->bg_color);
            break;
            
        case ANIM_PULSE:
            draw_pulse(anim->x + anim->width/2, anim->y + anim->height/2,
                      anim->width/4, anim->color, anim->frame);
            break;
    }
    
    // Draw text if set
    if (anim->show_text && anim->text[0]) {
        int text_y = anim->y + anim->height - 20;
        gfx_draw_string(anim->x + 10, text_y, anim->text, 0xFF000000);
    }
}

// Set animation text
void loading_anim_set_text(loading_animation_t* anim, const char* text) {
    if (anim && text) {
        strncpy(anim->text, text, sizeof(anim->text) - 1);
        anim->text[sizeof(anim->text) - 1] = 0;
        anim->show_text = 1;
    }
}

// Set progress
void loading_anim_set_progress(loading_animation_t* anim, int progress, int max) {
    if (anim) {
        anim->progress = progress;
        anim->max_progress = max > 0 ? max : 1;
    }
}

// Start animation
void loading_anim_start(loading_animation_t* anim) {
    if (anim) {
        anim->active = 1;
        anim->frame = 0;
        anim->last_update = timer_get_ticks();
    }
}

// Stop animation
void loading_anim_stop(loading_animation_t* anim) {
    if (anim) {
        anim->active = 0;
    }
}

// Check if animation is active
int loading_anim_is_active(loading_animation_t* anim) {
    return anim ? anim->active : 0;
}
