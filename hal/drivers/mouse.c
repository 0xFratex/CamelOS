// hal/drivers/mouse.c
// PS/2 Mouse driver with Intellimouse scroll wheel support
// Includes polling fallback for VirtualBox compatibility
#include "../common/ports.h"
#include "vga.h"
#include "types.h"

// Import screen dimensions from graphics subsystem
extern int screen_w;
extern int screen_h;

// Mouse state
uint8_t mouse_cycle = 0;
int8_t mouse_byte[4];      // 4 bytes for Intellimouse protocol
int mouse_x = 160;
int mouse_y = 100;
int mouse_btn_left = 0;
int mouse_btn_right = 0;
int mouse_btn_middle = 0;
int mouse_scroll_delta = 0;  // Scroll wheel: positive = up, negative = down

// Intellimouse support flag
static uint8_t mouse_has_wheel = 0;

// --- Polling fallback for VirtualBox ---
// VirtualBox's emulated PS/2 mouse may not reliably generate IRQ12 interrupts.
// We track the last time an IRQ-based mouse event was received; if none arrive
// within MOUSE_IRQ_TIMEOUT_MS milliseconds, we switch to polling mode which
// directly reads from the PS/2 data port (like the installer does).
static uint32_t mouse_last_irq_tick = 0;   // Last timer tick when IRQ12 fired
static int mouse_irq_active = 0;            // 1 = IRQ12 events have been seen
static int mouse_poll_mode = 0;             // 1 = currently using polling fallback

#define MOUSE_IRQ_TIMEOUT_MS  500           // Switch to poll after 500ms without IRQ

// Called from mouse_handler() to record IRQ activity
static void mouse_notify_irq(void) {
    extern uint32_t timer_ticks;
    mouse_last_irq_tick = timer_ticks;
    mouse_irq_active = 1;
    mouse_poll_mode = 0;  // IRQ is working, no need to poll
}

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

    // Record that we received an IRQ12 event (for polling fallback detection)
    mouse_notify_irq();

    uint8_t data = inb(0x60);

    // --- Packet Synchronization ---
    // The first byte of a standard PS/2 mouse packet always has Bit 3 set (0x08).
    // If we are at cycle 0 and this bit is missing, we are out of sync.
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return; // Ignore byte, wait for start of next packet
    }

    mouse_byte[mouse_cycle] = data;
    mouse_cycle++;

    // Determine packet length based on wheel support
    uint8_t packet_len = mouse_has_wheel ? 4 : 3;

    if (mouse_cycle >= packet_len) {
        mouse_cycle = 0;

        // Byte 0: Flags
        mouse_btn_left = (mouse_byte[0] & 0x01);
        mouse_btn_right = (mouse_byte[0] & 0x02) >> 1;
        mouse_btn_middle = (mouse_byte[0] & 0x04) >> 2;

        // Byte 0 Check: Overflow bits (X=Bit6, Y=Bit7). If set, discard packet.
        if ((mouse_byte[0] & 0xC0) != 0) return;

        // Byte 1: X Movement
        int8_t rel_x = mouse_byte[1];

        // Byte 2: Y Movement
        int8_t rel_y = mouse_byte[2];

        mouse_x += rel_x;
        mouse_y -= rel_y; // PS/2 Y is positive upwards, screen is positive downwards

        // Byte 3: Scroll wheel (Intellimouse)
        // Accumulate scroll events so rapid scrolling between frames is not lost.
        // mouse_scroll_delta is consumed and cleared each frame by sys_mouse_scroll().
        if (mouse_has_wheel) {
            mouse_scroll_delta += (int8_t)mouse_byte[3];
        }

        // === Use Dynamic Screen Size ===
        int limit_w = (screen_w > 0) ? screen_w : 320;
        int limit_h = (screen_h > 0) ? screen_h : 200;

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= limit_w) mouse_x = limit_w - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= limit_h) mouse_y = limit_h - 1;
    }
}

