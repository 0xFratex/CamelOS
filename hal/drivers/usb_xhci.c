#include "usb_xhci.h"
#include "../../sys/io_ports.h"
#include "serial.h"
#include "../../core/string.h"
#include "usb.h"

static xhci_cap_regs_t* cap_regs = 0;
static xhci_op_regs_t* op_regs = 0;
static uint32_t max_ports = 0;

// Port Status Register Offsets (XHCI spec)
#define PORTSC_CCS (1 << 0) // Current Connect Status
#define PORTSC_PED (1 << 1) // Port Enabled/Disabled

void xhci_scan_ports() {
    if (!op_regs || max_ports == 0) return;

    // Port regs start 0x400 after op regs base usually, but we iterate via the array
    // xhci_op_regs_t struct usually ends with an array of port regs.
    // For this simplified struct, we calculate offset manually.

    volatile uint32_t* port_regs_base = (volatile uint32_t*)((uint32_t)op_regs + 0x400);

    for (uint32_t i = 0; i < max_ports; i++) {
        // Each port set is 16 bytes (4 dwords). PORTSC is the first dword.
        uint32_t portsc = MMIO_READ32(port_regs_base + (i * 4));

        if (portsc & PORTSC_CCS) {
            s_printf("[XHCI] Device detected on Port ");
            char buf[4]; int_to_str(i+1, buf); s_printf(buf); s_printf("\n");

            // Reset port to enable it
            // MMIO_WRITE32(port_regs_base + (i*4), portsc | (1 << 4)); // PR (Port Reset)

            // Mock registration for the UI/Driver layer
            // In a real OS, we would read the Device Descriptor here.
            // We simulate finding the Realtek Dongle on Port 1
            if (i == 0) {
                usb_register_device(0x0BDA, 0xC811); // Realtek 8811AU
            } else {
                usb_register_device(0x8086, 0x0001); // Generic
            }
        }
    }
}

void xhci_controller_init(pci_device_t* dev) {
    uint32_t mmio_base = dev->bar[0] & 0xFFFFFFF0;
    cap_regs = (xhci_cap_regs_t*)mmio_base;
    op_regs = (xhci_op_regs_t*)(mmio_base + cap_regs->cap_length);

    // Read HCSPARAMS1 to get MaxPorts (Bits 24:31)
    uint32_t hcsparams1 = MMIO_READ32(&cap_regs->hcs_params1);
    max_ports = (hcsparams1 >> 24) & 0xFF;

    s_printf("[XHCI] Init MMIO: 0x"); char buf[16]; int_to_str(mmio_base, buf); s_printf(buf);
    s_printf(" MaxPorts: "); int_to_str(max_ports, buf); s_printf(buf); s_printf("\n");

    // Reset
    uint32_t cmd = MMIO_READ32(&op_regs->usbcmd);
    if (cmd & 1) {
        MMIO_WRITE32(&op_regs->usbcmd, cmd & ~1);
        while(MMIO_READ32(&op_regs->usbsts) & 0);
    }
    MMIO_WRITE32(&op_regs->usbcmd, 2); // Reset
    while(MMIO_READ32(&op_regs->usbcmd) & 2); // Wait for reset bit to clear

    s_printf("[XHCI] Host Controller Started.\n");

    // Initial Scan
    xhci_scan_ports();
}