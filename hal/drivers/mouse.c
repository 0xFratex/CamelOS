// hal/drivers/mouse.c
// PS/2 Mouse driver with Intellimouse scroll wheel support
//
// Architecture:
//   - IRQ12 handler (mouse_handler) pushes raw bytes into a ring buffer.
//     No packet assembly happens in ISR context — this eliminates all race
//     conditions with the main-loop processing code.
//   - mouse_process() (called from the main event loop) drains BOTH the
//     IRQ ring buffer AND the PS/2 port directly (VirtualBox fallback),
//     assembles complete packets, and updates mouse_x/y/buttons/scroll.
//   - Because all packet assembly is single-threaded (main loop only),
//     there are no shared mutable packet-assembly state variables and
//     no need for CLI/STI around the polling code.
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
#define MOUSE_RING_SIZE 64  // Must be power of 2; 64 bytes > 20 packets/sec * 4 bytes
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

// ============================================================================
// mouse_handler() — Called from IRQ12 (ISR context)
//
// Minimal work: just push the raw byte into the ring buffer.
// All packet parsing happens in mouse_process() on the main loop.
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
// mouse_process() — Main-loop mouse processing
//
// Call this once per main-loop iteration. It:
//   1. Drains the IRQ ring buffer (bytes pushed by mouse_handler)
//   2. Directly polls port 0x60/0x64 for any remaining mouse data
//      (VirtualBox fallback — some emulators don't reliably deliver IRQ12)
//   3. Assembles complete packets and updates mouse state
//
// All packet assembly happens here, never in ISR context, so there are
// no race conditions on pkt_byte[] or pkt_cycle.
// ============================================================================
void mouse_process(void) {
    uint8_t packet_len = mouse_has_wheel ? 4 : 3;

    // --- Phase 1: Drain the IRQ ring buffer ---
    while (mouse_ring_tail != mouse_ring_head) {
        uint8_t data = mouse_ring[mouse_ring_tail];
        mouse_ring_tail = (mouse_ring_tail + 1) & (MOUSE_RING_SIZE - 1);

        // Packet synchronization: byte 0 must have bit 3 set
        if (pkt_cycle == 0 && !(data & 0x08)) continue;

        pkt_byte[pkt_cycle] = data;
        pkt_cycle++;

        if (pkt_cycle >= packet_len) {
            pkt_cycle = 0;
            mouse_process_packet();
        }
    }

    // --- Phase 2: Direct PS/2 port polling (VirtualBox fallback) ---
    // If the emulator didn't deliver IRQ12 for some bytes, they'll still
    // be sitting in the PS/2 output buffer.  Read them directly.
    // This is safe because all packet assembly state (pkt_byte, pkt_cycle)
    // is local to this function's calling context (main loop only).
    while ((inb(0x64) & 0x21) == 0x21) {  // OBF=1 and AUX_OBF=1
        uint8_t data = inb(0x60);

        // Packet synchronization
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
// Legacy API compatibility — the main loop and some code reference these names
// ============================================================================
void mouse_poll_fallback(void) {
    mouse_process();
}

// ============================================================================
// init_mouse() — PS/2 mouse initialization
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
    mouse_has_wheel = 0;  // Start with standard 3-byte mode.
                          // Will be set to 1 after successful Intellimouse
                          // detection (device_id == 0x03).  Forcing 4-byte
                          // mode when the mouse only sends 3 bytes causes
                          // the driver to wait for a 4th byte that never
                          // comes — the next packet's byte 0 is consumed as
                          // the scroll byte, misaligning all subsequent
                          // packets and causing phantom clicks and teleportation.

    uint8_t _status;
    uint8_t ack __attribute__((unused));

    mouse_wait(1);
    outb(0x64, 0xA8); // Enable Aux

    // Drain any stale data from the PS/2 buffer before we start
    for (volatile int d = 0; d < 1000; d++) {}
    while (inb(0x64) & 0x01) {
        inb(0x60);  // Discard
    }

    mouse_wait(1);
    outb(0x64, 0x20); // Get Compaq Status Byte
    mouse_wait(0);
    _status = inb(0x60);

    _status |= 2;     // Enable IRQ 12
    _status &= ~0x20; // Enable mouse clock

    mouse_wait(1);
    outb(0x64, 0x60); // Set Compaq Status
    mouse_wait(1);
    outb(0x60, _status);

    // Reset Mouse
    // The PS/2 mouse responds to 0xFF with THREE bytes:
    //   0xFA = ACK
    //   0xAA = BAT (Basic Assurance Test) completion
    //   0x00 = Default Device ID
    // We MUST read all three.  Leaving stale bytes in the buffer
    // shifts all subsequent command/response reads, causing the
    // Intellimouse detection to fail and producing phantom clicks
    // and cursor teleportation.
    mouse_write(0xFF);
    ack = mouse_read();   // 0xFA — ACK
    ack = mouse_read();   // 0xAA — BAT complete
    ack = mouse_read();   // 0x00 — Default Device ID

    // Drain any extra leftover bytes (some controllers send additional data)
    for (volatile int d = 0; d < 5000; d++) {}
    while ((inb(0x64) & 0x21) == 0x21) {
        inb(0x60);  // Discard
    }

    // --- Enable Intellimouse (scroll wheel) protocol ---
    // The magic sequence: Set Sample Rate 200, then 100, then 80.
    // After this, a Get Device ID command returns 0x03 if the mouse
    // supports the scroll wheel protocol (4-byte packets).

    // Step 1: Set sample rate 200
    mouse_write(0xF3); // Set Sample Rate command
    ack = mouse_read(); // ACK
    mouse_write(200);  // Sample rate = 200
    ack = mouse_read(); // ACK

    // Step 2: Set sample rate 100
    mouse_write(0xF3);
    ack = mouse_read(); // ACK
    mouse_write(100);
    ack = mouse_read(); // ACK

    // Step 3: Set sample rate 80
    mouse_write(0xF3);
    ack = mouse_read(); // ACK
    mouse_write(80);
    ack = mouse_read(); // ACK

    // Step 4: Read device ID — if 0x03, Intellimouse is supported
    // The mouse responds to 0xF2 with: ACK (0xFA) + Device ID byte
    mouse_write(0xF2);       // Get Device ID
    ack = mouse_read();      // 0xFA — ACK
    uint8_t device_id = mouse_read();  // Device ID (0x00=standard, 0x03=Intellimouse)

    if (device_id == 0x03) {
        // Confirmed Intellimouse — wheel enabled, 4-byte packets
        mouse_has_wheel = 1;
    }
    // If device_id != 0x03, the mouse stays in standard 3-byte
    // mode.  We MUST keep mouse_has_wheel = 0 — reading 4 bytes
    // from a 3-byte mouse causes the driver to consume byte 0 of
    // the next packet as the "scroll" byte, misaligning everything.

    // Set a reasonable sample rate
    mouse_write(0xF3);
    ack = mouse_read(); // ACK
    mouse_write(100);  // 100 samples/sec
    ack = mouse_read(); // ACK

    // Enable Streaming
    mouse_write(0xF4);
    ack = mouse_read(); // ACK

    // Final drain: discard any stale bytes left in the buffer.
    // Without this, leftover ACK/ID bytes from the init sequence
    // are misinterpreted as mouse packets, causing teleportation
    // and phantom clicks on the very first mouse_process() calls.
    for (volatile int d = 0; d < 10000; d++) {}
    while ((inb(0x64) & 0x21) == 0x21) {
        inb(0x60);  // Discard
    }

    // Clear ring buffer and packet assembly state one more time
    // in case IRQ12 fired during init and pushed stale data
    mouse_ring_head = 0;
    mouse_ring_tail = 0;
    pkt_cycle = 0;

    // Unmask IRQ 12 on PIC (Slave)
    uint8_t mask = inb(0xA1);
    outb(0xA1, mask & ~(1 << 4));

    mask = inb(0x21);
    outb(0x21, mask & ~(1 << 2));
}
