// hal/cpu/idt.c
#include "idt.h"
#include "../common/ports.h"
#include "../core/memory.h"

// Define types
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

// Assembly tables
extern uint32_t isr_stub_table[];
extern uint32_t irq_stub_table[];
extern void isr128(); // Declaration from ASM
extern void rtl8169_handler(); // Declaration from Driver

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = (base & 0xFFFF);
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].type_attr = flags;
    idt[num].offset_high = (base >> 16) & 0xFFFF;
}

void init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;

    memset(&idt, 0, sizeof(struct idt_entry) * 256);

    // 1. Remap PIC (IRQ 0-7 -> 32-39, IRQ 8-15 -> 40-47)
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    
    // 2. Mask Interrupts (Enable All for now to ensure NIC works)
    // 0x00 = Enable All
    outb(0x21, 0x00); 
    outb(0xA1, 0x00);

    // 3. Install Exception Handlers (0-31)
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    }

    // 4. Install IRQ Handlers (32-47)
    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_stub_table[i], 0x08, 0x8E);
    }

    // Register Network Interrupt (Vector 0x80 / 128)
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0x8E);

    // 5. Load IDT
    asm volatile ("lidt %0" : : "m" (idtp));
    asm volatile ("sti"); 
}