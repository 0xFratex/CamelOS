// hal/cpu/isr.c
#include "isr.h"
#include "../common/ports.h"
#include "../hal/drivers/pci.h"
#include "../hal/drivers/serial.h"
#include "../core/panic.h"

// External Handlers
extern void timer_callback();
extern void keyboard_callback();
extern void mouse_handler();
extern void rtl8139_handler();
extern void rtl8169_handler(); // NEW
extern void page_fault_handler(registers_t regs); // NEW

void isr_handler(registers_t r) {
    // Exceptions (0-31)
    if (r.int_no < 32) {
        // Handle Page Fault specifically (INT 14)
        if (r.int_no == 14) {
            page_fault_handler(r);
            return;
        }

        // Handle Invalid Opcode (INT 6) specifically - this is fatal!
        if (r.int_no == 6) {
            s_printf("\n=== INVALID OPCODE DEBUG ===\n");
            
            // Dump the bytes at EIP
            unsigned char* code = (unsigned char*)r.eip;
            s_printf("Bytes at EIP: ");
            for(int i = 0; i < 16; i++) {
                char hex[4];
                hex[0] = "0123456789ABCDEF"[code[i] >> 4];
                hex[1] = "0123456789ABCDEF"[code[i] & 0xF];
                hex[2] = ' ';
                hex[3] = 0;
                s_printf(hex);
            }
            s_printf("\n");
            
            panic("Invalid Opcode (INT 6)", &r);
            return;
        }

        s_printf("\n[ISR] Exception Int: ");
        char buf[8];
        extern void int_to_str(int, char*);
        int_to_str(r.int_no, buf);
        s_printf(buf);
        s_printf("\n");
        // If critical, hang here
        // asm volatile("hlt");
        return;
    }

    // Network Interrupt
    if (r.int_no == 128) { // 0x80
        rtl8169_handler();
        // EOI handled inside driver or here depending on APIC logic
        // apic_send_eoi() is safer here if shared, but specific driver does it.
        return;
    }

    // Hardware Interrupts
    if (r.int_no >= 32 && r.int_no <= 47) {
        uint32_t irq = r.int_no - 32;

        if (irq == 0) {
            timer_callback(&r);
        }
        else if (irq == 1) {
            keyboard_callback();
        }
        else if (irq == 12) {
            mouse_handler();
        }
        else {
            // Check for PCI devices sharing this IRQ
            // RTL8139
            if (rtl8139_irq_line != 0xFF && irq == rtl8139_irq_line) {
                rtl8139_handler();
            }

            // RTL8169
            if (rtl8169_irq_line != 0xFF && irq == rtl8169_irq_line) {
                rtl8169_handler();
            }

             // Add other PCI handlers (e.g. XHCI) here if they have interrupts enabled
        }

        // APIC EOI
        extern void apic_send_eoi();
        apic_send_eoi();
    }
}
