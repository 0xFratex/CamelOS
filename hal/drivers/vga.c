// hal/drivers/vga.c
#include "vga.h"
#include "../core/memory.h"
#include "../common/font.h"

// Import from gfx_hal.c
extern void gfx_put_pixel(int x, int y, uint32_t color);
extern void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
extern void gfx_swap_buffers(); // Required to show output on screen

// Global state for the graphical console
static int g_con_x = 0;
static int g_con_y = 0;

uint8_t vga_approx_color(uint32_t rgba) {
    int r = (rgba >> 16) & 0xFF;
    int g = (rgba >> 8) & 0xFF;
    int b = rgba & 0xFF;
    int brightness = (r + g + b) / 3;
    
    if (r > 160 && g > 140 && b < 140) return 14; 
    if (r > 120 && g > 60 && b < 60) return 6;    
    if (r > 150) return 12; 
    if (g > 150) return 10; 
    if (b > 150) return 9;  
    if (brightness < 32) return 0;
    if (brightness > 220) return 15;
    return 32 + (brightness / 8);
}


// --- Legacy Register Stuff ---
uint32_t* gfx_mem = 0;
int screen_w = 0;
int screen_h = 0;
int screen_pitch = 0;
int screen_bpp = 0;
static uint16_t* text_mem = (uint16_t*)0xB8000;
static int text_x = 0;
static int text_y = 0;
static uint8_t text_color = 0x07;
static int g_suppress_text = 0;

void vga_mute_log(int enable) {
    g_suppress_text = enable;
}

void write_regs(unsigned char *regs) {
    unsigned int i;
    outb(0x3C2, *regs); regs++;
    for(i = 0; i < 5; i++) { outb(0x3C4, i); outb(0x3C5, *regs); regs++; }
    outb(0x3D4, 0x03); outb(0x3D5, inb(0x3D5) | 0x80);
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & ~0x80);
    regs[0x03] |= 0x80; regs[0x11] &= ~0x80;
    for(i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, *regs); regs++; }
    for(i = 0; i < 9; i++) { outb(0x3CE, i); outb(0x3CF, *regs); regs++; }
    for(i = 0; i < 21; i++) { inb(0x3DA); outb(0x3C0, i); outb(0x3C0, *regs); regs++; }
    inb(0x3DA); outb(0x3C0, 0x20);
}

unsigned char mode_13h_regs[] = {
    0x63, 0x03, 0x01, 0x0F, 0x00, 0x0E, 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 
    0xB9, 0xA3, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF, 0x00, 0x01, 
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

void set_palette(int idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, idx); outb(0x3C9, r); outb(0x3C9, g); outb(0x3C9, b);
}

void setup_default_palette() {
    set_palette(0, 0, 0, 0);
    set_palette(1, 0, 0, 42);
    set_palette(2, 0, 42, 0);
    set_palette(3, 0, 42, 42);
    set_palette(4, 42, 0, 0);
    set_palette(5, 42, 0, 42);
    set_palette(6, 42, 21, 0);
    set_palette(7, 42, 42, 42);
    set_palette(8, 21, 21, 21);
    set_palette(9, 21, 21, 63);
    set_palette(10, 21, 63, 21);
    set_palette(11, 21, 63, 63);
    set_palette(12, 63, 21, 21);
    set_palette(13, 63, 21, 63);
    set_palette(14, 63, 55, 10);
    set_palette(15, 63, 63, 63);
    set_palette(16, 15, 30, 32);
    set_palette(17, 0, 0, 32);
    set_palette(18, 60, 60, 58);
    set_palette(19, 25, 37, 59);
    set_palette(20, 12, 12, 12);
    set_palette(22, 10, 10, 10);
    set_palette(23, 14, 14, 14);
    set_palette(24, 10, 10, 10);
    for(int i=25; i<32; i++) set_palette(i, 0,0,0);
    for(int i=0; i<32; i++) {
        uint8_t val = i * 2;
        set_palette(32 + i, val, val, val);
    }
}

void init_vga_graphics() {
    write_regs(mode_13h_regs);
    setup_default_palette();
    screen_w = 320; screen_h = 200; screen_bpp = 8; screen_pitch = 320;
    gfx_mem = (uint32_t*)0xA0000;
    uint8_t* vram = (uint8_t*)gfx_mem;
    for(int i=0; i<64000; i++) vram[i] = 0;
}

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed)) multiboot_info_t;

