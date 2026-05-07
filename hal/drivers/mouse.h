#ifndef MOUSE_H
#define MOUSE_H

extern int mouse_x;
extern int mouse_y;
extern int mouse_btn_left;
extern int mouse_btn_right;
extern int mouse_btn_middle;
extern int mouse_scroll_delta;   // Scroll wheel: positive = up, negative = down

void init_mouse(void);
void mouse_handler(void);         // IRQ12 handler — pushes bytes into ring buffer
void mouse_process(void);         // Main-loop processing — assembles packets, updates state
void mouse_poll_fallback(void);   // Legacy alias for mouse_process() (called from main loop)

#endif
