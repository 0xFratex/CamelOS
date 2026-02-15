// hal/video/loading_animation.h - Loading Animation System for CamelOS
#ifndef LOADING_ANIMATION_H
#define LOADING_ANIMATION_H

#include "../../include/types.h"

// Animation types
typedef enum {
    ANIM_SPINNER,       // Classic spinning circle
    ANIM_DOTS,          // Bouncing dots
    ANIM_PROGRESS,      // Progress bar
    ANIM_PULSE          // Pulsing circle
} loading_anim_type_t;

// Animation state
typedef struct {
    int active;
    int x, y;
    int width, height;
    loading_anim_type_t type;
    uint32_t color;
    uint32_t bg_color;
    int frame;
    int max_frames;
    int speed;
    uint32_t last_update;
    
    // For progress animation
    int progress;
    int max_progress;
    
    // Text to display
    char text[64];
    int show_text;
} loading_animation_t;

// Initialize the animation system
void loading_anim_init(void);

// Create a loading animation
loading_animation_t* loading_anim_create(int x, int y, int w, int h, 
                                          loading_anim_type_t type, uint32_t color);

// Destroy a loading animation
void loading_anim_destroy(loading_animation_t* anim);

// Update animation frame (call this in your main loop)
void loading_anim_update(loading_animation_t* anim);

// Draw the animation (call this in your paint callback)
void loading_anim_draw(loading_animation_t* anim);

// Set animation text
void loading_anim_set_text(loading_animation_t* anim, const char* text);

// Set progress (for progress bar type)
void loading_anim_set_progress(loading_animation_t* anim, int progress, int max);

// Start/stop animation
void loading_anim_start(loading_animation_t* anim);
void loading_anim_stop(loading_animation_t* anim);

// Check if animation is active
int loading_anim_is_active(loading_animation_t* anim);

// Draw a spinner at position
void draw_spinner(int x, int y, int radius, uint32_t color, int frame);

// Draw bouncing dots
void draw_bouncing_dots(int x, int y, int count, uint32_t color, int frame);

// Draw progress bar
void draw_progress_bar(int x, int y, int w, int h, int progress, int max,
                       uint32_t fg_color, uint32_t bg_color);

// Draw pulsing circle
void draw_pulse(int x, int y, int radius, uint32_t color, int frame);

#endif // LOADING_ANIMATION_H
