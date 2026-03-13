// kernel/panic.h
#ifndef PANIC_H
#define PANIC_H

#include "../hal/drivers/vga.h"

static inline void kernel_halt() {
    asm volatile("cli"); // Disable interrupts
    asm volatile("hlt"); // Halt CPU
    for(;;); // Loop forever if hlt fails
}

static inline void kpanic(const char* reason) {
    vga_set_color(WHITE, RED);
    vga_print("\n\n[!!!] KERNEL PANIC [!!!]\n");
    vga_print("Secure execution environment compromised.\n");
    vga_print("Reason: ");
    vga_print(reason);
    vga_print("\nSystem Halted.");
    kernel_halt();
}

#endif
