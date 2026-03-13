#ifndef NET_RTL8169_H
#define NET_RTL8169_H

#include "pci.h"
#include "../../include/types.h"

// Descriptor Structure (16 bytes)
typedef struct {
    uint32_t cmd_status;
    uint32_t vlan_tag;
    uint32_t buf_addr_lo;
    uint32_t buf_addr_hi;
} __attribute__((packed)) rtl8169_desc_t;

// Public Functions
void rtl8169_init(pci_device_t* pci);
void rtl8169_handler();
void rtl8169_poll(); // <--- ADD THIS

#endif