void init_vga_multiboot(void* mboot_ptr) {
    if (!mboot_ptr) return;
    multiboot_info_t* mb = (multiboot_info_t*)mboot_ptr;
    if (mb->flags & (1 << 12)) {
        gfx_mem = (uint32_t*)(uint32_t)mb->framebuffer_addr;
        screen_w = mb->framebuffer_width;
        screen_h = mb->framebuffer_height;
        screen_pitch = mb->framebuffer_pitch;
        screen_bpp = mb->framebuffer_bpp;
    }
}

static uint32_t* draw_target = 0;
void gfx_set_target(uint32_t* buffer) { draw_target = buffer; }

void gfx_draw_char(int x, int y, char c, uint32_t color) {
    int idx = c - 32;
    if (idx < 0 || idx > 95) idx = 0;
    
    // Draw 8x16 background box for readability overlay
    gfx_fill_rect(x, y, 8, 16, 0xAA000000); // Semi-transparent black

    for(int row=0; row<16; row++) {
        uint8_t line = font_8x16[idx][row];
        for(int col=0; col<8; col++) {
            // Bit 7 is leftmost
            if((line << col) & 0x80) {
                gfx_put_pixel(x + col, y + row, color);
            }
        }
    }
}

void vga_text_print(const char* str) {
    while (*str) {
        if (*str == '\n') { text_x = 0; text_y++; } 
        else {
            int offset = (text_y * 80 + text_x) * 2;
            text_mem[offset/2] = (uint16_t)(*str) | (text_color << 8);
            text_x++;
        }
        if (text_x >= 80) { text_x = 0; text_y++; }
        if (text_y >= 25) text_y = 0; 
        str++;
    }
}

// === FIXED: High-Resolution Console Output ===
void vga_print(const char* str) {
    extern void s_printf(const char*);
    s_printf(str); // Always log to serial for debugging

    // If in GUI mode (32bpp or 24bpp), draw text using fonts
    if (screen_bpp == 32 || screen_bpp == 24) {
        // Check if framebuffer is ready
        if (!gfx_mem) return;

        while (*str) {
            char c = *str++;
            if (c == '\n') {
                g_con_x = 0;
                g_con_y += 16; // 8x16 font height
            } else if (c == '\b') {
                 if (g_con_x >= 8) {
                     g_con_x -= 8;
                     gfx_fill_rect(g_con_x, g_con_y, 8, 16, 0xFF000000);
                 }
            } else if (c >= 32) {
                gfx_draw_char(g_con_x, g_con_y, c, 0xFFFFFFFF);
                g_con_x += 8;
            }
            
            // Wrap
            if (g_con_x >= screen_w - 8) {
                g_con_x = 0;
                g_con_y += 16;
            }
            
            // Scroll (Reset to top for simplicity)
            if (g_con_y >= screen_h - 16) {
                g_con_y = 0;
                // For performance, we don't full clear, just loop.
                // Or clear strictly the top left area?
                // gfx_fill_rect(0, 0, screen_w, screen_h, 0xFF000000);
            }
        }
        
        // FORCE BUFFER SWAP so text is visible immediately
        gfx_swap_buffers();
        return;
    }

    // Fallback to standard VGA Text Mode
    // ... (Legacy code omitted for brevity) ...
    vga_text_print(str);
}

void vga_clear() {
    if(gfx_mem && (screen_bpp == 32 || screen_bpp == 24)) {
        gfx_fill_rect(0, 0, screen_w, screen_h, 0xFF000000);
        g_con_x = 0;
        g_con_y = 0;
        gfx_swap_buffers();
    } else {
        for (int i = 0; i < 80 * 25; i++) text_mem[i] = (0x20) | (text_color << 8);
        text_x = 0; text_y = 0;
    }
}

void vga_wait_vsync() {
    while(inb(0x3DA) & 8);
    while(!(inb(0x3DA) & 8));
}

void vga_set_color(unsigned char fg, unsigned char bg) {
    text_color = (bg << 4) | (fg & 0x0F);
}

void vga_print_char(char c) {
    char buf[2] = {c, 0};
    vga_print(buf);
}

void vga_print_int(int value) {
    char buf[12];
    extern void int_to_str(int, char*); 
    int_to_str(value, buf);
    vga_print(buf);
}