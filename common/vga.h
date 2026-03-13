// common/vga.h
#ifndef VGA_H
#define VGA_H

#include "ports.h"

// Manual type definitions for bare-metal environment
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

#define VGA_ADDRESS 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

enum Colors {
    BLACK = 0, BLUE = 1, GREEN = 2, CYAN = 3, RED = 4, MAGENTA = 5, BROWN = 6, LIGHT_GREY = 7,
    DARK_GREY = 8, LIGHT_BLUE = 9, LIGHT_GREEN = 10, LIGHT_CYAN = 11, LIGHT_RED = 12, LIGHT_MAGENTA = 13, YELLOW = 14, WHITE = 15,
};

// Global variables
extern uint16_t* vga_buffer;
extern int term_col;
extern int term_row;
extern uint8_t term_color;

// Function declarations
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_update_cursor(int x, int y);
void vga_scroll();
void vga_clear();
void vga_print_char(char c);
void vga_print(const char* str);
void vga_print_int(int n);

#endif