void init_mouse() {
    // Zero out state
    mouse_btn_left = 0;
    mouse_btn_right = 0;
    mouse_btn_middle = 0;
    mouse_scroll_delta = 0;
    mouse_cycle = 0;
    mouse_has_wheel = 1;  // Default to Intellimouse (4-byte) mode —
                          // all modern emulators (QEMU, VirtualBox, Bochs)
                          // support the scroll wheel protocol.  Prevents
                          // the "scroll sends a click" bug that occurs when
                          // the mouse sends 4-byte packets but the driver
                          // only reads 3 (the extra scroll byte is consumed
                          // as byte 0 of the next packet and can have bit 0
                          // set, causing phantom left-clicks).

    uint8_t _status;
    uint8_t ack __attribute__((unused));

    mouse_wait(1);
    outb(0x64, 0xA8); // Enable Aux

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
    mouse_write(0xFF);
    ack = mouse_read(); // Ack (0xFA)

    // Small delay after reset to let the mouse controller settle
    for (volatile int d = 0; d < 10000; d++) {}

    // --- Enable Intellimouse (scroll wheel) protocol ---
    // Step 1: Set sample rate 200
    mouse_write(0xF3); // Set Sample Rate command
    ack = mouse_read();
    mouse_write(200);  // Sample rate = 200
    ack = mouse_read();

    // Step 2: Set sample rate 100
    mouse_write(0xF3);
    ack = mouse_read();
    mouse_write(100);
    ack = mouse_read();

    // Step 3: Set sample rate 80
    mouse_write(0xF3);
    ack = mouse_read();
    mouse_write(80);
    ack = mouse_read();

    // Step 4: Read device ID — if 0x03, Intellimouse is supported
    mouse_write(0xF2); // Get Device ID
    ack = mouse_read();

    // Small delay before reading device ID to avoid consuming
    // stale bytes from the buffer
    for (volatile int d = 0; d < 5000; d++) {}

    uint8_t device_id = mouse_read();

    if (device_id == 0x03) {
        // Confirmed Intellimouse — wheel already enabled above
        mouse_has_wheel = 1;
    }
    // Even if device_id != 0x03, keep mouse_has_wheel = 1.
    // QEMU/VirtualBox sometimes fail the detection sequence but
    // still send 4-byte packets.  Reading 3 bytes when 4 are
    // sent causes the scroll byte to be misinterpreted as a
    // button click (the "scroll sends click" bug).

    // Set a reasonable sample rate
    mouse_write(0xF3);
    ack = mouse_read();
    mouse_write(100);  // 100 samples/sec
    ack = mouse_read();

    // Enable Streaming
    mouse_write(0xF4);
    ack = mouse_read(); // Ack

    // Unmask IRQ 12 on PIC (Slave)
    uint8_t mask = inb(0xA1);
    outb(0xA1, mask & ~(1 << 4));

    mask = inb(0x21);
    outb(0x21, mask & ~(1 << 2));

    // Initialize polling fallback state
    mouse_irq_active = 0;
    mouse_poll_mode = 0;
    {
        extern uint32_t timer_ticks;
        mouse_last_irq_tick = timer_ticks;
    }
}

// ============================================================================
// mouse_poll_fallback() — Polling fallback for VirtualBox compatibility
//
// VirtualBox's PS/2 mouse emulation sometimes does not reliably deliver
// IRQ12 interrupts. This function is called from the main event loop and
// checks: if no IRQ12 mouse events have been received for a timeout period,
// it directly polls the PS/2 data port (port 0x60/0x64) for mouse packets,
// exactly like the installer's poll_input() does.
//
// This ensures the mouse works in VirtualBox even when IRQ12 is unreliable.
// When IRQ12 events resume (e.g., on real hardware), polling is automatically
// disabled to avoid duplicate processing.
// ============================================================================
void mouse_poll_fallback(void) {
    extern uint32_t timer_ticks;
    uint32_t now = timer_ticks;

    // If we've seen IRQ12 events recently, no need to poll
    if (mouse_irq_active) {
        // timer_ticks increments at ~50 Hz (init_timer(50) in kernel.c)
        // 50 ticks/sec → 500ms = 25 ticks
        uint32_t elapsed = now - mouse_last_irq_tick;
        if (elapsed < 25) {
            return;  // IRQ is active and recent, skip polling
        }
        // No IRQ for 500ms — fall through to polling mode
    }

    // Poll the PS/2 controller for available mouse data
    // Read all available bytes from the output buffer while Bit 0 (OBF) is set
    // and Bit 5 (AUX_OBF) indicates mouse data
    while ((inb(0x64) & 0x21) == 0x21) {  // OBF=1 and AUX_OBF=1
        uint8_t data = inb(0x60);

        // Packet synchronization: byte 0 must have bit 3 set
        if (mouse_cycle == 0 && !(data & 0x08)) {
            continue;  // Out of sync, discard
        }

        mouse_byte[mouse_cycle] = data;
        mouse_cycle++;

        uint8_t packet_len = mouse_has_wheel ? 4 : 3;

        if (mouse_cycle >= packet_len) {
            mouse_cycle = 0;

            // Overflow bits set? Discard packet
            if ((mouse_byte[0] & 0xC0) != 0) continue;

            // Parse buttons
            mouse_btn_left   = (mouse_byte[0] & 0x01);
            mouse_btn_right  = (mouse_byte[0] & 0x02) >> 1;
            mouse_btn_middle = (mouse_byte[0] & 0x04) >> 2;

            // Parse movement
            int8_t rel_x = mouse_byte[1];
            int8_t rel_y = mouse_byte[2];

            mouse_x += rel_x;
            mouse_y -= rel_y;  // PS/2 Y is positive upwards

            // Scroll wheel
            if (mouse_has_wheel) {
                mouse_scroll_delta += (int8_t)mouse_byte[3];
            }

            // Clamp to screen bounds
            int limit_w = (screen_w > 0) ? screen_w : 320;
            int limit_h = (screen_h > 0) ? screen_h : 200;

            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= limit_w) mouse_x = limit_w - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= limit_h) mouse_y = limit_h - 1;

            // Mark that we're in polling mode (no IRQ)
            mouse_poll_mode = 1;
        }
    }
}
