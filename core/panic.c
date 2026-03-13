// core/panic.c - Beautiful MacOS-style Kernel Panic Screen
#include "panic.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/isr.h"
#include "../common/ports.h"
#include "../common/font.h"
#include "string.h"
#include "memory.h"

// MacOS-style panic colors
#define PANIC_BG_COLOR      0xFF1C1C1E    // Dark gray background
#define PANIC_ACCENT_COLOR  0xFF007AFF    // Apple blue
#define PANIC_TEXT_COLOR    0xFFFFFFFF    // White text
#define PANIC_DIM_COLOR     0xFF8E8E93    // Gray text
#define PANIC_RED_COLOR     0xFFFF375F    // Apple red
#define PANIC_YELLOW_COLOR  0xFFFFD60A    // Apple yellow

// Drawing helpers for panic screen
static void panic_draw_rect(int x, int y, int w, int h, uint32_t color) {
    extern gfx_context_t gfx_ctx;
    if (!gfx_ctx.back_ptr) return;
    
    for (int dy = 0; dy < h && (y + dy) < gfx_ctx.height; dy++) {
        for (int dx = 0; dx < w && (x + dx) < gfx_ctx.width; dx++) {
            if ((y + dy) >= 0 && (x + dx) >= 0) {
                gfx_ctx.back_ptr[(y + dy) * gfx_ctx.width + (x + dx)] = color;
            }
        }
    }
}

static void panic_draw_gradient_background(void) {
    extern gfx_context_t gfx_ctx;
    if (!gfx_ctx.back_ptr) return;
    
    // Create a subtle gradient background
    for (int y = 0; y < gfx_ctx.height; y++) {
        // Blend from dark to slightly lighter
        uint8_t gradient = (y * 10) / gfx_ctx.height;
        uint32_t color = PANIC_BG_COLOR + (gradient << 16) + (gradient << 8) + gradient;
        
        for (int x = 0; x < gfx_ctx.width; x++) {
            gfx_ctx.back_ptr[y * gfx_ctx.width + x] = color;
        }
    }
}

static void panic_draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color) {
    // Draw main rectangle
    panic_draw_rect(x + radius, y, w - 2*radius, h, color);
    panic_draw_rect(x, y + radius, w, h - 2*radius, color);
    
    // Draw rounded corners (simple approximation)
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            if (dx*dx + dy*dy <= radius*radius) {
                // Top-left
                panic_draw_rect(x + radius - dx, y + radius - dy, 1, 1, color);
                // Top-right
                panic_draw_rect(x + w - radius + dx - 1, y + radius - dy, 1, 1, color);
                // Bottom-left
                panic_draw_rect(x + radius - dx, y + h - radius + dy - 1, 1, 1, color);
                // Bottom-right
                panic_draw_rect(x + w - radius + dx - 1, y + h - radius + dy - 1, 1, 1, color);
            }
        }
    }
}

// Draw a large panic icon (sad Mac style)
static void panic_draw_icon(int cx, int cy) {
    int size = 80;
    
    // Draw circle background
    for (int y = -size; y <= size; y++) {
        for (int x = -size; x <= size; x++) {
            if (x*x + y*y <= size*size) {
                panic_draw_rect(cx + x, cy + y, 1, 1, PANIC_RED_COLOR);
            }
        }
    }
    
    // Draw "X" in white
    int thickness = 8;
    for (int i = -40; i <= 40; i++) {
        for (int t = 0; t < thickness; t++) {
            // Diagonal 1
            panic_draw_rect(cx + i, cy + i + t - thickness/2, 1, 1, 0xFFFFFFFF);
            // Diagonal 2
            panic_draw_rect(cx + i, cy - i + t - thickness/2, 1, 1, 0xFFFFFFFF);
        }
    }
}

