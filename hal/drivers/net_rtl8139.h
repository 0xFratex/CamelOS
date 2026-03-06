#ifndef NET_RTL8139_H
#define NET_RTL8139_H

#include "pci.h"
#include "../core/net_if.h"

// Device structure
typedef struct {
    uint32_t io_base;
    net_if_t* net_if;
    int initialized;
} rtl8139_dev_t;

extern rtl8139_dev_t rtl_dev;

void rtl8139_init(pci_device_t* pci);
void rtl8139_send_packet(void* data, uint32_t len);
void rtl8139_handler();
void rtl8139_poll(); // <--- ADDED: Polling hook
void rtl8139_configure_ip(uint32_t ip, uint32_t gw, uint32_t mask);

#endif