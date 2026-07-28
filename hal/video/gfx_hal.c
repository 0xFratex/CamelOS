// hal/video/gfx_hal.c
#include "gfx_hal.h"
#include "../../core/memory.h"
#include "../../hal/drivers/vga.h"
#include "../../hal/drivers/serial.h"
#include "../../hal/cpu/paging.h"
#include "../../common/font.h"
#include "../../include/stb_truetype.h"

// ============================================================================
// TRUE TYPE FONT RENDERING WITH GLYPH CACHE (macOS-like smooth text)
// ============================================================================

// Global TrueType font: set once at boot when a .ttf is loaded.
// When non-NULL, gfx_draw_string*() will use TrueType rendering with
// anti-aliasing and subpixel hints, falling back to bitmap for speed.
static const uint8_t* gfx_tt_font_data = NULL;
static stbtt_fontinfo gfx_tt_font_info;
static int gfx_tt_initialized = 0;

// Register a TrueType font for the global gfx text renderer.
// `data` must remain valid for the lifetime of the OS (typically
// static or kmalloc'd font data that is never freed).
void gfx_set_tt_font(const uint8_t* ttf_data) {
    if (!ttf_data) return;
    if (stbtt_InitFont(&gfx_tt_font_info, ttf_data, 0)) {
        gfx_tt_font_data = ttf_data;
        gfx_tt_initialized = 1;
        s_printf("[GFX] TrueType font registered for smooth rendering.\n");
    } else {
        s_printf("[GFX] WARNING: Failed to parse TrueType font data.\n");
    }
}

// --- GLYPH CACHE ---
// Cache rendered TrueType glyphs at common pixel sizes.
// Each entry stores a pre-rendered alpha bitmap for one ASCII character
// at one size.  This avoids re-rasterizing on every frame.

#define GLYPH_CACHE_SIZES  6
static const int glyph_cache_sizes[GLYPH_CACHE_SIZES] = {8, 10, 12, 16, 24, 32};

// Maximum glyph bitmap dimension (pixels).  Even at 32px, glyphs rarely
// exceed 40x40.
#define MAX_GLYPH_DIM  48
#define MAX_GLYPH_PIX  (MAX_GLYPH_DIM * MAX_GLYPH_DIM)

// Per-glyph cached bitmap
typedef struct {
    uint8_t  pixels[MAX_GLYPH_PIX];  // Alpha values (0-255)
    int      width;
    int      height;
    int      xoff;     // X offset from pen position
    int      yoff;     // Y offset from baseline
    int      advance;  // Horizontal advance (pixels)
    int      valid;    // 1 if this entry has been rendered
} glyph_cache_entry_t;

// 128 ASCII chars x 6 sizes = 768 entries.  ~2.4 MB total.
static glyph_cache_entry_t glyph_cache[128][GLYPH_CACHE_SIZES];

// Find the cache size index for a given pixel height.
// Returns -1 if the size is not in our cached set.
static int glyph_cache_find_size(int pixel_height) {
    for (int i = 0; i < GLYPH_CACHE_SIZES; i++) {
        if (glyph_cache_sizes[i] == pixel_height) return i;
    }
    return -1;
}

// Find the nearest cached size <= pixel_height.
static int glyph_cache_nearest_size(int pixel_height) {
    int best = 0;
    for (int i = 1; i < GLYPH_CACHE_SIZES; i++) {
        if (glyph_cache_sizes[i] <= pixel_height) best = i;
    }
    return best;
}

