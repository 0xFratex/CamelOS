// hal/video/gfx_hal.c
#include "gfx_hal.h"
#include "../../core/memory.h"
#include "../../hal/drivers/vga.h"
#include "../../hal/drivers/serial.h"
#include "../../hal/cpu/paging.h"
#include "../../common/font.h"

gfx_context_t gfx_ctx;
static int use_backbuffer = 0;

// --- GLASS ENGINE STATE ---
static uint32_t* wallpaper_blur_ptr = 0; // Secondary buffer for "frosted" background

void gfx_init_hal(void* mboot_ptr) {
    // 1. Initialize Context
    if (mboot_ptr) init_vga_multiboot(mboot_ptr);
    if (screen_w == 0) { init_vga_graphics(); }
    
    gfx_ctx.width = screen_w; 
    gfx_ctx.height = screen_h;
    gfx_ctx.pitch = screen_pitch; 
    gfx_ctx.bpp = screen_bpp;
    gfx_ctx.vram_ptr = gfx_mem;

    s_printf("[GFX] Init: "); 
    char buf[16]; 
    extern void int_to_str(int, char*);
    int_to_str(gfx_ctx.width, buf); s_printf(buf); s_printf("x");
    int_to_str(gfx_ctx.height, buf); s_printf(buf); s_printf(" VRAM: 0x");
    
    // Manual hex print for debugging
    uint32_t vptr = (uint32_t)gfx_ctx.vram_ptr;
    char* hex = "0123456789ABCDEF";
    for(int i=28; i>=0; i-=4) write_serial(hex[(vptr>>i)&0xF]);
    s_printf("\n");

    // 2. Map Video Memory (CRITICAL FIX)
    // We unconditionally map the framebuffer if we are in high-res mode.
    // This prevents Page Faults when writing to 0xFD000000+
    if (gfx_ctx.vram_ptr && gfx_ctx.bpp >= 24) {
        uint32_t fb_size = gfx_ctx.height * gfx_ctx.pitch;
        // Align up to 4KB
        if (fb_size % 4096) fb_size += 4096 - (fb_size % 4096);
        
        paging_map_region((uint32_t)gfx_ctx.vram_ptr, (uint32_t)gfx_ctx.vram_ptr, fb_size, 0x03); // Present | RW
        s_printf("[GFX] VRAM Mapped.\n");
    }

    // 3. Allocate Backbuffer (CRITICAL FIX: NULL CHECK)
    uint32_t size = gfx_ctx.width * gfx_ctx.height * 4;
    gfx_ctx.back_ptr = (uint32_t*)kmalloc(size);
    
    if (gfx_ctx.back_ptr) { 
        use_backbuffer = 1; 
        memset(gfx_ctx.back_ptr, 0, size); 
        s_printf("[GFX] Backbuffer Allocated.\n");
    } else {
        use_backbuffer = 0;
        s_printf("[GFX] WARNING: Backbuffer alloc failed! Using direct VRAM.\n");
    }
}

void gfx_swap_buffers() {
    if (!use_backbuffer || !gfx_ctx.vram_ptr) return;
    if (gfx_ctx.bpp == 24) {
        uint8_t* dst_row = (uint8_t*)gfx_ctx.vram_ptr;
        uint32_t* src_row = gfx_ctx.back_ptr;
        for(int y=0; y < gfx_ctx.height; y++) {
            uint8_t* d = dst_row;
            uint32_t* s = src_row;
            for(int x=0; x < gfx_ctx.width; x++) {
                uint32_t c = *s++;
                d[0] = c & 0xFF; d[1] = (c >> 8) & 0xFF; d[2] = (c >> 16) & 0xFF;
                d += 3;
            }
            dst_row += gfx_ctx.pitch; src_row += gfx_ctx.width;
        }
        return;
    }
    if (gfx_ctx.bpp == 32) {
        if (gfx_ctx.pitch == gfx_ctx.width * 4) {
            memcpy(gfx_ctx.vram_ptr, gfx_ctx.back_ptr, gfx_ctx.width * gfx_ctx.height * 4);
        } else {
            uint8_t* dst = (uint8_t*)gfx_ctx.vram_ptr;
            uint8_t* src = (uint8_t*)gfx_ctx.back_ptr;
            int row_len = gfx_ctx.width * 4;
            for(int y=0; y < gfx_ctx.height; y++) {
                memcpy(dst, src, row_len);
                dst += gfx_ctx.pitch; src += row_len;
            }
        }
    }
}

