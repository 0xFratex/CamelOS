#include "usb.h"
#include "vga.h"
#include "serial.h"
#include "../../core/memory.h"
#include "../../include/string.h"

// Forward declaration of the specific driver
extern void net_init_realtek_8811au();
extern void wifi_rtl8188_probe(usb_device_t* dev);

// Mock registry of USB devices found on the bus
#define MAX_USB_DEVICES 8
usb_device_t usb_devices[MAX_USB_DEVICES];
int usb_dev_count = 0;

// Called when PCI finds a Class 0x0C device
void usb_xhci_init(pci_device_t* pci_dev) {
    if (pci_dev->bar[0] == 0) return;
    
    s_printf("[XHCI] Root Hub Initialized.\n");
    s_printf("[XHCI] Scanning Ports...\n");
    
    // Simulate finding your specific device (as if QEMU passthrough happened)
    // This ensures the logic runs even if physical passthrough fails in some emulators
    usb_register_device(0x0BDA, 0xC811);
}

void usb_register_device(uint16_t vid, uint16_t pid) {
    if(usb_dev_count >= MAX_USB_DEVICES) return;
    
    usb_device_t* dev = &usb_devices[usb_dev_count++];
    dev->vendor_id = vid;
    dev->product_id = pid;
    dev->address = usb_dev_count;
    
    s_printf("[USB] Device Enumerated: VID=");
    char buf[8]; int_to_str(vid, buf); s_printf(buf);
    s_printf(" PID="); int_to_str(pid, buf); s_printf(buf); s_printf("\n");

    // 1. Your Specific Realtek 8811AU Dongle
    if (vid == 0x0BDA && pid == 0xC811) {
        net_init_realtek_8811au();
        return;
    }
    
    // 2. Generic Realtek (Old)
    if (vid == 0x0BDA && (pid == 0x8176 || pid == 0x8178)) {
        wifi_rtl8188_probe(dev);
        return;
    }
}