// Render and cache a single glyph.
static glyph_cache_entry_t* glyph_cache_render(int codepoint, int size_idx) {
    if (codepoint < 0 || codepoint >= 128) return NULL;
    if (!gfx_tt_initialized) return NULL;

    glyph_cache_entry_t* entry = &glyph_cache[codepoint][size_idx];
    if (entry->valid) return entry;

    int pixel_height = glyph_cache_sizes[size_idx];
    int scale = stbtt_ScaleForPixelHeight(&gfx_tt_font_info, pixel_height);

    // Get advance width
    int advance_raw, lsb;
    stbtt_GetCodepointHMetrics(&gfx_tt_font_info, codepoint, &advance_raw, &lsb);
    entry->advance = (int)(((long long)advance_raw * scale) >> 16);

    // Render the glyph
    int bw, bh, bxoff, byoff;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(
        &gfx_tt_font_info, scale, scale,
        codepoint, &bw, &bh, &bxoff, &byoff);

    if (!bitmap || bw <= 0 || bh <= 0 || bw > MAX_GLYPH_DIM || bh > MAX_GLYPH_DIM) {
        // Empty glyph (e.g. space)
        entry->width = 0;
        entry->height = 0;
        entry->xoff = 0;
        entry->yoff = 0;
        entry->valid = 1;
        if (bitmap) kfree(bitmap);
        return entry;
    }

    entry->width = bw;
    entry->height = bh;
    entry->xoff = bxoff + (int)(((long long)lsb * scale) >> 16);
    entry->yoff = byoff;

    // Copy bitmap into cache (stbtt result will be freed)
    int total = bw * bh;
    if (total > MAX_GLYPH_PIX) total = MAX_GLYPH_PIX;
    for (int i = 0; i < total; i++) {
        entry->pixels[i] = bitmap[i];
    }

    kfree(bitmap);
    entry->valid = 1;
    return entry;
}

// --- SMOOTH TEXT RENDERING ---
// Draw a string using the TrueType glyph cache with alpha-blended
// anti-aliasing for macOS-quality smooth text.

static void gfx_draw_char_tt(int x, int y, int codepoint, uint32_t color, int pixel_height) {
    if (codepoint < 32 || codepoint >= 127) return;

    int size_idx = glyph_cache_find_size(pixel_height);
    if (size_idx < 0) {
        // Not a cached size – pick nearest and scale on-the-fly
        size_idx = glyph_cache_nearest_size(pixel_height);
    }

    glyph_cache_entry_t* entry = glyph_cache_render(codepoint, size_idx);
    if (!entry || entry->width <= 0) return;

    // Get font baseline for proper Y positioning
    int f_ascent, f_descent, f_linegap;
    stbtt_GetFontVMetrics(&gfx_tt_font_info, &f_ascent, &f_descent, &f_linegap);
    int scale = stbtt_ScaleForPixelHeight(&gfx_tt_font_info, glyph_cache_sizes[size_idx]);
    int baseline = (int)(((long long)f_ascent * scale) >> 16);

    int dest_x = x + entry->xoff;
    int dest_y = y + baseline + entry->yoff;

    // Scale factor if rendering at a different size than cached
    int cached_size = glyph_cache_sizes[size_idx];
    int needs_scale = (cached_size != pixel_height);
    // For simplicity, if not exact size, just render at cached size
    // (the visual difference is minimal for nearby sizes)

    // Extract color components for alpha blending
    uint8_t col_a = (color >> 24) & 0xFF;
    uint8_t col_r = (color >> 16) & 0xFF;
    uint8_t col_g = (color >> 8) & 0xFF;
    uint8_t col_b = color & 0xFF;

    // Composite the alpha bitmap onto the framebuffer
    for (int py = 0; py < entry->height; py++) {
        for (int px = 0; px < entry->width; px++) {
            uint8_t alpha = entry->pixels[py * entry->width + px];
            if (alpha > 0) {
                if (alpha >= 250) {
                    // Nearly opaque – fast path
                    gfx_put_pixel(dest_x + px, dest_y + py, color);
                } else {
                    // Semi-transparent – alpha blend
                    uint8_t blended_a = (col_a * alpha) >> 8;
                    if (blended_a == 0) continue;
                    uint32_t blended = (blended_a << 24) | (col_r << 16) | (col_g << 8) | col_b;
                    gfx_put_pixel(dest_x + px, dest_y + py, blended);
                }
            }
        }
    }
}

