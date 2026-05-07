#ifndef MOUSE_H
#define MOUSE_H

extern int mouse_x;
extern int mouse_y;
extern int mouse_btn_left;
extern int mouse_btn_right;
extern int mouse_btn_middle;
extern int mouse_scroll_delta;   // Scroll wheel: positive = up, negative = down
// Note: mouse_has_wheel is static in mouse.c, not exported

void init_mouse(void);
void mouse_handler(void);
void mouse_poll_fallback(void);  // Polling fallback for VirtualBox (called from main loop)

#endif
