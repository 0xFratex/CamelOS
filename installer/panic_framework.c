#include "../common/ports.h"
#include "../hal/drivers/vga.h"
#include "../core/string.h"

// --- Serial Debugging (First Line of Defense) ---
#define COM1 0x3F8

void debug_init() {
    outb(COM1 + 1, 0x00); // Disable interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB
    outb(COM1 + 0, 0x03); // 38400 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); // 8 bits, no parity
    outb(COM1 + 2, 0xC7); // FIFO
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

void debug_char(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(COM1, c);
}

void debug_print(const char* str) {
    while (*str) debug_char(*str++);
}

void debug_hex(unsigned int n) {
    char* chars = "0123456789ABCDEF";
    debug_print("0x");
    for (int i = 28; i >= 0; i -= 4) {
        debug_char(chars[(n >> i) & 0xF]);
    }
}

// --- Visual Panic (Red Screen) ---
#include "../hal/video/gfx_hal.h"

void bsod(const char* title, const char* msg, unsigned int code) {
    // 1. Log to Serial (Reliable)
    debug_print("\n[CRITICAL FAILURE] ");
    debug_print(title);
    debug_print(": ");
    debug_print(msg);
    debug_print(" Code: ");
    debug_hex(code);
    debug_print("\n");

    // 2. Visual Dump (If Video Init)
    if (gfx_mem) {
        // Red Screen
        gfx_fill_rect(0, 0, screen_w, screen_h, 0xFF880000);

        // Use the hal string function if linked, or fallback
        // Since we are in installer, we can use gfx_draw_string
        gfx_draw_string(50, 50, "SYSTEM HALTED", 0xFFFFFFFF);
        gfx_draw_string(50, 80, title, 0xFFFFFFFF);
        gfx_draw_string(50, 100, msg, 0xFFFFFFFF);
        
        char code_buf[32];
        // hex to string manual
        char* hex = "0123456789ABCDEF";
        code_buf[0]='E'; code_buf[1]='r'; code_buf[2]='r'; code_buf[3]=':'; code_buf[4]=' '; code_buf[5]='0'; code_buf[6]='x';
        for(int i=0; i<8; i++) code_buf[7+i] = hex[(code >> ((7-i)*4)) & 0xF];
        code_buf[15] = 0;
        
        gfx_draw_string(50, 130, code_buf, 0xFFFFFFFF);
        
        gfx_swap_buffers();
    } else {
        // Text mode fallback?
        // Assuming we are in graphics mode but fb_ptr is null implies bootloader failed.
    }

    // 3. Halt
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}