#ifndef KEYBOARD_H
#define KEYBOARD_H
void init_keyboard();
extern int kbd_shift_pressed;
extern int kbd_ctrl_pressed;
extern const char scancode_ascii[];
#endif