// Get the advance width for a character at a given pixel height (TrueType)
static int gfx_tt_char_advance(int codepoint, int pixel_height) {
    if (!gfx_tt_initialized || codepoint < 32 || codepoint >= 127) return 8;
    int scale = stbtt_ScaleForPixelHeight(&gfx_tt_font_info, pixel_height);
    int advance_raw, lsb;
    stbtt_GetCodepointHMetrics(&gfx_tt_font_info, codepoint, &advance_raw, &lsb);
    return (int)(((long long)advance_raw * scale) >> 16);
}

gfx_context_t gfx_ctx;
static int use_backbuffer = 0;

// Static fallback backbuffer for high-resolution modes.
// If kmalloc fails for the backbuffer (e.g., heap too fragmented for a
// 3MB+ contiguous allocation), we fall back to this static buffer.
// Supports up to 1024x768@32bpp (3,145,728 bytes).
// IMPORTANT: Without a valid backbuffer, gfx_put_pixel_aa() and other
// functions that dereference back_ptr directly will corrupt low memory
// (writing through NULL pointer into the first 3MB of physical RAM),
// overwriting kernel code and causing Invalid Opcode panics.
#define STATIC_BACKBUFFER_SIZE (1024 * 768 * 4)
static uint32_t static_backbuffer[STATIC_BACKBUFFER_SIZE / 4] __attribute__((aligned(4096)));
static int using_static_backbuffer = 0;

// --- SOFTWARE CLIP RECTANGLE ---
// When set, all drawing primitives are clipped to this rectangle.
// By default the clip rect covers the entire screen.
static int clip_x1 = 0, clip_y1 = 0;
static int clip_x2 = 0, clip_y2 = 0;
static int clip_enabled = 0;

void gfx_set_clip(int x, int y, int w, int h) {
    clip_x1 = x; clip_y1 = y;
    clip_x2 = x + w; clip_y2 = y + h;
    clip_enabled = 1;
}

void gfx_reset_clip(void) {
    clip_x1 = 0; clip_y1 = 0;
    clip_x2 = gfx_ctx.width; clip_y2 = gfx_ctx.height;
    clip_enabled = 0;
}

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
    int_to_str(gfx_ctx.width, buf); s_printf("%s", buf); s_printf("x");
    int_to_str(gfx_ctx.height, buf); s_printf("%s", buf); s_printf(" VRAM: 0x");
    
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
    gfx_ctx.page_size = gfx_ctx.pitch * gfx_ctx.height;

    // 4. Hardware Page Flipping: DISABLED
    // The VGA CRTC Start Address registers (0x0C/0x0D) are only 16 bits,
    // allowing a max offset of 65535 DWORDs = 256KB. At 1024x768@32bpp,
    // each page is ~3MB, requiring 20 bits to address. The CRTC cannot
    // address the second page, causing the display to wrap around and
    // show garbage/flicker. Use RAM backbuffer + memcpy instead.
    gfx_ctx.use_page_flip = 0;
    gfx_ctx.current_page = 0;
    gfx_ctx.vram_page[0] = gfx_ctx.vram_ptr;
    gfx_ctx.vram_page[1] = 0;

    if (!gfx_ctx.use_page_flip) {
        // Fallback: system RAM backbuffer + memcpy to VRAM
        uint32_t needed = gfx_ctx.width * gfx_ctx.height * 4;
        gfx_ctx.back_ptr = (uint32_t*)kmalloc(needed);

        if (gfx_ctx.back_ptr) {
            use_backbuffer = 1;
            using_static_backbuffer = 0;
            memset(gfx_ctx.back_ptr, 0, needed);
            s_printf("[GFX] Backbuffer Allocated (heap, %d bytes).\n", needed);
        } else if (needed <= STATIC_BACKBUFFER_SIZE) {
            // Heap allocation failed but we can fit in the static buffer
            gfx_ctx.back_ptr = static_backbuffer;
            use_backbuffer = 1;
            using_static_backbuffer = 1;
            memset(gfx_ctx.back_ptr, 0, needed);
            s_printf("[GFX] Backbuffer using static fallback (%d bytes).\n", needed);
        } else {
            // Neither heap nor static buffer is large enough
            use_backbuffer = 0;
            s_printf("[GFX] WARNING: Backbuffer alloc failed! Using direct VRAM.\n");
        }
    }
}

