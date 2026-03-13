#ifndef DOCK_H
#define DOCK_H

#include "framework.h" // For Window* and DockIcon definitions

void dock_init();
void dock_register(const char* label, int color, Window* win);
void dock_render(uint32_t* buffer, int screen_w, int screen_h, int mx, int my);
int dock_handle_click(int mx, int my, int screen_w, int screen_h);

// New function to link a window to a dock icon by title matching
void dock_bind_window(Window* win);

// Function to get dock icon rect for Genie animation
void dock_get_window_rect(window_t* win, int* out_x, int* out_y, int* out_w, int* out_h);

#endif