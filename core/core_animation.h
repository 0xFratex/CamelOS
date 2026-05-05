#ifndef CORE_ANIMATION_H
#define CORE_ANIMATION_H

#include "../include/types.h"

/* Animation type enums */
#define CA_ANIM_BASIC      0
#define CA_ANIM_TRANSITION 1

/* Timing function enums */
#define CA_TIMING_LINEAR     0
#define CA_TIMING_EASE_IN    1
#define CA_TIMING_EASE_OUT   2
#define CA_TIMING_EASE_IN_OUT 3

/* Transition type enums */
#define CA_TRANSITION_FADE  0
#define CA_TRANSITION_SLIDE 1
#define CA_TRANSITION_GENIE 2

/* Basic animation: interpolates a float from -> to over duration */
typedef struct {
    int   type;            /* CA_ANIM_BASIC */
    float from_value;
    float to_value;
    float duration;        /* seconds */
    float elapsed;
    int   timing_func;
    int   autoreverses;    /* 0 = no, 1 = yes */
    int   repeat_count;    /* 0 = infinite, N = repeat N times, 1 = no repeat */
    int   completed_count; /* how many cycles completed so far */
    float current_value;   /* computed each tick */
    int   active;          /* 1 = running, 0 = completed/removed */
    void  (*on_complete)(void* user_data);
    void* user_data;
} CABasicAnimation;

/* Transition animation: progress-based effect (fade, slide, genie) */
typedef struct {
    int   type;            /* CA_ANIM_TRANSITION */
    int   transition_type; /* CA_TRANSITION_* */
    float duration;
    float elapsed;
    int   timing_func;
    float progress;        /* 0.0 to 1.0 */
    int   active;          /* 1 = running, 0 = completed/removed */
    void  (*on_complete)(void* user_data);
    void* user_data;
} CATransition;

/* Maximum concurrent animations of each type */
#define CA_MAX_ANIMATIONS 32

/* Create a basic animation */
CABasicAnimation* ca_basic_animation_create(float from_val, float to_val, float duration);

/* Create a transition animation */
CATransition* ca_transition_create(int transition_type, float duration);

/* Start an animation (sets active = 1) */
void ca_animation_start(CABasicAnimation* anim);

/* Start a transition (sets active = 1) */
void ca_transition_start(CATransition* trans);

/* Process all active animations for one tick (delta_ms in milliseconds) */
void ca_render_tick(uint32_t delta_ms);

/* Evaluate a timing function at t (0.0 to 1.0) */
float ca_timing_evaluate(int timing_func, float t);

/* Remove a basic animation from the pool */
void ca_animation_remove(CABasicAnimation* anim);

/* Remove a transition from the pool */
void ca_transition_remove(CATransition* trans);

#endif