static inline uint32_t fast_blend(uint32_t bg, uint32_t fg) {
    unsigned int a = (fg >> 24) & 0xFF;
    if (a == 0) return bg;
    if (a == 255) return fg;
    unsigned int inv_a = 255 - a;
    unsigned int rb = (((bg & 0xFF00FF) * inv_a) + ((fg & 0xFF00FF) * a)) >> 8;
    unsigned int g  = (((bg & 0x00FF00) * inv_a) + ((fg & 0x00FF00) * a)) >> 8;
    return 0xFF000000 | (rb & 0xFF00FF) | (g & 0x00FF00);
}

// Helper: Fast Fixed-Point Alpha Blend
static inline uint32_t blend_fast(uint32_t bg, uint32_t fg, uint32_t alpha) {
    if (alpha == 0) return bg;
    if (alpha >= 255) return fg;
    
    // We treat alpha as 0..256 for fast shifting
    uint32_t inv_a = 256 - alpha;
    
    uint32_t rb_bg = bg & 0xFF00FF;
    uint32_t g_bg  = bg & 0x00FF00;
    
    uint32_t rb_fg = fg & 0xFF00FF;
    uint32_t g_fg  = fg & 0x00FF00;
    
    uint32_t rb = (rb_bg * inv_a + rb_fg * alpha) >> 8;
    uint32_t g  = (g_bg * inv_a + g_fg * alpha) >> 8;
    
    return (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000; // Force full alpha on result
}

// Accessor for Desktop to write to blur buffer
uint32_t* gfx_get_blur_buffer() {
    // Allocate if missing (lazy init)
    if (!wallpaper_blur_ptr && gfx_ctx.width) {
        wallpaper_blur_ptr = (uint32_t*)kmalloc(gfx_ctx.width * gfx_ctx.height * 4);
        if (wallpaper_blur_ptr) memset(wallpaper_blur_ptr, 0, gfx_ctx.width * gfx_ctx.height * 4);
    }
    return wallpaper_blur_ptr;
}

// Draw a pixel with Alpha (AA Helper)
void gfx_put_pixel_aa(int x, int y, uint32_t color, uint8_t alpha) {
    if (x < 0 || x >= gfx_ctx.width || y < 0 || y >= gfx_ctx.height) return;
    
    uint32_t* ptr = &gfx_ctx.back_ptr[y * gfx_ctx.width + x];
    
    // Combine geometry alpha with color alpha
    uint32_t col_a = (color >> 24) & 0xFF;
    uint32_t final_a = (col_a * alpha) >> 8;
    
    *ptr = blend_fast(*ptr, color, final_a);
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    if ((unsigned int)x >= (unsigned int)gfx_ctx.width || (unsigned int)y >= (unsigned int)gfx_ctx.height) return;
    if (use_backbuffer) {
        uint32_t* ptr = &gfx_ctx.back_ptr[y * gfx_ctx.width + x];
        unsigned int a = (color >> 24) & 0xFF;
        if(a == 255) *ptr = color;
        else if(a > 0) *ptr = fast_blend(*ptr, color);
    } else {
        if(gfx_ctx.bpp == 32) {
            uint32_t* p = (uint32_t*)((uint8_t*)gfx_ctx.vram_ptr + y*gfx_ctx.pitch + x*4);
            *p = color;
        }
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x >= (int)gfx_ctx.width || y >= (int)gfx_ctx.height) return;
    if (x + w <= 0 || y + h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gfx_ctx.width) w = (int)gfx_ctx.width - x;
    if (y + h > (int)gfx_ctx.height) h = (int)gfx_ctx.height - y;
    if (w <= 0 || h <= 0) return;

    if (use_backbuffer) {
        unsigned int a = (color >> 24) & 0xFF;
        if (a == 255) {
            for(int row=0; row<h; row++) {
                uint32_t* line = &gfx_ctx.back_ptr[(y + row) * gfx_ctx.width + x];
                for(int col=0; col<w; col++) line[col] = color;
            }
        } else if (a > 0) {
            for(int row=0; row<h; row++) {
                uint32_t* line = &gfx_ctx.back_ptr[(y + row) * gfx_ctx.width + x];
                for(int col=0; col<w; col++) line[col] = fast_blend(line[col], color);
            }
        }
    } else {
        for(int row=0; row<h; row++)
            for(int col=0; col<w; col++) gfx_put_pixel(x+col, y+row, color);
    }
}

// === FIXED: Asset Drawing Logic (Correct Clipping) ===
void gfx_draw_asset_scaled(uint32_t* buffer, int x, int y, const uint32_t* data, int sw, int sh, int dw, int dh) {
    if(!data) return;
    if (dw == 0 || dh == 0) return;
    if (sw == 0 || sh == 0) return;

    uint32_t* target = buffer ? buffer : (use_backbuffer ? gfx_ctx.back_ptr : (uint32_t*)gfx_ctx.vram_ptr);

    // Calculate Clipping
    int start_dx = 0, start_dy = 0;
    int end_dx = dw, end_dy = dh;

    // Clip Left/Top
    // IMPORTANT: Do NOT set x=0 or y=0 here. We need the negative offset for the pointer calc!
    if (x < 0) { 
        start_dx = -x; 
    }
    if (y < 0) { 
        start_dy = -y; 
    }

    // Clip Right/Bottom
    if (x + (end_dx - start_dx) > (int)gfx_ctx.width) end_dx = start_dx + ((int)gfx_ctx.width - x);
    if (y + (end_dy - start_dy) > (int)gfx_ctx.height) end_dy = start_dy + ((int)gfx_ctx.height - y);

    if (end_dx <= start_dx || end_dy <= start_dy) return;

    for(int dy = start_dy; dy < end_dy; dy++) {
        int sy = (dy * sh) / dh;
        
        // Safety: ensure sy is within bounds
        if (sy >= sh) sy = sh - 1;

        // Pointer Arithmetic
        // If y is negative (e.g. -5) and dy is 5 (start_dy), y+dy = 0. Safe.
        uint32_t* dest_line = &target[(y + dy) * gfx_ctx.width + x];

        for(int dx = start_dx; dx < end_dx; dx++) {
            int sx = (dx * sw) / dw;
            if (sx >= sw) sx = sw - 1;

            uint32_t pixel = data[sy*sw + sx];
            unsigned int a = (pixel >> 24) & 0xFF;

            if (a > 0) {
               // Access dest_line[dx]. If x=-5, dx=5. dest_line[5].
               // Effectively target[0 + -5 + 5] = target[0]. Correct.
               if(a == 255) dest_line[dx] = pixel;
               else dest_line[dx] = fast_blend(dest_line[dx], pixel);
            }
        }
    }
}

uint32_t* gfx_get_active_buffer() { return use_backbuffer ? gfx_ctx.back_ptr : (uint32_t*)gfx_ctx.vram_ptr; }
void gfx_draw_icon(int x, int y, int w, int h, const uint32_t* data) { gfx_draw_asset_scaled(0, x, y, data, w, h, w, h); }

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_fill_rect(x, y, w, 1, color); 
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color); 
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

