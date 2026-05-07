// hal/drivers/mouse.c
// PS/2 Mouse driver with Intellimouse scroll wheel support
//
// Architecture:
//   - IRQ12 handler (mouse_handler) pushes raw bytes into a ring buffer.
//     No packet assembly happens in ISR context.
//   - mouse_process() drains the ring buffer only — no direct port polling.
//     This avoids a double-read race where the IRQ handler and the polling
//     path both read the same byte from port 0x60.
//   - The IO-APIC routes GSI 12 → Vector 44 (IRQ12), so the ISR fires
//     even though the legacy PIC has IRQ12 masked.  All mouse bytes are
//     delivered via the ring buffer.
//
// Supports: QEMU, VirtualBox, Bochs, real hardware (PS/2 + USB HID)
#include "../common/ports.h"
#include "vga.h"
#include "types.h"

// Import screen dimensions from graphics subsystem
extern int screen_w;
extern int screen_h;

// ============================================================================
// Mouse state (updated only by mouse_process, read by the rest of the OS)
// ============================================================================
int mouse_x = 160;
int mouse_y = 100;
int mouse_btn_left = 0;
int mouse_btn_right = 0;
int mouse_btn_middle = 0;
int mouse_scroll_delta = 0;  // Scroll wheel: positive = up, negative = down

// ============================================================================
// IRQ ring buffer — mouse_handler() pushes bytes, mouse_process() drains them
// ============================================================================
#define MOUSE_RING_SIZE 64  // Must be power of 2
static volatile uint8_t mouse_ring[MOUSE_RING_SIZE];
static volatile uint32_t mouse_ring_head = 0;  // Written by ISR (producer)
static volatile uint32_t mouse_ring_tail = 0;  // Read by main loop (consumer)

// Intellimouse support flag
static uint8_t mouse_has_wheel = 0;

// ============================================================================
// Packet assembly state (used ONLY by mouse_process in main-loop context)
// ============================================================================
static uint8_t pkt_byte[4];    // Current packet bytes
static uint8_t pkt_cycle = 0;  // How many bytes of current packet received

// ============================================================================
// PS/2 helper functions (used during initialization only)
// ============================================================================

// Wait for the PS/2 controller to be ready.
// type=0: wait until output buffer is full (data available to read)
// type=1: wait until input buffer is empty (safe to send a command)
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

// Send a byte to the mouse via the PS/2 controller (command 0xD4 prefix)
void mouse_write(uint8_t a_write) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, a_write);
}

// Read a response byte from the PS/2 mouse.
// Simple wait-for-OBF + read — same as the installer's ps2_mouse_read().
// Safe during init because init_mouse() runs with cli (no IRQ interference).
uint8_t mouse_read() {
    mouse_wait(0);
    return inb(0x60);
}

// ============================================================================
// mouse_handler() — Called from IRQ12 (ISR context)
//
// Pushes the raw byte into the ring buffer.  All packet parsing happens
// in mouse_process() on the main loop.
// ============================================================================
void mouse_handler() {
    uint8_t status = inb(0x64);

    // Only proceed if the output buffer has mouse data (AUX_OBF = bit 5)
    if (!(status & 0x20)) return;

    uint8_t data = inb(0x60);

    // Push into ring buffer (lock-free single-producer/single-consumer)
    uint32_t next_head = (mouse_ring_head + 1) & (MOUSE_RING_SIZE - 1);
    if (next_head != mouse_ring_tail) {
        mouse_ring[mouse_ring_head] = data;
        mouse_ring_head = next_head;
    }
    // If ring is full, byte is dropped — extremely unlikely at 100 samples/sec
}

// ============================================================================
// mouse_process_packet() — Parse a complete PS/2 mouse packet
//
// Called only from mouse_process() (main-loop context, single-threaded).
// Applies 9-bit sign extension for X/Y deltas per the PS/2 protocol:
//   Byte 0 bit 4 = X sign, bit 5 = Y sign
// ============================================================================
static void mouse_process_packet(void) {
    // Overflow bits set? Discard packet (hardware overflow, data is unreliable)
    if ((pkt_byte[0] & 0xC0) != 0) return;

    // Byte 0: Button flags
    mouse_btn_left   = (pkt_byte[0] & 0x01);
    mouse_btn_right  = (pkt_byte[0] & 0x02) >> 1;
    mouse_btn_middle = (pkt_byte[0] & 0x04) >> 2;

    // Byte 1: X movement — 9-bit signed (sign bit is bit 4 of byte 0)
    int rel_x = pkt_byte[1];
    if (pkt_byte[0] & 0x10) rel_x -= 256;

    // Byte 2: Y movement — 9-bit signed (sign bit is bit 5 of byte 0)
    // PS/2 Y is positive upwards; screen Y is positive downwards → negate
    int rel_y = pkt_byte[2];
    if (pkt_byte[0] & 0x20) rel_y -= 256;

    mouse_x += rel_x;
    mouse_y -= rel_y;

    // Byte 3: Scroll wheel (Intellimouse)
    if (mouse_has_wheel) {
        mouse_scroll_delta += (int8_t)pkt_byte[3];
    }

    // Clamp to screen bounds
    int limit_w = (screen_w > 0) ? screen_w : 320;
    int limit_h = (screen_h > 0) ? screen_h : 200;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= limit_w) mouse_x = limit_w - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= limit_h) mouse_y = limit_h - 1;
}

