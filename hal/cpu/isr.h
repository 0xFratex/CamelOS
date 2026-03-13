// cpu/isr.h
#ifndef ISR_H
#define ISR_H

#include "../common/ports.h"

// Define types manually since we're using -nostdinc
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

// Registers struct must match the 'push' order in system_entry.asm
// Stack grows downwards, so the last pushed item is at offset 0.
typedef struct {
    // Pushed by us (Segs)
    uint32_t gs, fs, es, ds;
    // Pushed by pusha
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    // Pushed by ISR/IRQ macro
    uint32_t int_no, err_code;
    // Pushed by CPU automatically
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

// Function declarations
void isr_handler(registers_t r);
void isr_install_handlers();
void panic(const char* msg, registers_t* regs);

#endif