static inline int abs(int x) { return x < 0 ? -x : x; }
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        gfx_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_char_scaled(int x, int y, char c, uint32_t color, int scale) {
    int idx = c - 32; if(idx<0) idx=0;
    for(int row=0; row<16; row++) {
        uint8_t line = font_8x16[idx][row];
        for(int col=0; col<8; col++) {
            // Bit 7 is leftmost
            if((line << col) & 0x80) {
                gfx_fill_rect(x+col*scale, y+row*scale, scale, scale, color);
            }
        }
    }
}
void gfx_draw_string_scaled(int x, int y, const char* str, uint32_t color, int scale) { 
    while(*str) { gfx_draw_char_scaled(x, y, *str++, color, scale); x+=6*scale; } 
}
void gfx_draw_string(int x, int y, const char* str, uint32_t color) { 
    gfx_draw_string_scaled(x, y, str, color, 1); 
}

// Rounded Rect Fill (Updated to use blend_fast for edges)
void gfx_fill_rounded_rect(int x, int y, int w, int h, uint32_t color, int r) {
    if (w < 2*r) r = w/2; if (h < 2*r) r = h/2;
    
    // Extract alpha from color
    uint32_t col_alpha = (color >> 24) & 0xFF;
    
    // Center Body
    gfx_fill_rect(x + r, y, w - 2*r, h, color);
    gfx_fill_rect(x, y + r, r, h - 2*r, color);
    gfx_fill_rect(x + w - r, y + r, r, h - 2*r, color);
    
    int r2 = r*r;
    
    // Corners
    for(int dy=0; dy<r; dy++) {
        for(int dx=0; dx<r; dx++) {
            // Distance from center of corner circle
            int cx = r - 1 - dx;
            int cy = r - 1 - dy;
            
            if (cx*cx + cy*cy <= r2) {
                // Pixel is inside. Calculate coordinate for 4 corners
                // TL
                gfx_put_pixel_aa(x + dx, y + dy, color, 255);
                // TR
                gfx_put_pixel_aa(x + w - 1 - dx, y + dy, color, 255);
                // BL
                gfx_put_pixel_aa(x + dx, y + h - 1 - dy, color, 255);
                // BR
                gfx_put_pixel_aa(x + w - 1 - dx, y + h - 1 - dy, color, 255);
            }
        }
    }
}

