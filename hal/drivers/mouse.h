#ifndef MOUSE_H
#define MOUSE_H

extern int mouse_x;
extern int mouse_y;
extern int mouse_btn_left;
extern int mouse_btn_right;
extern int mouse_btn_middle;
extern int mouse_scroll_delta;   // Scroll wheel: positive = up, negative = down
extern int mouse_has_wheel;      // 1 if Intellimouse scroll wheel detected

void init_mouse(void);
void mouse_handler(void);

#endif
