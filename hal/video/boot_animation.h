// hal/video/boot_animation.h - Boot Loading Animation Header
#ifndef BOOT_ANIMATION_H
#define BOOT_ANIMATION_H

#include "../../include/types.h"

#define MAX_BOOT_STEPS 8

typedef struct {
    int current_step;
    int total_steps;
    int overall_progress;
    int step_progress[MAX_BOOT_STEPS];
    int step_complete[MAX_BOOT_STEPS];
    int is_complete;
    uint32_t animation_tick;
    float logo_scale;
    int show_progress;
    int spinner_angle;
} BootAnimationState;

// Initialization
void boot_animation_init(void);

// Progress control
void boot_animation_set_step(int step);
void boot_animation_set_step_progress(int step, int progress);
void boot_animation_complete_step(int step);
void boot_animation_update_overall(void);
void boot_animation_finish(void);

// Rendering
void boot_animation_render(void);

// Status
int boot_animation_is_complete(void);

#endif // BOOT_ANIMATION_H
