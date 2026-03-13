#ifndef USB_H
#define USB_H

#include "pci.h"

// Standard USB Descriptor Types
#define USB_DESC_DEVICE 1
#define USB_DESC_CONFIG 2
#define USB_DESC_STRING 3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT 5

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    void* controller;
    int address;
} usb_device_t;

// The specific function to initialize the controller found by PCI
void usb_xhci_init(pci_device_t* pci_dev);

// Helper to register a found USB device (called by controller scanner)
void usb_register_device(uint16_t vid, uint16_t pid);

#endif