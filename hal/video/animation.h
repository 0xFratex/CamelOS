#ifndef ANIMATION_H
#define ANIMATION_H

typedef struct {
    int x, y, w, h;
} rect_t;

// Fixed-point versions (16.16 format)
int math_lerp(int start, int end, int t);
int anim_ease_in_out_quad(int t);
int anim_ease_out_back(int t);
void anim_genie_calc(rect_t src, rect_t dest, int t, rect_t* out);

#endif