// Draw text with larger font (2x scaled)
static void panic_draw_text_scaled(int x, int y, const char* text, uint32_t color, int scale) {
    extern const uint8_t font_8x16[96][16];
    
    int orig_x = x;
    while (*text) {
        char c = *text++;
        if (c < 32 || c > 127) c = '?';
        
        const uint8_t* char_data = font_8x16[c - 32];
        
        for (int row = 0; row < 16; row++) {
            uint8_t row_data = char_data[row];
            for (int col = 0; col < 8; col++) {
                if (row_data & (1 << (7 - col))) {
                    // Draw scaled pixel
                    panic_draw_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        x += 8 * scale;
    }
}

// Draw normal text
static void panic_draw_text(int x, int y, const char* text, uint32_t color) {
    panic_draw_text_scaled(x, y, text, color, 1);
}

// Draw hex value
static void panic_hex(uint32_t n, char* buf) {
    char* chars = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = chars[(n >> (28 - i * 4)) & 0xF];
    }
    buf[10] = 0;
}

// Draw decimal value
static void panic_int(int n, char* buf) {
    if (n == 0) {
        buf[0] = '0'; buf[1] = 0;
        return;
    }
    
    int i = 0;
    int neg = 0;
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    
    char temp[12];
    while (n > 0) {
        temp[i++] = '0' + (n % 10);
        n /= 10;
    }
    
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = 0;
}

// Get interrupt name
static const char* get_interrupt_name(uint32_t int_no) {
    switch (int_no) {
        case 0: return "Divide by Zero";
        case 1: return "Debug Exception";
        case 2: return "NMI";
        case 3: return "Breakpoint";
        case 4: return "Overflow";
        case 5: return "Bounds Check";
        case 6: return "Invalid Opcode";
        case 7: return "FPU Not Available";
        case 8: return "Double Fault";
        case 10: return "Invalid TSS";
        case 11: return "Segment Not Present";
        case 12: return "Stack Fault";
        case 13: return "General Protection Fault";
        case 14: return "Page Fault";
        case 16: return "FPU Error";
        case 17: return "Alignment Check";
        case 18: return "Machine Check";
        case 19: return "SIMD Exception";
        default: return "Unknown Interrupt";
    }
}

// Main panic function
void panic(const char* msg, registers_t* regs) {
    asm volatile("cli"); // Disable interrupts immediately
    
    // Log to serial first
    s_printf("\n\n[!!!] KERNEL PANIC [!!!]\n");
    s_printf("Reason: "); s_printf(msg); s_printf("\n");
    
    // Initialize graphics if not already done
    extern gfx_context_t gfx_ctx;
    extern void gfx_init_hal(void*);
    
    if (!gfx_ctx.back_ptr) {
        // Try to init graphics - pass NULL for multiboot
        gfx_init_hal(NULL);
    }
    
    // Clear screen with gradient
    panic_draw_gradient_background();
    
    // Screen dimensions
    int screen_w = gfx_ctx.width ? gfx_ctx.width : 1024;
    int screen_h = gfx_ctx.height ? gfx_ctx.height : 768;
    int cx = screen_w / 2;
    
    // Draw main icon
    panic_draw_icon(cx, 120);
    
    // Draw title
    panic_draw_text_scaled(cx - 200, 220, "Camel OS Kernel Panic", PANIC_TEXT_COLOR, 2);
    
    // Draw error message
    panic_draw_text(cx - 250, 280, "The system has encountered a fatal error and cannot continue.", PANIC_DIM_COLOR);
    
    // Draw error details box
    int box_x = cx - 300;
    int box_y = 320;
    int box_w = 600;
    int box_h = 200;
    
    panic_draw_rounded_rect(box_x, box_y, box_w, box_h, 10, 0xFF2C2C2E);
    panic_draw_rect(box_x + 2, box_y + 2, box_w - 4, box_h - 4, 0xFF3A3A3C);
    
    // Draw error details
    char buf[64];
    int y = box_y + 20;
    
    panic_draw_text(box_x + 20, y, "Error:", PANIC_DIM_COLOR);
    panic_draw_text(box_x + 120, y, msg, PANIC_RED_COLOR);
    y += 30;
    
    if (regs) {
        panic_draw_text(box_x + 20, y, "Interrupt:", PANIC_DIM_COLOR);
        panic_int(regs->int_no, buf);
        panic_draw_text(box_x + 120, y, buf, PANIC_YELLOW_COLOR);
        panic_draw_text(box_x + 180, y, "(", PANIC_DIM_COLOR);
        const char* int_name = get_interrupt_name(regs->int_no);
        panic_draw_text(box_x + 190, y, int_name, PANIC_DIM_COLOR);
        panic_draw_text(box_x + 190 + strlen(int_name) * 8, y, ")", PANIC_DIM_COLOR);
        y += 30;
        
        // Draw register dump in two columns
        int col1_x = box_x + 20;
        int col2_x = box_x + 320;
        int row_y = y;
        
        // Column 1
        panic_draw_text(col1_x, row_y, "EAX:", PANIC_DIM_COLOR);
        panic_hex(regs->eax, buf);
        panic_draw_text(col1_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col1_x, row_y, "EBX:", PANIC_DIM_COLOR);
        panic_hex(regs->ebx, buf);
        panic_draw_text(col1_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col1_x, row_y, "ECX:", PANIC_DIM_COLOR);
        panic_hex(regs->ecx, buf);
        panic_draw_text(col1_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col1_x, row_y, "EDX:", PANIC_DIM_COLOR);
        panic_hex(regs->edx, buf);
        panic_draw_text(col1_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        
        // Column 2
        row_y = y;
        panic_draw_text(col2_x, row_y, "ESP:", PANIC_DIM_COLOR);
        panic_hex(regs->esp, buf);
        panic_draw_text(col2_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col2_x, row_y, "EBP:", PANIC_DIM_COLOR);
        panic_hex(regs->ebp, buf);
        panic_draw_text(col2_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col2_x, row_y, "ESI:", PANIC_DIM_COLOR);
        panic_hex(regs->esi, buf);
        panic_draw_text(col2_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col2_x, row_y, "EDI:", PANIC_DIM_COLOR);
        panic_hex(regs->edi, buf);
        panic_draw_text(col2_x + 50, row_y, buf, PANIC_TEXT_COLOR);
        row_y += 20;
        
        panic_draw_text(col2_x, row_y, "EIP:", PANIC_DIM_COLOR);
        panic_hex(regs->eip, buf);
        panic_draw_text(col2_x + 50, row_y, buf, PANIC_ACCENT_COLOR);
        row_y += 20;
        
        panic_draw_text(col2_x, row_y, "EFLAGS:", PANIC_DIM_COLOR);
        panic_hex(regs->eflags, buf);
        panic_draw_text(col2_x + 70, row_y, buf, PANIC_TEXT_COLOR);
        
        // Draw segment registers
        row_y += 30;
        panic_draw_text(col1_x, row_y, "CS:", PANIC_DIM_COLOR);
        panic_hex(regs->cs, buf);
        panic_draw_text(col1_x + 40, row_y, buf, PANIC_TEXT_COLOR);
        
        panic_draw_text(col1_x + 120, row_y, "DS:", PANIC_DIM_COLOR);
        panic_hex(regs->ds, buf);
        panic_draw_text(col1_x + 160, row_y, buf, PANIC_TEXT_COLOR);
        
        panic_draw_text(col1_x + 240, row_y, "SS:", PANIC_DIM_COLOR);
        panic_hex(regs->ss, buf);
        panic_draw_text(col1_x + 280, row_y, buf, PANIC_TEXT_COLOR);
    }
    
    // Draw instructions
    y = box_y + box_h + 40;
    panic_draw_text(cx - 200, y, "Please restart your computer.", PANIC_DIM_COLOR);
    
    // Draw version info at bottom
    panic_draw_text(20, screen_h - 30, "Camel OS v1.0 - Build " __DATE__ " " __TIME__, PANIC_DIM_COLOR);
    
    // Swap buffers to display
    extern void gfx_swap_buffers(void);
    gfx_swap_buffers();
    
    // Halt
    s_printf("\nSystem Halted.\n");
    for(;;) asm("hlt");
}
