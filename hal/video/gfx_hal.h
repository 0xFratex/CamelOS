#ifndef GFX_HAL_H
#define GFX_HAL_H

typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t* vram_ptr;
    uint32_t* back_ptr; // Double Buffer
} gfx_context_t;

extern gfx_context_t gfx_ctx;

void gfx_init_hal(void* mboot_ptr);
void gfx_swap_buffers();
void gfx_put_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
uint32_t gfx_blend_color(uint32_t bg, uint32_t fg);
void gfx_draw_char_scaled(int x, int y, char c, uint32_t color, int scale);
void gfx_draw_string_scaled(int x, int y, const char* str, uint32_t color, int scale);
void gfx_draw_string(int x, int y, const char* str, uint32_t color);
void gfx_fill_rounded_rect(int x, int y, int w, int h, uint32_t color, int r);
uint32_t* gfx_get_active_buffer();
void gfx_draw_asset_scaled(uint32_t* buffer, int x, int y, const uint32_t* data, int sw, int sh, int dw, int dh);
void gfx_draw_icon(int x, int y, int w, int h, const uint32_t* data);

#endif