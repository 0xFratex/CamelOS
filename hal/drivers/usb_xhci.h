#ifndef USB_XHCI_H
#define USB_XHCI_H

#include "pci.h"

// Capability Registers
typedef struct {
    uint8_t cap_length;
    uint8_t reserved;
    uint16_t hci_version;
    uint32_t hcs_params1;
    uint32_t hcs_params2;
    uint32_t hcs_params3;
    uint32_t hcc_params1;
    uint32_t db_off;
    uint32_t run_regs_off;
    uint32_t hcc_params2;
} __attribute__((packed)) xhci_cap_regs_t;

// Operational Registers
typedef struct {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t pagesize;
    volatile uint32_t res1[2];
    volatile uint32_t dnctrl;
    volatile uint32_t crcr_lo;
    volatile uint32_t crcr_hi;
    volatile uint32_t res2[4];
    volatile uint32_t dcbaap_lo;
    volatile uint32_t dcbaap_hi;
    volatile uint32_t config;
} __attribute__((packed)) xhci_op_regs_t;

void xhci_controller_init(pci_device_t* dev);

#endif