#ifndef MOUSE_H
#define MOUSE_H

extern int mouse_x;
extern int mouse_y;
extern int mouse_btn_left;
extern int mouse_btn_right;

void init_mouse(void);
void mouse_handler(void);

#endif
