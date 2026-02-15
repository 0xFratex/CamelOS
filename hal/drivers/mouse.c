// hal/drivers/mouse.c
#include "../common/ports.h"
#include "vga.h"

// Import screen dimensions from graphics subsystem
extern int screen_w;
extern int screen_h;

// Define types manually for bare-metal
typedef unsigned char uint8_t;
typedef signed char int8_t;

// Mouse state
uint8_t mouse_cycle = 0;
int8_t mouse_byte[3]; 
int mouse_x = 160;    
int mouse_y = 100;    
int mouse_btn_left = 0;
int mouse_btn_right = 0;

void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) {
            if ((inb(0x64) & 1) == 1) return;
        }
    } else {
        while (timeout--) {
            if ((inb(0x64) & 2) == 0) return;
        }
    }
}

void mouse_write(uint8_t a_write) {
    mouse_wait(1);
    outb(0x64, 0xD4); 
    mouse_wait(1);
    outb(0x60, a_write);
}

uint8_t mouse_read() {
    mouse_wait(0);
    return inb(0x60);
}

void mouse_handler() {
    uint8_t status = inb(0x64);

    // Check if the buffer actually has mouse data (Bit 5 is set for Aux device)
    if (!(status & 0x20)) return;

    uint8_t data = inb(0x60);

    // --- FIX: Packet Synchronization ---
    // The first byte of a standard PS/2 mouse packet always has Bit 3 set (0x08).
    // If we are at cycle 0 and this bit is missing, we are out of sync.
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return; // Ignore byte, wait for start of next packet
    }

    mouse_byte[mouse_cycle] = data;
    mouse_cycle++;

    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        // Byte 0: Flags
        mouse_btn_left = (mouse_byte[0] & 0x01);
        mouse_btn_right = (mouse_byte[0] & 0x02) >> 1;

        // Byte 0 Check: Overflow bits (X=Bit6, Y=Bit7). If set, discard packet to prevent huge jumps.
        if ((mouse_byte[0] & 0xC0) != 0) return;

        // Byte 1: X Movement
        int8_t rel_x = mouse_byte[1];

        // Byte 2: Y Movement
        int8_t rel_y = mouse_byte[2];

        mouse_x += rel_x;
        mouse_y -= rel_y; // PS/2 Y is positive upwards, screen is positive downwards

        // === FIX: Use Dynamic Screen Size ===
        // Use 320x200 as safe fallback if screen vars are 0 (during boot)
        int limit_w = (screen_w > 0) ? screen_w : 320;
        int limit_h = (screen_h > 0) ? screen_h : 200;

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= limit_w) mouse_x = limit_w - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= limit_h) mouse_y = limit_h - 1;
    }
}

void init_mouse() {
    // FIX: Explicitly zero out buttons to prevent ghost clicks on startup
    mouse_btn_left = 0;
    mouse_btn_right = 0;
    mouse_cycle = 0;

    uint8_t _status;

    mouse_wait(1);
    outb(0x64, 0xA8); // Enable Aux

    mouse_wait(1);
    outb(0x64, 0x20); // Get Compaq Status Byte
    mouse_wait(0);
    _status = inb(0x60);

    _status |= 2; // Enable IRQ 12
    _status &= ~0x20; // Disable Mouse Clock (Clear bit 5) to enable it? No, clear disables disable.

    mouse_wait(1);
    outb(0x64, 0x60); // Set Compaq Status
    mouse_wait(1);
    outb(0x60, _status);

    // Reset Mouse
    mouse_write(0xFF);
    mouse_read(); // Ack

    // Enable Streaming
    mouse_write(0xF4);
    mouse_read(); // Ack

    // Unmask IRQ 12 on PIC (Slave)
    // Master PIC (0x21) bit 2 (Cascade) must be 0
    // Slave PIC (0xA1) bit 4 (IRQ 12) must be 0
    uint8_t mask = inb(0xA1);
    outb(0xA1, mask & ~(1 << 4));

    mask = inb(0x21);
    outb(0x21, mask & ~(1 << 2));
}
