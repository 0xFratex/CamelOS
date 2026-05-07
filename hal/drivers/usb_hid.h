// hal/drivers/usb_hid.h - USB HID Boot Protocol Driver for CamelOS
// Provides keyboard and mouse support via USB HID boot protocol
// Enables operation on USB-only hardware (no PS/2 required)

#ifndef USB_HID_H
#define USB_HID_H

#include "pci.h"
#include "../../include/types.h"

// HID device types
#define HID_DEVICE_KEYBOARD     1
#define HID_DEVICE_MOUSE        2
#define HID_DEVICE_OTHER        3

// HID device state
typedef struct {
    int active;                  // 1 if device is configured and polling
    uint8_t slot_id;             // xHCI slot ID
    uint8_t interface_num;       // USB interface number
    uint8_t protocol;            // 1=Keyboard, 2=Mouse, 0=Other
    uint8_t subclass;            // 1=Boot subclass
    uint8_t endpoint_addr;       // Interrupt IN endpoint address
    uint8_t endpoint_interval;   // Polling interval in frames
    uint16_t max_packet_size;    // Maximum packet size for interrupt EP
    uint16_t vendor_id;          // USB Vendor ID
    uint16_t product_id;         // USB Product ID
} hid_device_t;

// Initialize the USB HID subsystem
// Must be called after xHCI controller init and USB enumeration
void usb_hid_init(void);

// Poll all active HID devices for input reports
// Should be called from the main event loop (or timer ISR)
void usb_hid_poll(void);

// Get the list of discovered HID devices
hid_device_t* usb_hid_get_devices(int* count);

#endif /* USB_HID_H */
