/**
 * CamelOS CoreAnimation Implementation
 *
 * CoreAnimation-style animation system for the CamelOS kernel.
 * Uses static object pools for freestanding allocation (no heap needed).
 * Supports basic float interpolation and transition effects with
 * pluggable timing functions.
 */

#include "core_animation.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"

/* Static pools for animation objects */
static CABasicAnimation basic_pool[CA_MAX_ANIMATIONS];
static CATransition     transition_pool[CA_MAX_ANIMATIONS];

/**
 * Create a basic animation
 *
 * Finds a free slot in the basic_pool and initializes it.
 * Returns NULL if the pool is full.
 */
CABasicAnimation* ca_basic_animation_create(float from_val, float to_val, float duration) {
    for (int i = 0; i < CA_MAX_ANIMATIONS; i++) {
        if (basic_pool[i].active == 0) {
            CABasicAnimation* anim = &basic_pool[i];
            anim->type           = CA_ANIM_BASIC;
            anim->from_value     = from_val;
            anim->to_value       = to_val;
            anim->duration       = duration;
            anim->elapsed        = 0.0f;
            anim->timing_func    = CA_TIMING_LINEAR;
            anim->autoreverses   = 0;
            anim->repeat_count   = 1;
            anim->completed_count = 0;
            anim->current_value  = from_val;
            anim->active         = 0;  /* not started yet */
            anim->on_complete    = (void(*)(void*))0;
            anim->user_data      = (void*)0;
            return anim;
        }
    }
    s_printf("[CA] basic_pool full, cannot create animation\n");
    return (CABasicAnimation*)0;
}

/**
 * Create a transition animation
 *
 * Finds a free slot in the transition_pool and initializes it.
 * Returns NULL if the pool is full.
 */
CATransition* ca_transition_create(int transition_type, float duration) {
    for (int i = 0; i < CA_MAX_ANIMATIONS; i++) {
        if (transition_pool[i].active == 0) {
            CATransition* trans = &transition_pool[i];
            trans->type            = CA_ANIM_TRANSITION;
            trans->transition_type = transition_type;
            trans->duration        = duration;
            trans->elapsed         = 0.0f;
            trans->timing_func     = CA_TIMING_EASE_IN_OUT;
            trans->progress        = 0.0f;
            trans->active          = 0;  /* not started yet */
            trans->on_complete     = (void(*)(void*))0;
            trans->user_data       = (void*)0;
            return trans;
        }
    }
    s_printf("[CA] transition_pool full, cannot create transition\n");
    return (CATransition*)0;
}

/**
 * Start a basic animation
 */
void ca_animation_start(CABasicAnimation* anim) {
    if (!anim) return;
    anim->active = 1;
}

/**
 * Start a transition
 */
void ca_transition_start(CATransition* trans) {
    if (!trans) return;
    trans->active = 1;
}

/**
 * Evaluate a timing function at parameter t (0.0 to 1.0)
 *
 * LINEAR:     t
 * EASE_IN:    t^2  (quadratic ease-in)
 * EASE_OUT:   t*(2-t)  (quadratic ease-out)
 * EASE_IN_OUT: quadratic ease-in-out
 */
float ca_timing_evaluate(int timing_func, float t) {
    switch (timing_func) {
        case CA_TIMING_EASE_IN:
            return t * t;
        case CA_TIMING_EASE_OUT:
            return t * (2.0f - t);
        case CA_TIMING_EASE_IN_OUT:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        case CA_TIMING_LINEAR:
        default:
            return t;
    }
}

/**
 * Process all active animations for one render tick
 *
 * delta_ms: elapsed time in milliseconds since last tick
 *
 * For basic animations:
 *   - Accumulates elapsed time
 *   - On completion, handles autoreverses and repeat logic
 *   - Computes current_value using the timing function
 *
 * For transitions:
 *   - Accumulates elapsed time
 *   - Computes progress (0.0 to 1.0) with timing function applied
 *   - Fires on_complete when progress reaches 1.0
 */
void ca_render_tick(uint32_t delta_ms) {
    float dt = (float)delta_ms / 1000.0f;

    /* Process basic animations */
    for (int i = 0; i < CA_MAX_ANIMATIONS; i++) {
        CABasicAnimation* anim = &basic_pool[i];
        if (!anim->active) continue;

        anim->elapsed += dt;

        if (anim->elapsed >= anim->duration) {
            /* Animation cycle complete */
            if (anim->autoreverses && anim->completed_count < anim->repeat_count * 2) {
                /* Swap from and to values for reverse pass */
                float tmp      = anim->from_value;
                anim->from_value = anim->to_value;
                anim->to_value   = tmp;
                anim->elapsed    = 0.0f;
                anim->completed_count++;
            } else if (!anim->autoreverses && anim->repeat_count != 0 &&
                       anim->completed_count < anim->repeat_count) {
                /* Reset for next repeat cycle */
                anim->elapsed = 0.0f;
                anim->completed_count++;
            } else {
                /* Animation fully complete */
                anim->current_value = anim->to_value;
                anim->active        = 0;
                if (anim->on_complete) {
                    anim->on_complete(anim->user_data);
                }
            }
        } else {
            /* Interpolate current value */
            float t       = anim->elapsed / anim->duration;
            float eased_t = ca_timing_evaluate(anim->timing_func, t);

            /* If autoreverses and on an odd cycle, reverse direction */
            if (anim->autoreverses && (anim->completed_count % 2 == 1)) {
                anim->current_value = anim->to_value + (anim->from_value - anim->to_value) * eased_t;
            } else {
                anim->current_value = anim->from_value + (anim->to_value - anim->from_value) * eased_t;
            }
        }
    }

    /* Process transitions */
    for (int i = 0; i < CA_MAX_ANIMATIONS; i++) {
        CATransition* trans = &transition_pool[i];
        if (!trans->active) continue;

        trans->elapsed += dt;

        float raw_progress = trans->elapsed / trans->duration;
        if (raw_progress >= 1.0f) {
            trans->progress = 1.0f;
            trans->active   = 0;
            if (trans->on_complete) {
                trans->on_complete(trans->user_data);
            }
        } else {
            trans->progress = ca_timing_evaluate(trans->timing_func, raw_progress);
        }
    }
}

/**
 * Remove a basic animation from the pool
 *
 * Sets active to 0 and zeroes out the struct.
 */
void ca_animation_remove(CABasicAnimation* anim) {
    if (!anim) return;
    anim->active = 0;
    memset(anim, 0, sizeof(CABasicAnimation));
}

/**
 * Remove a transition from the pool
 *
 * Sets active to 0 and zeroes out the struct.
 */
void ca_transition_remove(CATransition* trans) {
    if (!trans) return;
    trans->active = 0;
    memset(trans, 0, sizeof(CATransition));
}
