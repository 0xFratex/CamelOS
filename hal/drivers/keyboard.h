#ifndef KEYBOARD_H
#define KEYBOARD_H

// Keyboard layout constants
#define KBD_LAYOUT_US      0
#define KBD_LAYOUT_UK      1
#define KBD_LAYOUT_GERMAN  2
#define KBD_LAYOUT_FRENCH  3
#define KBD_LAYOUT_SPANISH 4
#define KBD_LAYOUT_ITALIAN 5
#define KBD_LAYOUT_PORTBR  6
#define KBD_LAYOUT_DVORAK  7
#define KBD_LAYOUT_JAPANESE 8
#define KBD_LAYOUT_KOREAN  9
#define KBD_LAYOUT_CHINESE 10
#define KBD_LAYOUT_COUNT   11

void init_keyboard();
void kbd_set_layout(int layout_id);
const char* kbd_layout_name(int layout_id);
int kbd_find_layout_by_name(const char* name);

extern int kbd_shift_pressed;
extern int kbd_ctrl_pressed;
extern int kbd_alt_pressed;
extern int kbd_layout;
extern const char scancode_std[];
extern const char scancode_shift[];

#endif
