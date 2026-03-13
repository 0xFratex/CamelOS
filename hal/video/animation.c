#include "animation.h"

// Fixed-point math (16.16 format)
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define FP_HALF (1 << (FP_SHIFT-1))

// Convert int to fixed-point
static inline int int_to_fp(int x) {
    return x << FP_SHIFT;
}

// Convert fixed-point to int
static inline int fp_to_int(int x) {
    return x >> FP_SHIFT;
}

// Fixed-point multiplication
static inline int fp_mul(int a, int b) {
    return ((long long)a * b) >> FP_SHIFT;
}

// Fixed-point linear interpolation
int math_lerp(int start, int end, int t) {
    return start + fp_mul(end - start, t);
}

// Ease In-Out Quad (fixed-point version)
int anim_ease_in_out_quad(int t) {
    if (t < FP_HALF) {
        int tt = fp_mul(t, t);
        return fp_mul(2, tt);
    } else {
        t = t - FP_ONE;
        int tt = fp_mul(t, t);
        return FP_ONE - (fp_mul(tt, -2) >> 1);
    }
}

// Ease Out Back (Overshoot effect for windows opening)
int anim_ease_out_back(int t) {
    const int c1 = int_to_fp(170158) >> 8; // 1.70158 in FP
    const int c3 = c1 + FP_ONE;
    t = t - FP_ONE;
    int tt = fp_mul(t, t);
    int ttt = fp_mul(tt, t);
    return FP_ONE + fp_mul(c3, ttt) + fp_mul(c1, tt);
}

// Calculate Genie Effect Rect (fixed-point version)
void anim_genie_calc(rect_t src, rect_t dest, int t, rect_t* out) {
    // Non-linear progress for "sucking" effect
    int lateral_t = fp_mul(t, t); // Move towards dock slower at start
    int vertical_t = t;

    // Interpolate positions
    out->x = fp_to_int(math_lerp(int_to_fp(src.x), int_to_fp(dest.x), lateral_t));
    out->y = fp_to_int(math_lerp(int_to_fp(src.y), int_to_fp(dest.y), vertical_t));

    // Scale width linearly
    out->w = fp_to_int(math_lerp(int_to_fp(src.w), int_to_fp(dest.w), t));

    // Scale height rapidly at the end (squeezing)
    out->h = fp_to_int(math_lerp(int_to_fp(src.h), int_to_fp(dest.h), t));
}