void gfx_swap_buffers() {
    if (!use_backbuffer || !gfx_ctx.vram_ptr) return;

    // Hardware Page Flipping: swap CRTC start address instead of copying
    // This is ~100ns vs ~8ms memcpy, and completely eliminates tearing
    if (gfx_ctx.use_page_flip) {
        // The back_ptr is already pointing to the off-screen VRAM page
        // (rendering went directly into VRAM). Just flip the display.
        extern void vga_set_display_start(uint32_t offset_bytes);
        int next_page = 1 - gfx_ctx.current_page;
        vga_set_display_start(next_page * gfx_ctx.page_size);
        gfx_ctx.current_page = next_page;
        // Set back_ptr to the NEW off-screen page for next frame's rendering
        gfx_ctx.back_ptr = gfx_ctx.vram_page[1 - gfx_ctx.current_page];
        return;
    }

    // Fallback: copy system RAM backbuffer to VRAM
    if (gfx_ctx.bpp == 24) {
        // Optimised 24bpp conversion: process 4 pixels at a time using
        // 32-bit writes.  Each source pixel is 4 bytes (XRGB), each dest
        // pixel is 3 bytes (RGB).  Four source pixels = 16 bytes → 12 dest
        // bytes = 3 DWORDs.  This reduces loop overhead by 4x and lets the
        // compiler use 32-bit stores instead of three 8-bit stores per pixel.
        uint8_t* dst_row = (uint8_t*)gfx_ctx.vram_ptr;
        uint32_t* src_row = gfx_ctx.back_ptr;
        int w = gfx_ctx.width;
        int h = gfx_ctx.height;

        for(int y = 0; y < h; y++) {
            uint8_t* d = dst_row;
            uint32_t* s = src_row;
            int x = 0;

            // Process 4 pixels at a time
            int chunks = w >> 2;  // w / 4
            for(int c = 0; c < chunks; c++) {
                uint32_t p0 = s[0], p1 = s[1], p2 = s[2], p3 = s[3];
                s += 4;

                // Pack 4 pixels (12 bytes) into 3 DWORD writes
                // Pixel 0: R0 G0 B0 | Pixel 1: R1 → DWORD0 = B0 G0 R0 R1
                // Pixel 1: G1 B1 | Pixel 2: R2 G2 → DWORD1 = G2 R2 B1 G1
                // Pixel 2: B2 | Pixel 3: R3 G3 B3 → DWORD2 = B3 G3 R3 B2
                uint32_t dw0 = (p0 & 0xFFFFFF) | ((p1 & 0xFF) << 24);
                uint32_t dw1 = ((p1 >> 8) & 0xFFFF) | ((p2 & 0xFFFF) << 16);
                uint32_t dw2 = ((p2 >> 16) & 0xFF) | ((p3 & 0xFFFFFF) << 8);

                // Write 3 DWORDs (12 bytes)
                *((uint32_t*)d) = dw0;
                *((uint32_t*)(d + 4)) = dw1;
                *((uint32_t*)(d + 8)) = dw2;
                d += 12;
            }

            // Handle remaining pixels (0-3)
            x = chunks << 2;
            for(; x < w; x++) {
                uint32_t c = *s++;
                d[0] = c & 0xFF;
                d[1] = (c >> 8) & 0xFF;
                d[2] = (c >> 16) & 0xFF;
                d += 3;
            }

            dst_row += gfx_ctx.pitch;
            src_row += w;
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
    if (alpha >= 255) return 0xFF000000 | (fg & 0x00FFFFFF);  // Force opaque result
    
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
    // Software clip rectangle — prevents rounded-corner pixels from
    // leaking outside the window content area during selection box drawing.
    if (clip_enabled && (x < clip_x1 || x >= clip_x2 || y < clip_y1 || y >= clip_y2)) return;
    // CRITICAL: Must check use_backbuffer before dereferencing back_ptr.
    // When back_ptr is NULL (backbuffer alloc failed), writing through it
    // corrupts low physical memory (first 3MB), overwriting kernel code
    // and causing Invalid Opcode panics.
    if (!use_backbuffer || !gfx_ctx.back_ptr) return;
    
    uint32_t* ptr = &gfx_ctx.back_ptr[y * gfx_ctx.width + x];
    
    // Combine geometry alpha with color alpha
    uint32_t col_a = (color >> 24) & 0xFF;
    uint32_t final_a = (col_a * alpha) >> 8;
    
    *ptr = blend_fast(*ptr, color, final_a);
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    if ((unsigned int)x >= (unsigned int)gfx_ctx.width || (unsigned int)y >= (unsigned int)gfx_ctx.height) return;
    // Software clip rectangle
    if (clip_enabled && (x < clip_x1 || x >= clip_x2 || y < clip_y1 || y >= clip_y2)) return;
    if (use_backbuffer && gfx_ctx.back_ptr) {
        uint32_t* ptr = &gfx_ctx.back_ptr[y * gfx_ctx.width + x];
        unsigned int a = (color >> 24) & 0xFF;
        if(a == 255) *ptr = color;
        else if(a > 0) *ptr = fast_blend(*ptr, color);
    } else if (gfx_ctx.vram_ptr) {
        if(gfx_ctx.bpp == 32) {
            uint32_t* p = (uint32_t*)((uint8_t*)gfx_ctx.vram_ptr + y*gfx_ctx.pitch + x*4);
            *p = color;
        } else if(gfx_ctx.bpp == 24) {
            uint8_t* p = (uint8_t*)gfx_ctx.vram_ptr + y*gfx_ctx.pitch + x*3;
            p[0] = color & 0xFF;
            p[1] = (color >> 8) & 0xFF;
            p[2] = (color >> 16) & 0xFF;
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
    // Apply software clip rectangle
    if (clip_enabled) {
        if (x < clip_x1) { w -= (clip_x1 - x); x = clip_x1; }
        if (y < clip_y1) { h -= (clip_y1 - y); y = clip_y1; }
        if (x + w > clip_x2) w = clip_x2 - x;
        if (y + h > clip_y2) h = clip_y2 - y;
    }
    if (w <= 0 || h <= 0) return;

    if (use_backbuffer && gfx_ctx.back_ptr) {
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
    } else if (gfx_ctx.vram_ptr) {
        // Direct VRAM fast fill (no backbuffer)
        if(gfx_ctx.bpp == 32 && (color >> 24) == 0xFF) {
            // Fast path: opaque fill to 32bpp VRAM
            for(int row=0; row<h; row++) {
                uint32_t* line = (uint32_t*)((uint8_t*)gfx_ctx.vram_ptr + (y+row)*gfx_ctx.pitch + x*4);
                for(int col=0; col<w; col++) line[col] = color;
            }
        } else {
            for(int row=0; row<h; row++)
                for(int col=0; col<w; col++) gfx_put_pixel(x+col, y+row, color);
        }
    }
}

// === FIXED: Asset Drawing Logic (Correct Clipping) ===
void gfx_draw_asset_scaled(uint32_t* buffer, int x, int y, const uint32_t* data, int sw, int sh, int dw, int dh) {
    if(!data) return;
    if (dw == 0 || dh == 0) return;
    if (sw == 0 || sh == 0) return;

    uint32_t* target = buffer ? buffer : (use_backbuffer && gfx_ctx.back_ptr ? gfx_ctx.back_ptr : (uint32_t*)gfx_ctx.vram_ptr);
    if (!target) return;

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

uint32_t* gfx_get_active_buffer() { return (use_backbuffer && gfx_ctx.back_ptr) ? gfx_ctx.back_ptr : (uint32_t*)gfx_ctx.vram_ptr; }
/* gfx_get_width/gfx_get_height moved to gfx_hal.h as static inline */
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
    // Use unsigned char to handle Latin-1 characters (160-255) correctly
    unsigned char uc = (unsigned char)c;
    const uint8_t* glyph;
    
    if (uc >= 32 && uc <= 127) {
        glyph = font_8x16[uc - 32];
    } else if (uc >= 160 && uc <= 255) {
        glyph = font_latin1_8x16[uc - 160];
    } else {
        glyph = font_8x16[0]; // Fallback to space
    }
    
    for(int row=0; row<16; row++) {
        uint8_t line = glyph[row];
        for(int col=0; col<8; col++) {
            // Bit 7 is leftmost
            if((line << col) & 0x80) {
                gfx_fill_rect(x+col*scale, y+row*scale, scale, scale, color);
            }
        }
    }
}

// --- ENHANCED STRING RENDERING ---
// When a TrueType font is registered, use it for smooth, anti-aliased text.
// Otherwise fall back to the bitmap font (fast, always available).

void gfx_draw_string_scaled(int x, int y, const char* str, uint32_t color, int scale) {
    if (!str) return;

    // If TrueType font is available, use smooth rendering
    if (gfx_tt_initialized) {
        // Map scale to pixel height: scale=1 → 12px, scale=2 → 20px, etc.
        int pixel_height;
        switch (scale) {
            case 1:  pixel_height = 12; break;
            case 2:  pixel_height = 20; break;
            case 3:  pixel_height = 32; break;
            default: pixel_height = 12 * scale; break;
        }

        // Get baseline for Y positioning
        int f_ascent, f_descent, f_linegap;
        stbtt_GetFontVMetrics(&gfx_tt_font_info, &f_ascent, &f_descent, &f_linegap);
        int tt_scale = stbtt_ScaleForPixelHeight(&gfx_tt_font_info, pixel_height);
        int baseline = (int)(((long long)f_ascent * tt_scale) >> 16);

        int cursor_x = x;
        int prev_char = 0;

        while (*str) {
            int codepoint = (unsigned char)*str;

            // Only render ASCII (32-126) with TrueType; Latin-1 falls back
            if (codepoint >= 32 && codepoint < 127) {
                // Apply kerning
                if (prev_char) {
                    int kern = stbtt_GetCodepointKernAdvance(&gfx_tt_font_info, prev_char, codepoint);
                    cursor_x += (int)(((long long)kern * tt_scale) >> 16);
                }
                prev_char = codepoint;

                gfx_draw_char_tt(cursor_x, y, codepoint, color, pixel_height);
                cursor_x += gfx_tt_char_advance(codepoint, pixel_height);
            } else if (codepoint >= 160 && codepoint <= 255) {
                // Latin-1: fall back to bitmap for now
                gfx_draw_char_scaled(cursor_x, y + baseline - 12, *str, color, 1);
                cursor_x += 8;
                prev_char = 0;
            } else {
                cursor_x += 4; // Unknown char
                prev_char = 0;
            }
            str++;
        }
        return;
    }

    // Fallback: bitmap font
    while(*str) { gfx_draw_char_scaled(x, y, *str++, color, scale); x+=8*scale; }
}

void gfx_draw_string(int x, int y, const char* str, uint32_t color) {
    gfx_draw_string_scaled(x, y, str, color, 1);
}
void gfx_draw_string_clipped(int x, int y, const char* str, uint32_t color, int max_width) {
    // Draw string but only render characters that fit within max_width pixels
    if (!str || max_width <= 0) return;
    int max_chars = max_width / 8;
    int i = 0;
    while (*str && i < max_chars) {
        gfx_draw_char_scaled(x, y, *str, color, 1);
        x += 8;
        str++;
        i++;
    }
}
void gfx_draw_string_centered(int cx, int y, const char* str, uint32_t color, int scale) {
    int len = 0; while (str[len]) len++;
    gfx_draw_string_scaled(cx - len * 4 * scale, y, str, color, scale);
}

// Rounded Rect Fill (Updated to use blend_fast for edges)
void gfx_fill_rounded_rect(int x, int y, int w, int h, uint32_t color, int r) {
    if (w < 2*r) r = w/2;
    if (h < 2*r) r = h/2;
    
    // Alpha extracted from color (used for future blend optimization)
    (void)0; /* col_alpha removed */
    
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
    if (!use_backbuffer || !gfx_ctx.back_ptr) return;
    
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
    if (!wallpaper_blur_ptr || !use_backbuffer || !gfx_ctx.back_ptr) {
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

// ============================================================================
// Stroke Rounded Rect (draw outline only, no fill)
// ============================================================================
void gfx_stroke_rounded_rect(int x, int y, int w, int h, uint32_t color, int r, int line_width) {
    if (w < 2*r) r = w/2;
    if (h < 2*r) r = h/2;
    if (r < 1) { gfx_draw_rect(x, y, w, h, color); return; }
    // Clamp line_width to radius to avoid degenerate stroke
    if (line_width > r) line_width = r;
    
    // Use a single unified pixel loop for the entire stroke ring.
    // This eliminates the gaps that appeared between edge strips and
    // corner arcs when the old two-pass approach (fill_rect edges +
    // per-pixel corners) used slightly different coordinate math.
    int r2 = r * r;
    int inner_r = r - line_width;
    int inner_r2 = inner_r * inner_r;
    
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int cx = r - 1 - dx;
            int cy = r - 1 - dy;
            int dist_sq = cx * cx + cy * cy;
            
            // Pixel is on the stroke ring if inside outer radius but outside inner
            if (dist_sq <= r2 && dist_sq >= inner_r2) {
                // TL
                gfx_put_pixel(x + dx, y + dy, color);
                // TR
                gfx_put_pixel(x + w - 1 - dx, y + dy, color);
                // BL
                gfx_put_pixel(x + dx, y + h - 1 - dy, color);
                // BR
                gfx_put_pixel(x + w - 1 - dx, y + h - 1 - dy, color);
            }
        }
    }
    
    // Top edge (between corner arcs)
    gfx_fill_rect(x + r, y, w - 2*r, line_width, color);
    // Bottom edge
    gfx_fill_rect(x + r, y + h - line_width, w - 2*r, line_width, color);
    // Left edge
    gfx_fill_rect(x, y + r, line_width, h - 2*r, color);
    // Right edge
    gfx_fill_rect(x + w - line_width, y + r, line_width, h - 2*r, color);
}

// ============================================================================
// Dirty-Region Tracking
// ============================================================================

static dirty_rect_t g_dirty = {0, 0, 0, 0, 0};

void gfx_mark_dirty(int x, int y, int w, int h) {
    // Clamp to screen bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gfx_ctx.width)  w = (int)gfx_ctx.width - x;
    if (y + h > (int)gfx_ctx.height) h = (int)gfx_ctx.height - y;
    if (w <= 0 || h <= 0) return;

    if (!g_dirty.valid) {
        g_dirty.x = x; g_dirty.y = y;
        g_dirty.w = w; g_dirty.h = h;
        g_dirty.valid = 1;
    } else {
        // Merge: compute bounding box of old + new
        int x1 = g_dirty.x;
        int y1 = g_dirty.y;
        int x2 = g_dirty.x + g_dirty.w;
        int y2 = g_dirty.y + g_dirty.h;
        if (x < x1) x1 = x;
        if (y < y1) y1 = y;
        if (x + w > x2) x2 = x + w;
        if (y + h > y2) y2 = y + h;
        g_dirty.x = x1; g_dirty.y = y1;
        g_dirty.w = x2 - x1; g_dirty.h = y2 - y1;
    }
}

void gfx_mark_dirty_all(void) {
    g_dirty.x = 0; g_dirty.y = 0;
    g_dirty.w = gfx_ctx.width; g_dirty.h = gfx_ctx.height;
    g_dirty.valid = 1;
}

int gfx_get_dirty_rect(int* x, int* y, int* w, int* h) {
    if (!g_dirty.valid) return 0;
    if (x) *x = g_dirty.x;
    if (y) *y = g_dirty.y;
    if (w) *w = g_dirty.w;
    if (h) *h = g_dirty.h;
    return 1;
}

void gfx_clear_dirty(void) {
    g_dirty.valid = 0;
}

int gfx_is_dirty(void) {
    return g_dirty.valid;
}

// Check if the dirty region covers the entire screen.
// When true, a full redraw is needed (no point in partial updates).
int gfx_dirty_is_full(void) {
    if (!g_dirty.valid) return 0;
    return (g_dirty.x == 0 && g_dirty.y == 0 &&
            g_dirty.w >= (int)gfx_ctx.width &&
            g_dirty.h >= (int)gfx_ctx.height);
}

// ============================================================================
// Partial Buffer Swap — only copy the dirty region to VRAM.
// Much faster than a full 3 MB memcpy when only a small area changed
// (e.g. a window moved).  Falls back to gfx_swap_buffers() if the
// dirty region covers most of the screen.
// ============================================================================

void gfx_swap_buffers_region(int rx, int ry, int rw, int rh) {
    if (!use_backbuffer || !gfx_ctx.vram_ptr) return;

    // If dirty region covers > 80% of screen, just do a full swap
    // (the per-row overhead of region copying isn't worth it)
    int screen_area = (int)gfx_ctx.width * (int)gfx_ctx.height;
    int region_area = rw * rh;
    if (region_area * 10 > screen_area * 8) {
        gfx_swap_buffers();
        return;
    }

    // Clamp region to screen
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int)gfx_ctx.width)  rw = (int)gfx_ctx.width - rx;
    if (ry + rh > (int)gfx_ctx.height) rh = (int)gfx_ctx.height - ry;
    if (rw <= 0 || rh <= 0) return;

    // For 32bpp with matching pitch, use fast row-by-row copy
    if (gfx_ctx.bpp == 32) {
        int row_bytes = rw * 4;
        for (int row = 0; row < rh; row++) {
            uint8_t* dst = (uint8_t*)gfx_ctx.vram_ptr + (ry + row) * gfx_ctx.pitch + rx * 4;
            uint8_t* src = (uint8_t*)gfx_ctx.back_ptr + (ry + row) * gfx_ctx.width * 4 + rx * 4;
            memcpy(dst, src, row_bytes);
        }
    } else if (gfx_ctx.bpp == 24) {
        // 24bpp: convert each pixel from XRGB to RGB
        for (int row = 0; row < rh; row++) {
            uint8_t* d = (uint8_t*)gfx_ctx.vram_ptr + (ry + row) * gfx_ctx.pitch + rx * 3;
            uint32_t* s = &gfx_ctx.back_ptr[(ry + row) * gfx_ctx.width + rx];
            for (int col = 0; col < rw; col++) {
                uint32_t c = s[col];
                d[col * 3]     = c & 0xFF;
                d[col * 3 + 1] = (c >> 8) & 0xFF;
                d[col * 3 + 2] = (c >> 16) & 0xFF;
            }
        }
    }
}

// ============================================================================
// Box Blur (3-pass) - Generates blur buffer from current backbuffer
// Call this after drawing the wallpaper, before drawing windows
// This is implemented in compositor.c to avoid circular includes
// ============================================================================
