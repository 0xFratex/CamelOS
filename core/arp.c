#include "arp.h"
#include "net.h"
#include "net_if.h"
#include "../hal/drivers/serial.h"
#include "../core/string.h"
#include "../core/memory.h"
#include "../hal/cpu/timer.h"

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint32_t arp_gateway_ip = 0;
static uint32_t local_ip = 0;
static uint32_t netmask = 0xFFFFFF00; // 255.255.255.0 default

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    s_printf("[ARP] Cache initialized (");
    char buf[16];
    int_to_str(ARP_CACHE_SIZE, buf);
    s_printf(buf);
    s_printf(" entries)\n");
}

void arp_configure(uint32_t ip, uint32_t gw, uint32_t nm) {
    local_ip = ip;
    arp_gateway_ip = gw;
    netmask = nm;
    
    char buf[16];
    s_printf("[ARP] Config: Local=");
    ip_to_str(ip, buf); s_printf(buf);
    s_printf(" GW=");
    ip_to_str(gw, buf); s_printf(buf);
    s_printf("\n");
    
    // Add gateway to cache immediately (will trigger ARP request)
    if (gw != 0) {
        arp_send_request(gw);
    }
}

int arp_is_local(uint32_t ip) {
    return ((ip & netmask) == (local_ip & netmask));
}

uint32_t arp_get_gateway_ip(void) {
    return arp_gateway_ip;
}

// Find entry by IP
static arp_entry_t* arp_find_entry(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state != ARP_STATE_FREE && 
            arp_cache[i].ip_addr == ip) {
            return &arp_cache[i];
        }
    }
    return 0;
}

// Allocate new entry or find existing
static arp_entry_t* arp_alloc_entry(uint32_t ip) {
    // Check if exists
    arp_entry_t* existing = arp_find_entry(ip);
    if (existing) return existing;
    
    // Find empty slot
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state == ARP_STATE_FREE) {
            arp_cache[i].ip_addr = ip;
            arp_cache[i].state = ARP_STATE_INCOMPLETE;
            arp_cache[i].timestamp = get_tick_count();
            arp_cache[i].retries = 0;
            return &arp_cache[i];
        }
    }
    
    // Evict oldest STALE entry
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state == ARP_STATE_STALE) {
            arp_cache[i].ip_addr = ip;
            arp_cache[i].state = ARP_STATE_INCOMPLETE;
            arp_cache[i].timestamp = get_tick_count();
            arp_cache[i].retries = 0;
            return &arp_cache[i];
        }
    }
    
    return 0; // Cache full
}

void arp_add_static(uint32_t ip, uint8_t* mac) {
    // First check if entry already exists
    arp_entry_t* entry = arp_find_entry(ip);
    if (!entry) {
        entry = arp_alloc_entry(ip);
    }
    if (entry) {
        memcpy(entry->mac_addr, mac, 6);
        entry->state = ARP_STATE_COMPLETE;
        entry->timestamp = get_tick_count();
        entry->retries = 0;
        
        char ip_str[16], mac_str[18];
        ip_to_str(ip, ip_str);
        mac_to_str(mac, mac_str);
        s_printf("[ARP] Static: ");
        s_printf(ip_str);
        s_printf(" -> ");
        s_printf(mac_str);
        s_printf("\n");
    }
}

void arp_send_request(uint32_t target_ip) {
    uint8_t packet[42]; // 14 eth + 28 arp
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    net_if_t* iface = net_get_default();
    if (!iface) return;
    
    // Ethernet header
    memcpy(packet, broadcast_mac, 6);           // Dest MAC
    memcpy(packet + 6, iface->mac, 6);          // Source MAC
    packet[12] = 0x08;                          // EtherType ARP (0x0806)
    packet[13] = 0x06;
    
    // ARP packet (28 bytes)
    packet[14] = 0x00; packet[15] = 0x01;       // Hardware type: Ethernet
    packet[16] = 0x08; packet[17] = 0x00;       // Protocol type: IPv4
    packet[18] = 0x06;                          // Hardware size
    packet[19] = 0x04;                          // Protocol size
    packet[20] = 0x00; packet[21] = 0x01;       // Opcode: Request
    
    // Sender MAC
    memcpy(packet + 22, iface->mac, 6);
    // Sender IP (local_ip is in network byte order, extract bytes MSB first)
    packet[28] = (local_ip >> 24) & 0xFF;  // First octet
    packet[29] = (local_ip >> 16) & 0xFF;  // Second octet
    packet[30] = (local_ip >> 8) & 0xFF;   // Third octet
    packet[31] = local_ip & 0xFF;          // Fourth octet
    // Target MAC (unknown)
    memset(packet + 32, 0, 6);
    // Target IP (target_ip is in network byte order, extract bytes MSB first)
    packet[38] = (target_ip >> 24) & 0xFF;  // First octet
    packet[39] = (target_ip >> 16) & 0xFF;  // Second octet
    packet[40] = (target_ip >> 8) & 0xFF;   // Third octet
    packet[41] = target_ip & 0xFF;          // Fourth octet
    
    iface->send(iface, packet, 42);
    
    char buf[16];
    s_printf("[ARP] Request sent for ");
    ip_to_str(target_ip, buf); s_printf(buf); s_printf("\n");
}

