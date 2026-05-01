#ifndef KEYBOARD_H
#define KEYBOARD_H

// Keyboard layout constants
#define KBD_LAYOUT_US         0
#define KBD_LAYOUT_UK        1
#define KBD_LAYOUT_GERMAN    2
#define KBD_LAYOUT_FRENCH    3
#define KBD_LAYOUT_SPANISH   4
#define KBD_LAYOUT_ITALIAN   5
#define KBD_LAYOUT_PORTBR    6
#define KBD_LAYOUT_DVORAK    7
#define KBD_LAYOUT_JAPANESE  8
#define KBD_LAYOUT_KOREAN    9
#define KBD_LAYOUT_CHINESE   10
#define KBD_LAYOUT_SWISS     11
#define KBD_LAYOUT_SWEDISH   12
#define KBD_LAYOUT_NORWEGIAN 13
#define KBD_LAYOUT_DANISH    14
#define KBD_LAYOUT_FINNISH   15
#define KBD_LAYOUT_POLISH    16
#define KBD_LAYOUT_CZECH     17
#define KBD_LAYOUT_HUNGARIAN 18
#define KBD_LAYOUT_ROMANIAN  19
#define KBD_LAYOUT_TURKISH_Q 20
#define KBD_LAYOUT_TURKISH_F 21
#define KBD_LAYOUT_RUSSIAN   22
#define KBD_LAYOUT_ARABIC    23
#define KBD_LAYOUT_HEBREW    24
#define KBD_LAYOUT_THAI      25
#define KBD_LAYOUT_VIETNAMESE 26
#define KBD_LAYOUT_GREEK     27
#define KBD_LAYOUT_CROATIAN  28
#define KBD_LAYOUT_PORTPT    29
#define KBD_LAYOUT_CANADIAN  30
#define KBD_LAYOUT_COUNT     31

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
