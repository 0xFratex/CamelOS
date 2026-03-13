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

// Forward declaration for network polling
extern void rtl8169_poll();

// Forward declaration for ARP cleanup
extern void arp_cleanup(void);

// Forward declarations for scheduler
extern void scheduler_tick(void);
extern uint32_t scheduler_schedule(registers_t* regs);

// Called from ISR handler (Vector 32)
// Now receives registers pointer for context switching
void timer_callback(registers_t* regs) {
    ticks++;
    
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
    uint32_t start_pit = 0;
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
    uint32_t eticks = ticks + (ms / 10); // Assuming 100Hz timer
    while(ticks < eticks) asm volatile("hlt");
}

void timer_sleep(int ms) {
    uint32_t start = ticks;
    uint32_t target = start + (ms * ticks_per_ms) / 1000;
    while (ticks < target) {
        asm volatile("pause");
    }
}