void arp_send_reply(uint32_t target_ip, uint8_t* target_mac) {
    uint8_t packet[42];
    
    net_if_t* iface = net_get_default();
    if (!iface) return;
    
    // Ethernet header
    memcpy(packet, target_mac, 6);
    memcpy(packet + 6, iface->mac, 6);
    packet[12] = 0x08; packet[13] = 0x06;
    
    // ARP packet
    packet[14] = 0x00; packet[15] = 0x01;
    packet[16] = 0x08; packet[17] = 0x00;
    packet[18] = 0x06; packet[19] = 0x04;
    packet[20] = 0x00; packet[21] = 0x02;       // Opcode: Reply
    
    memcpy(packet + 22, iface->mac, 6);              // Sender MAC
    // Sender IP (local_ip is in network byte order, extract bytes MSB first)
    packet[28] = (local_ip >> 24) & 0xFF;
    packet[29] = (local_ip >> 16) & 0xFF;
    packet[30] = (local_ip >> 8) & 0xFF;
    packet[31] = local_ip & 0xFF;
    memcpy(packet + 32, target_mac, 6);              // Target MAC
    // Target IP (target_ip is in network byte order, extract bytes MSB first)
    packet[38] = (target_ip >> 24) & 0xFF;
    packet[39] = (target_ip >> 16) & 0xFF;
    packet[40] = (target_ip >> 8) & 0xFF;
    packet[41] = target_ip & 0xFF;
    
    iface->send(iface, packet, 42);
}

// Called when ARP packet received
void arp_receive(uint8_t* packet, uint32_t len) {
    if (len < 42) return;
    
    // ARP Packet Structure (after 14-byte Ethernet header):
    // Offset 14: Hardware Type (2 bytes)
    // Offset 16: Protocol Type (2 bytes)  
    // Offset 18: Hardware Addr Len (1 byte) = 6
    // Offset 19: Protocol Addr Len (1 byte) = 4
    // Offset 20: Opcode (2 bytes)
    // Offset 22: Sender MAC (6 bytes)
    // Offset 28: Sender IP (4 bytes) - BIG ENDIAN (network byte order)
    // Offset 32: Target MAC (6 bytes)
    // Offset 38: Target IP (4 bytes) - BIG ENDIAN (network byte order)
    
    uint16_t opcode = (packet[20] << 8) | packet[21];
    
    // Read sender MAC and IP
    uint8_t* sender_mac = packet + 22;
    // FIX: Read IP bytes in network byte order (big endian)
    // packet[28] is most significant byte (first octet of IP)
    uint32_t sender_ip = ((uint32_t)packet[28] << 24) | ((uint32_t)packet[29] << 16) |
                         ((uint32_t)packet[30] << 8) | ((uint32_t)packet[31]);
    
    s_printf("[ARP] DEBUG raw bytes 22-27: ");
    for(int i=22; i<=27; i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[packet[i] >> 4];
        hex[1] = "0123456789ABCDEF"[packet[i] & 0xF];
        hex[2] = 0;
        s_printf(hex);
        if(i<27) s_printf(" ");
    }
    s_printf("\n");
    
    s_printf("[ARP] DEBUG raw bytes 28-31: ");
    for(int i=28; i<=31; i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[packet[i] >> 4];
        hex[1] = "0123456789ABCDEF"[packet[i] & 0xF];
        hex[2] = 0;
        s_printf(hex);
        if(i<31) s_printf(" ");
    }
    s_printf("\n");
    
    char buf[16];
    s_printf("[ARP] RX opcode=");
    int_to_str(opcode, buf); s_printf(buf);
    s_printf(" from ");
    // sender_ip is already in network byte order, ip_to_str expects that
    ip_to_str(sender_ip, buf); s_printf(buf);
    
    // Debug: print raw MAC bytes
    s_printf(" MAC=");
    for(int i=0; i<6; i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[sender_mac[i] >> 4];
        hex[1] = "0123456789ABCDEF"[sender_mac[i] & 0xF];
        hex[2] = 0;
        s_printf(hex);
        if(i<5) s_printf(":");
    }
    s_printf("\n");
    
    // Update cache
    arp_entry_t* entry = arp_find_entry(sender_ip);
    if (!entry) entry = arp_alloc_entry(sender_ip);
    
    if (entry) {
        memcpy(entry->mac_addr, sender_mac, 6);  // Copy 6 bytes MAC
        entry->state = ARP_STATE_COMPLETE;
        entry->timestamp = get_tick_count();
        
        s_printf("[ARP] Cached successfully\n");
    }
    
    // Reply if request for us
    if (opcode == 1) {
        // FIX: Read target IP bytes in network byte order
        uint32_t target_ip = ((uint32_t)packet[38] << 24) | ((uint32_t)packet[39] << 16) |
                             ((uint32_t)packet[40] << 8) | ((uint32_t)packet[41]);
        if (target_ip == local_ip) {
            arp_send_reply(sender_ip, sender_mac);
        }
    }
}

