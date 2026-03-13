#ifndef NET_IF_H
#define NET_IF_H

#include "../include/types.h"
#include "net.h"

// Standard driver return codes
#define NET_TX_OK 0
#define NET_TX_BUSY 1
#define NET_TX_ERROR -1

// Forward declaration
struct net_if_struct;

// Function pointer for sending a raw packet
typedef int (*net_send_func_t)(struct net_if_struct* net_if, uint8_t* data, uint32_t len);

typedef struct net_if_struct {
    char name[16];       // e.g., "eth0", "wlan0"
    uint8_t mac[6];      // Hardware Address

    // IPv4 Configuration
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;

    // IPv6 Configuration
    ipv6_addr_t ipv6_addr;
    ipv6_addr_t ipv6_netmask;
    ipv6_addr_t ipv6_gateway;

    // Flags
    int is_up;
    int is_promiscuous;

    // Statistics
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;

    // Driver Hooks
    net_send_func_t send;
    void* driver_state;  // Private driver data (e.g., PCI device pointer)

    struct net_if_struct* next; // Linked list
} net_if_t;

// Registry Functions
void net_init_subsystem();
void net_register_interface(net_if_t* interface);
net_if_t* net_get_default();
net_if_t* net_get_by_name(const char* name);

// Helper to update global "my_ip" legacy variable based on default interface
void net_update_globals();

// Interface configuration
void net_if_set_ip(net_if_t* iface, uint32_t ip, uint32_t gw, uint32_t mask);
void net_if_get_mac(uint8_t* mac_out);
int net_if_send(uint8_t* data, uint32_t len);
void net_if_receive(uint8_t* data, uint32_t len); // Called by driver

#endif