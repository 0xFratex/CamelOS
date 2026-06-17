// hal/cpu/timer.c
#include "timer.h"
#include "apic.h"
#include "isr.h"
#include "../common/ports.h"
#include "../drivers/serial.h"

// APIC Timer Registers
#define LAPIC_TIMER_LVT 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CURR 0x390
#define LAPIC_TIMER_DIV  0x3E0

volatile uint32_t ticks = 0;
uint32_t ticks_per_ms = 0;
uint32_t timer_ticks = 0;  // Alias for compatibility (used by mouse.c, window_server.c)

// Forward declaration for network polling
extern void rtl8169_poll();

// Forward declaration for ARP cleanup
extern void arp_cleanup(void);

// Forward declarations for scheduler
extern void scheduler_tick(void);
extern uint32_t scheduler_schedule(registers_t* regs);

// NOTE: tcp_retransmit_check() and tcp_process_listeners() are intentionally
// NOT called from this timer IRQ. They were briefly added here to ensure TCP
// maintenance runs in GUI mode, but the TCP code is not reentrancy-safe —
// tcp_send() writes into a per-connection buffer (conn->send_packet) without
// any lock, and if the timer IRQ fired mid-tcp_send() while the browser thread
// was also inside tcp_send() on the same connection, both would corrupt the
// packet buffer and the connection would fail. This was the root cause of the
// "network error" regression.
//
// TCP maintenance is instead driven from http_process_events() in core/http.c,
// which runs in thread context (called from k_connect / k_recvfrom / the
// browser's recv loop). That is safe because there is only one thread doing
// network I/O, so no reentrancy is possible.

// Called from ISR handler (Vector 32)
// Now receives registers pointer for context switching
void timer_callback(registers_t* regs) {
    ticks++;
    timer_ticks++;  // Keep in sync with ticks for compatibility
    
    // Call scheduler tick handler
    scheduler_tick();
    
    // ARP cleanup every second (50 ticks = 1 second at 50Hz)
    static int arp_timer_counter = 0;
    arp_timer_counter++;
    if (arp_timer_counter >= 50) {
        arp_cleanup();
        arp_timer_counter = 0;
    }

    // Poll Network driver occasionally (e.g. every 10ms)
    // if (ticks % 10 == 0) rtl8169_poll();
    
    // Perform scheduling - may modify regs->esp for context switch
    if (regs) {
        scheduler_schedule(regs);
    }
    
    apic_send_eoi(); // Acknowledge APIC after scheduling
}

// Calibrate APIC timer using PIT
void apic_timer_calibrate() {
    s_printf("[TIMER] Calibrating APIC Timer...\n");

    // 1. Prepare PIT for one-shot wait
    // Channel 2, Mode 0, rate generator not needed, just simple gate
    // Actually, simpler to use standard PIT Channel 0 mode 0 wait
    
    // Set APIC Timer to max
    *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_DIV) = 0x03; // Div 16
    *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_INIT) = 0xFFFFFFFF; // Max

    // Wait 10ms using legacy PIT logic
    // PIT runs at 1193182 Hz. 10ms = 11931 ticks
    uint16_t pit_count = 11931;
    outb(0x43, 0x30); // Cmd: Channel 0, Access lo/hi, Mode 0 (Interrupt on term count)
    outb(0x40, pit_count & 0xFF);
    outb(0x40, pit_count >> 8);

    // Spin wait until PIT wraps (Output bit in 0x61 goes high? No, read back)
    // Simplified spin: standard IO wait
    // start_pit removed - simplified calibration uses loop approximation
    // We can't easily read PIT progress without complex logic. 
    // Approximation loop for bare metal calibration:
    for(volatile int i=0; i<1000000; i++); 

    // Stop APIC Timer
    *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_LVT) = 0x10000; // Mask

    uint32_t curr = *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_CURR);
    uint32_t ticks_passed = 0xFFFFFFFF - curr;
    
    // This was roughly 10ms (tuned by loop above or real PIT wait)
    // Let's assume the loop was approx 10ms on modern CPU. 
    // In production, use RTC interrupt for precise calibration.
    
    ticks_per_ms = ticks_passed / 10; 
    s_printf("[TIMER] APIC Calibration done.\n");
}

void init_timer(uint32_t freq) {
    apic_timer_calibrate();

    // Map Vector 32 (IRQ 0 equivalent) to Timer
    // Periodic Mode (Bit 17) | Vector 32
    *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_LVT) = 32 | 0x20000;
    *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_DIV) = 0x03; // Div 16
    
    // Set count for desired frequency
    // Total ticks per second / freq
    uint32_t count = (ticks_per_ms * 1000) / freq;
    *(volatile uint32_t*)(0xFEE00000 + LAPIC_TIMER_INIT) = count;
}

uint32_t get_tick_count() {
    return ticks;
}

void timer_wait(int ms) {
    uint32_t eticks = ticks + (ms / 20); // 50Hz timer = 20ms per tick
    while(ticks < eticks) asm volatile("hlt");
}

// Forward declaration: scheduler_yield forces a context switch via int $32,
// letting other runnable tasks run. Safe to call from thread context.
extern void scheduler_yield(void);

void timer_sleep(int ms) {
    // `ticks` increments at 50Hz (init_timer(50) in kernel.c), so 1 tick = 20ms.
    // Convert the requested milliseconds to a tick count.
    //
    // BUG FIX: Previously this used (ms * ticks_per_ms) / 1000, where
    // ticks_per_ms is the raw APIC counter rate (~62500 under QEMU). That
    // made timer_sleep(1) compute target = ticks + 62, waiting 62 ticks
    // (1.24 seconds!) instead of ~1ms. This made every http_process_events()
    // call take over a second, which starved the DNS polling loop — only
    // 3-5 polls fit in a 2-second window, and DNS responses arrived too
    // late to be matched to the correct query txid.
    //
    // timer_wait() above already uses the correct formula (ms / 20). We
    // use the same approach here, with a minimum of 1 tick so that very
    // short sleeps still yield the CPU for at least one scheduling slice.
    uint32_t tick_wait = (ms <= 20) ? 1 : (ms / 20);
    uint32_t target = ticks + tick_wait;
    // Use signed comparison so a wrap-around of `ticks` doesn't cause us to
    // spin forever. Yield to the scheduler on every iteration so other tasks
    // (and the timer IRQ that eventually advances `ticks`) actually get CPU.
    while ((int32_t)(target - ticks) > 0) {
        scheduler_yield();
    }
}