// --- NEW: Anti-Aliased Rounded Rect (The "Squircle" Look) ---
void gfx_fill_rounded_rect_aa(int x, int y, int w, int h, uint32_t color, int r) {
    if (!use_backbuffer) return;
    
    // Clamp radius
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    if (r < 1) { gfx_fill_rect(x,y,w,h,color); return; }
    
    int r2 = r * r;
    
    // Center Body (Fast Fill)
    gfx_fill_rect(x + r, y, w - 2*r, h, color);
    gfx_fill_rect(x, y + r, r, h - 2*r, color);
    gfx_fill_rect(x + w - r, y + r, r, h - 2*r, color);
    
    // Corners (AA scanline)
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            // Distance from circle center (at r-1, r-1)
            // Invert coords to be relative to circle center
            int cx = r - 1 - dx;
            int cy = r - 1 - dy;
            int dist_sq = cx*cx + cy*cy;
            
            // AA Calculation
            int delta = dist_sq - r2;
            uint8_t alpha = 255;
            
            if (delta >= r) continue; // Completely outside
            if (delta >= -r) {
                // On the edge - approximate AA
                // Map delta (-r to r) to alpha (255 to 0)
                alpha = 255 - ((delta + r) * 255) / (2*r);
            }
            
            // Draw 4 Quadrants
            if (alpha > 0) {
                // TL
                gfx_put_pixel_aa(x + dx, y + dy, color, alpha);
                // TR
                gfx_put_pixel_aa(x + w - 1 - dx, y + dy, color, alpha);
                // BL
                gfx_put_pixel_aa(x + dx, y + h - 1 - dy, color, alpha);
                // BR
                gfx_put_pixel_aa(x + w - 1 - dx, y + h - 1 - dy, color, alpha);
            }
        }
    }
}

// --- NEW: Glass Rect (Samples Blur Buffer) ---
void gfx_draw_glass_rect(int x, int y, int w, int h, int r) {
    if (!wallpaper_blur_ptr || !use_backbuffer) {
        // Fallback to solid translucent white if no blur buffer
        gfx_fill_rounded_rect_aa(x, y, w, h, 0xCCF0F0F0, r);
        return;
    }
    
    uint32_t* back = gfx_ctx.back_ptr;
    uint32_t* blur = wallpaper_blur_ptr;
    int bw = gfx_ctx.width;
    int bh = gfx_ctx.height;
    
    // Iterate pixels
    for (int dy = 0; dy < h; dy++) {
        int ly = y + dy;
        if (ly < 0 || ly >= bh) continue;
        
        for (int dx = 0; dx < w; dx++) {
            int lx = x + dx;
            if (lx < 0 || lx >= bw) continue;
            
            // Check Rounded Corner Mask
            int in_corner = 0;
            int cx=0, cy=0;
            
            if (dx < r) { cx = r - 1 - dx; in_corner = 1; }
            else if (dx >= w - r) { cx = dx - (w - r); in_corner = 1; }
            
            if (dy < r) { cy = r - 1 - dy; in_corner |= 2; }
            else if (dy >= h - r) { cy = dy - (h - r); in_corner |= 2; }
            
            if (in_corner == 3) {
                if (cx*cx + cy*cy >= r*r) continue; // Skip pixel outside corner
            }
            
            // SAMPLE FROM BLUR BUFFER
            uint32_t bg_col = blur[ly * bw + lx];
            
            // Apply Tint (White Tint for Glass)
            // Blend 40% White over the blurred background
            back[ly * bw + lx] = blend_fast(bg_col, 0x50FFFFFF, 80);
        }
    }
    
    // Draw 1px white rim for "Glass Edge" feel
    // (Simplified: reuse rect draw with low alpha stroke)
}
