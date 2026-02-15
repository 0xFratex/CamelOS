#include "net_if.h"
#include "net.h"
#include "arp.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"

// External declaration of the interface list from net.c
extern net_if_t* if_list;

net_if_t* net_get_by_name(const char* name) {
    net_if_t* iface = if_list;
    while (iface) {
        if (strcmp(iface->name, name) == 0) {
            return iface;
        }
        iface = iface->next;
    }
    return 0;
}

void net_if_set_ip(net_if_t* iface, uint32_t ip, uint32_t gw, uint32_t mask) {
    if (!iface) return;
    iface->ip_addr = ip;
    iface->gateway = gw;
    iface->netmask = mask;
    arp_configure(ip, gw, mask);
}

void net_if_get_mac(uint8_t* mac_out) {
    net_if_t* iface = net_get_default();
    if (iface) {
        memcpy(mac_out, iface->mac, 6);
    } else {
        memset(mac_out, 0, 6);
    }
}

int net_if_send(uint8_t* data, uint32_t len) {
    net_if_t* iface = net_get_default();
    if (!iface || !iface->is_up) return -1;
    return iface->send(iface, data, len);
}

void net_if_receive(uint8_t* data, uint32_t len) {
    // Called by driver to pass received packet to network stack
    net_handle_packet(data, len);
}