// Resolve IP to MAC (blocking with timeout)
int arp_resolve(uint32_t ip, uint8_t* mac_out) {
    // Check cache first - look for COMPLETE entries
    arp_entry_t* entry = arp_find_entry(ip);
    
    if (entry && entry->state == ARP_STATE_COMPLETE) {
        // Check if stale (> 5 minutes)
        if (get_tick_count() - entry->timestamp > ARP_STALE_TIMEOUT) {
            entry->state = ARP_STATE_STALE;
        } else {
            memcpy(mac_out, entry->mac_addr, 6);
            return 0; // Success
        }
    }
    
    // If not local network, resolve gateway instead
    uint32_t target_ip = ip;
    if (!arp_is_local(ip)) {
        target_ip = arp_gateway_ip;
        entry = arp_find_entry(target_ip);
        if (entry && entry->state == ARP_STATE_COMPLETE) {
            memcpy(mac_out, entry->mac_addr, 6);
            return 0;
        }
    }
    
    // Allocate entry if needed
    if (!entry) {
        entry = arp_alloc_entry(target_ip);
    }
    
    if (!entry) {
        return -1; // Cache full
    }
    
    // Send ARP request and wait for response
    if (entry->retries < ARP_RETRY_MAX) {
        arp_send_request(target_ip);
        entry->retries++;
        
        // Poll for response with timeout (100 ticks = ~2 seconds)
        uint32_t start = get_tick_count();
        while (get_tick_count() - start < 100) {
            extern void rtl8139_poll(void);
            rtl8139_poll();  // Process incoming packets
            
            if (entry->state == ARP_STATE_COMPLETE) {
                memcpy(mac_out, entry->mac_addr, 6);
                return 0; // Success
            }
            
            // Small delay
            for(volatile int i = 0; i < 1000; i++) asm volatile("pause");
        }
    }
    
    return -1; // Failed or timeout
}

// Periodic cleanup (call from timer)
void arp_cleanup(void) {
    uint32_t now = get_tick_count();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state == ARP_STATE_INCOMPLETE) {
            // Retry incomplete entries
            if (now - arp_cache[i].timestamp > 50) { // 1 second timeout
                if (arp_cache[i].retries < ARP_RETRY_MAX) {
                    arp_send_request(arp_cache[i].ip_addr);
                    arp_cache[i].timestamp = now;
                    arp_cache[i].retries++;
                } else {
                    arp_cache[i].state = ARP_STATE_FREE;
                    s_printf("[ARP] Resolution failed for entry ");
                    char buf[16];
                    int_to_str(i, buf); s_printf(buf);
                    s_printf("\n");
                }
            }
        } else if (arp_cache[i].state == ARP_STATE_COMPLETE) {
            if (now - arp_cache[i].timestamp > ARP_STALE_TIMEOUT) { // 5 minutes
                arp_cache[i].state = ARP_STATE_STALE;
            }
        }
    }
}
