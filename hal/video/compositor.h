#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "../../core/window_server.h"

void compositor_draw_shadow(int x, int y, int w, int h, int radius, int active);
void compositor_draw_window(window_t* win);
void compositor_draw_blur_backdrop(int x, int y, int w, int h);

#endif