// ============================================================================
// mouse_process() — Main-loop mouse processing (ring buffer only)
//
// Drains the IRQ ring buffer and assembles complete packets.
// Does NOT directly poll the PS/2 port — all bytes are delivered by
// the IRQ12 handler via the IO-APIC (GSI 12 → Vector 44).
// ============================================================================
void mouse_process(void) {
    uint8_t packet_len = mouse_has_wheel ? 4 : 3;

    // Drain the IRQ ring buffer
    while (mouse_ring_tail != mouse_ring_head) {
        uint8_t data = mouse_ring[mouse_ring_tail];
        mouse_ring_tail = (mouse_ring_tail + 1) & (MOUSE_RING_SIZE - 1);

        // Packet synchronization: byte 0 must have bit 3 set (Always-1 bit)
        if (pkt_cycle == 0 && !(data & 0x08)) continue;

        pkt_byte[pkt_cycle] = data;
        pkt_cycle++;

        if (pkt_cycle >= packet_len) {
            pkt_cycle = 0;
            mouse_process_packet();
        }
    }
}

// ============================================================================
// Legacy API compatibility
// ============================================================================
void mouse_poll_fallback(void) {
    mouse_process();
}

// ============================================================================
// init_mouse() — PS/2 mouse initialization
//
// Matches the installer's proven init_ps2_mouse() sequence exactly.
// Runs with interrupts disabled to prevent IRQ12 from firing during
// the PS/2 command/response exchange.
// ============================================================================
void init_mouse() {
    // Zero out state
    mouse_btn_left = 0;
    mouse_btn_right = 0;
    mouse_btn_middle = 0;
    mouse_scroll_delta = 0;
    pkt_cycle = 0;
    mouse_ring_head = 0;
    mouse_ring_tail = 0;
    mouse_has_wheel = 0;

    // Disable interrupts during the entire init sequence.
    // This prevents the IRQ12 handler (routed via IO-APIC GSI 12)
    // from firing and consuming mouse response bytes during init.
    // The IO-APIC routes GSI 12 even though the legacy PIC has it masked.
    asm volatile("cli");

    uint8_t _status;
    uint8_t ack __attribute__((unused));

    // Enable auxiliary device (mouse port)
    mouse_wait(1);
    outb(0x64, 0xA8);

    // Read and modify Compaq Status Byte:
    //   Set bit 1  = enable IRQ12
    //   Clear bit 5 = enable mouse clock
    mouse_wait(1);
    outb(0x64, 0x20);         // Get Compaq Status
    mouse_wait(0);
    _status = inb(0x60);
    _status |= 2;              // Enable IRQ12
    _status &= ~0x20;          // Enable mouse clock
    mouse_wait(1);
    outb(0x64, 0x60);         // Set Compaq Status
    mouse_wait(1);
    outb(0x60, _status);

    // Reset mouse
    mouse_write(0xFF);
    ack = mouse_read();         // ACK (0xFA)

    // Small delay after reset to let the mouse controller settle
    for (volatile int d = 0; d < 10000; d++) {}

    // Drain any leftover bytes from the reset response
    // (some mice send BAT completion 0xAA + 0x00 after reset)
    for (volatile int d = 0; d < 5000; d++) {}
    while ((inb(0x64) & 0x21) == 0x21) {
        inb(0x60);  // Discard
    }

    // --- Intellimouse (scroll wheel) negotiation ---
    // The magic sequence: Set Sample Rate 200, then 100, then 80.
    // After this, a Get Device ID command returns 0x03 if the mouse
    // supports the scroll wheel protocol (4-byte packets).
    mouse_write(0xF3);    // Set Sample Rate
    ack = mouse_read();    // ACK
    mouse_write(200);     // Rate = 200
    ack = mouse_read();    // ACK

    mouse_write(0xF3);    // Set Sample Rate
    ack = mouse_read();    // ACK
    mouse_write(100);     // Rate = 100
    ack = mouse_read();    // ACK

    mouse_write(0xF3);    // Set Sample Rate
    ack = mouse_read();    // ACK
    mouse_write(80);      // Rate = 80
    ack = mouse_read();    // ACK

    // Get Device ID
    mouse_write(0xF2);    // Get Device ID
    ack = mouse_read();    // ACK (0xFA)

    // Small delay before reading device ID
    for (volatile int d = 0; d < 5000; d++) {}

    uint8_t dev_id = mouse_read();

    if (dev_id == 0x03) {
        mouse_has_wheel = 1;  // Intellimouse: 4-byte packets with scroll wheel
    }
    // If dev_id != 0x03, mouse stays in standard 3-byte mode.
    // Do NOT force 4-byte mode — that causes the "scroll = click" bug.

    // Set a reasonable sample rate
    mouse_write(0xF3);    // Set Sample Rate
    ack = mouse_read();    // ACK
    mouse_write(100);     // 100 samples/sec
    ack = mouse_read();    // ACK

    // Enable data reporting (streaming mode)
    mouse_write(0xF4);    // Enable
    ack = mouse_read();    // ACK

    // Final drain: discard any stale bytes left in the buffer
    for (volatile int d = 0; d < 10000; d++) {}
    while ((inb(0x64) & 0x21) == 0x21) {
        inb(0x60);  // Discard
    }

    // Clear ring buffer and packet assembly state
    mouse_ring_head = 0;
    mouse_ring_tail = 0;
    pkt_cycle = 0;

    // Re-enable interrupts — the IRQ12 handler (via IO-APIC GSI 12)
    // will now deliver mouse bytes to the ring buffer.
    asm volatile("sti");
}
