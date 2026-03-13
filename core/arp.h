#ifndef ARP_H
#define ARP_H

#include "../include/types.h"

#define ARP_CACHE_SIZE 32
#define ARP_TIMEOUT_TICKS (5 * 50) // 5 seconds at 50Hz
#define ARP_RETRY_MAX 3
#define ARP_STALE_TIMEOUT (300 * 50) // 5 minutes at 50Hz

typedef enum {
    ARP_STATE_FREE = 0,
    ARP_STATE_INCOMPLETE,  // Request sent, waiting for reply
    ARP_STATE_COMPLETE,    // Valid mapping
    ARP_STATE_STALE        // Entry expired
} arp_state_t;

typedef struct {
    uint32_t ip_addr;      // Host byte order (little endian)
    uint8_t  mac_addr[6];
    arp_state_t state;
    uint32_t timestamp;    // Last update tick
    uint8_t  retries;
} arp_entry_t;

// Initialize ARP subsystem
void arp_init(void);

// Configure ARP with network settings
void arp_configure(uint32_t local_ip, uint32_t gateway_ip, uint32_t netmask);

// Process incoming ARP packet (called from ethernet layer)
void arp_receive(uint8_t* packet, uint32_t len);

// Resolve IP to MAC (blocks until resolved or timeout)
// Returns 0 on success, fills mac[6]
int arp_resolve(uint32_t ip, uint8_t* mac_out);

// Send ARP request for IP
void arp_send_request(uint32_t target_ip);

// Send ARP reply
void arp_send_reply(uint32_t target_ip, uint8_t* target_mac);

// Check if IP is on local network (uses netmask)
int arp_is_local(uint32_t ip);

// Get gateway IP if IP is not local
uint32_t arp_get_gateway_ip(void);

// Periodic cleanup (call from timer)
void arp_cleanup(void);

// Add static ARP entry (for testing/debugging)
void arp_add_static(uint32_t ip, uint8_t* mac);

#endif
