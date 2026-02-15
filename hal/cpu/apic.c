// hal/cpu/apic.c
#include "apic.h"
#include "idt.h"
#include "paging.h"
#include "isr.h"
#include "../common/ports.h"
#include "../drivers/serial.h"
#include "../../core/memory.h"

// --- Configuration ---
#define LAPIC_BASE      0xFEE00000
#define IOAPIC_BASE     0xFEC00000

// --- Local APIC Registers ---
#define LAPIC_ID        0x0020
#define LAPIC_VER       0x0030
#define LAPIC_TPR       0x0080
#define LAPIC_EOI       0x00B0
#define LAPIC_SVR       0x00F0
#define LAPIC_ESR       0x0280
#define LAPIC_ICR_LO    0x0300
#define LAPIC_ICR_HI    0x0310
#define LAPIC_TIMER     0x0320
#define LAPIC_TICR      0x0380 // Initial Count
#define LAPIC_TCCR      0x0390 // Current Count
#define LAPIC_TDCR      0x03E0 // Divide Config

// --- IO APIC Registers ---
#define IOAPICID        0x00
#define IOAPICVER       0x01
#define IOAPICARB       0x02
#define IOREDTBL        0x10 // + 2 * index

// --- MSRs ---
#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800

// --- Utils ---

static void cpu_set_msr(uint32_t msr, uint32_t lo, uint32_t hi) {
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static void cpu_get_msr(uint32_t msr, uint32_t *lo, uint32_t *hi) {
    asm volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static uint32_t lapic_read(uint32_t reg) {
    return *((volatile uint32_t*)(LAPIC_BASE + reg));
}

static void lapic_write(uint32_t reg, uint32_t value) {
    *((volatile uint32_t*)(LAPIC_BASE + reg)) = value;
}

// IO-APIC access is indirect via (Index, Data) registers
static uint32_t ioapic_read(uint32_t reg) {
    volatile uint32_t* io_reg = (volatile uint32_t*)IOAPIC_BASE;
    volatile uint32_t* io_data = (volatile uint32_t*)(IOAPIC_BASE + 0x10);
    *io_reg = reg;
    return *io_data;
}

static void ioapic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t* io_reg = (volatile uint32_t*)IOAPIC_BASE;
    volatile uint32_t* io_data = (volatile uint32_t*)(IOAPIC_BASE + 0x10);
    *io_reg = reg;
    *io_data = value;
}

// Map a Global System Interrupt (GSI) to a CPU Vector (IDT Entry)
void ioapic_set_gsi_redirect(uint8_t gsi, uint8_t vector, uint8_t cpu_apic_id, int active_low, int level_trigger) {
    uint32_t low_index = IOREDTBL + (gsi * 2);
    uint32_t high_index = IOREDTBL + (gsi * 2) + 1;

    uint32_t high = (uint32_t)cpu_apic_id << 24;
    
    uint32_t low = vector;
    low |= (0 << 8);  // Delivery Mode: Fixed
    low |= (0 << 11); // Destination Mode: Physical
    
    if (active_low) low |= (1 << 13); // Polarity
    if (level_trigger) low |= (1 << 15); // Trigger Mode
    
    low |= (0 << 16); // Mask: 0 = Enabled

    ioapic_write(high_index, high);
    ioapic_write(low_index, low);
    
    s_printf("[APIC] Route GSI "); 
    char buf[8]; extern void int_to_str(int, char*); int_to_str(gsi, buf); s_printf(buf);
    s_printf(" -> Vector "); int_to_str(vector, buf); s_printf(buf); s_printf("\n");
}

void apic_send_eoi() {
    lapic_write(LAPIC_EOI, 0);
}

// --- Initialization ---

void init_apic() {
    s_printf("[APIC] Initializing...\n");

    // 1. Map MMIO Pages (Important!)
    // We explicitly map the 4KB pages for LAPIC and IOAPIC
    paging_map_region(LAPIC_BASE, LAPIC_BASE, 4096, 0x03); // RW, Supervisor
    paging_map_region(IOAPIC_BASE, IOAPIC_BASE, 4096, 0x03);

    // 2. Disable Legacy PIC
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    // 3. Enable Local APIC via MSR
    uint32_t lo, hi;
    cpu_get_msr(IA32_APIC_BASE_MSR, &lo, &hi);
    if (!(lo & IA32_APIC_BASE_MSR_ENABLE)) {
        s_printf("[APIC] Enabling via MSR...\n");
        lo |= IA32_APIC_BASE_MSR_ENABLE;
        lo &= ~0xFFFFF000; // Clear Base
        lo |= LAPIC_BASE;  // Set Base
        cpu_set_msr(IA32_APIC_BASE_MSR, lo, hi);
    }

    // 4. Enable Local APIC via SVR (Spurious Vector Register)
    // Map Spurious Interrupt to Vector 0xFF, set Bit 8 to Enable
    lapic_write(LAPIC_SVR, 0x1FF);

    // 5. Initialize Timer (One-shot for calibration, handled in timer.c)
    lapic_write(LAPIC_TDCR, 0x03); // Divide by 16
    lapic_write(LAPIC_TIMER, 0x10000); // Masked initially

    // 6. Set TPR to 0 to accept all interrupts
    lapic_write(LAPIC_TPR, 0);

    // 7. Route standard ISA IRQs (0-15) via IO-APIC to IDT 32-47
    // This maintains compatibility with keyboard (IRQ 1) etc.
    // Note: On some systems, IRQ 0 (PIT) is GSI 2.
    for (int i = 0; i < 16; i++) {
        // Identity map ISA IRQs to Vectors 32-47
        // GSI i -> Vector 32+i
        ioapic_set_gsi_redirect(i, 32 + i, 0, 0, 0); 
    }
    
    // Explicitly unmask Keyboard (GSI 1)
    ioapic_set_gsi_redirect(1, 33, 0, 0, 0);

    s_printf("[APIC] Initialization Complete.\n");
}