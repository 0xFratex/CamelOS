// hal/drivers/vga.h
#ifndef VGA_H
#define VGA_H

#include "../common/ports.h"
#include "../../include/types.h"

enum Colors {
    BLACK = 0,
    BLUE = 1,
    GREEN = 2,
    CYAN = 3,
    RED = 4,
    MAGENTA = 5,
    BROWN = 6,
    LIGHT_GREY = 7,
    DARK_GREY = 8,
    LIGHT_BLUE = 9,
    LIGHT_GREEN = 10,
    LIGHT_CYAN = 11,
    LIGHT_RED = 12,
    LIGHT_MAGENTA = 13,
    YELLOW = 14,
    WHITE = 15,
};

// ============================================================================
// VBE MODE DEFINITIONS
// ============================================================================
#define VBE_MODE_640x480x32    0x145
#define VBE_MODE_800x600x32    0x15C
#define VBE_MODE_1024x768x32   0x168
#define VBE_MODE_1280x720x32   0x16B
#define VBE_MODE_1280x1024x32  0x16A
#define VBE_MODE_1440x900x32   0x17D
#define VBE_MODE_1920x1080x32  0x18B

// ============================================================================
// COLOR DEFINITIONS (32-bit ARGB)
// ============================================================================
#define COLOR_BLACK       0xFF000000
#define COLOR_WHITE       0xFFFFFFFF
#define COLOR_RED         0xFFFF0000
#define COLOR_GREEN       0xFF008000
#define COLOR_BLUE        0xFF0000FF
#define COLOR_CYAN        0xFF00FFFF
#define COLOR_MAGENTA     0xFFFF00FF
#define COLOR_YELLOW      0xFFFFFF00
#define COLOR_GRAY        0xFF808080
#define COLOR_LIGHT_GRAY  0xFFC0C0C0
#define COLOR_DARK_GRAY   0xFF404040
#define COLOR_ORANGE      0xFFFFA500
#define COLOR_PINK        0xFFFFC0CB
#define COLOR_PURPLE      0xFF800080
#define COLOR_BROWN       0xFFA52A2A

// Global Video State
extern uint32_t* gfx_mem; // Pointer to Linear Framebuffer (Video RAM)
extern int screen_w;
extern int screen_h;
extern int screen_pitch;  // Bytes per line
extern int screen_bpp;

// Drawing target management
extern void gfx_set_target(uint32_t* buffer);
extern uint32_t* gfx_get_active_buffer();

void init_vga_multiboot(void* mboot_ptr);
void init_vga_graphics();

// VBE mode setting
int vbe_set_mode(uint16_t mode);
int vbe_get_current_mode(void);
void vbe_list_modes(void);

// New drawing primitives for 32-bit color
void gfx_put_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);

void gfx_draw_char(int x, int y, char c, uint32_t color);
void gfx_draw_string(int x, int y, const char* str, uint32_t color);

// Helper for assets
void gfx_draw_asset_scaled(uint32_t* buffer, int x, int y, const uint32_t* data,
                          int src_w, int src_h, int dest_w, int dest_h);
void gfx_draw_icon(int x, int y, int w, int h, const uint32_t* data);

// Legacy text mode stubs (for panic)
void vga_set_color(unsigned char fg, unsigned char bg);
void vga_print(const char* str);
void vga_clear();

// Add near other declarations
void vga_mute_log(int enable);
uint8_t vga_approx_color(uint32_t rgba);

#endif
