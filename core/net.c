// core/net.c - Optimized network core
#include "net.h"
#include "net_if.h"
#include "net_dhcp.h"
#include "socket.h"
#include "arp.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/timer.h"

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production, 1 for debugging
// ============================================================================
#define NET_DEBUG_ENABLED     0
#define NET_DEBUG_INIT        0    // Log initialization
#define NET_DEBUG_PACKETS     0    // Log packet details
#define NET_DEBUG_ERRORS      1    // Always log errors

net_if_t* if_list = 0;
net_if_t* default_if = 0;
ip_addr_t my_ip = {{0,0,0,0}};
mac_addr_t my_mac = {{0,0,0,0,0,0}};
ip_addr_t gateway_ip = {{0,0,0,0}};
mac_addr_t gateway_mac = {{0,0,0,0,0,0}};
int net_is_connected = 0;

extern void rtl8139_poll();

// Helper to update global "my_ip" legacy variable
void net_update_globals() { 
    if(default_if) { 
        my_ip.addr = default_if->ip_addr; 
        memcpy(my_mac.addr, default_if->mac, 6); 
        gateway_ip.addr = default_if->gateway; 
    } 
}

// Convert IP (network byte order) to string
void ip_to_str(uint32_t ip, char* str) {
    uint8_t b0 = (ip >> 24) & 0xFF;
    uint8_t b1 = (ip >> 16) & 0xFF;
    uint8_t b2 = (ip >> 8) & 0xFF;
    uint8_t b3 = ip & 0xFF;
    
    int_to_str(b0, str); strcat(str, ".");
    int len = strlen(str);
    int_to_str(b1, str + len); strcat(str + len, ".");
    len = strlen(str);
    int_to_str(b2, str + len); strcat(str + len, ".");
    len = strlen(str);
    int_to_str(b3, str + len);
}

// Parse IP string to uint32_t (returns network byte order)
uint32_t ip_parse(const char* str) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    int num = 0;
    int byte_idx = 0;
    
    while (*str && byte_idx < 4) {
        if (*str == '.') {
            bytes[byte_idx++] = num;
            num = 0;
        } else if (*str >= '0' && *str <= '9') {
            num = num * 10 + (*str - '0');
        }
        str++;
    }
    if (byte_idx < 4) bytes[byte_idx] = num;
    
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | 
           ((uint32_t)bytes[2] << 8) | ((uint32_t)bytes[3]);
}

void mac_to_str(uint8_t* mac, char* str) {
    sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void int_to_hex(uint32_t n, char* buf) {
    char* c = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for(int i=0; i<8; i++) buf[2+i] = c[(n >> ((7-i)*4)) & 0xF];
    buf[10] = 0;
}

void net_register_interface(net_if_t* interface) {
#if NET_DEBUG_INIT
    s_printf("[NET] Registering interface\n");
#endif
    
    interface->next = if_list; 
    if_list = interface;
    if (!default_if) { 
        default_if = interface; 
        net_update_globals(); 
    }
#if NET_DEBUG_INIT
    s_printf("[NET] Interface Registered: ");
    s_printf(interface->name);
    s_printf("\n");
#endif
}

net_if_t* net_get_default() { 
    return default_if; 
}

void net_add_static_arp(uint32_t ip, uint8_t* mac) {
    arp_add_static(ip, mac);
}

void net_init_arp(void) {
    arp_init();
    // QEMU MAC
    uint8_t qemu_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    
    // Gateway: 10.0.2.2
    net_add_static_arp(ip_parse("10.0.2.2"), qemu_mac);
    
    // DNS: 10.0.2.3
    net_add_static_arp(ip_parse("10.0.2.3"), qemu_mac);
}

uint16_t checksum(void *vdata, int length) {
    char* data = (char*)vdata; 
    uint32_t acc = 0;
    for (int i = 0; i + 1 < length; i += 2) { 
        uint16_t word; 
        memcpy(&word, data + i, 2); 
        acc += word; 
    }
    if (length & 1) { 
        uint16_t word = 0; 
        memcpy(&word, data + length - 1, 1); 
        acc += word; 
    }
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

uint16_t udp_checksum(uint8_t* packet, uint16_t len, uint32_t src_ip, uint32_t dst_ip) {
    uint32_t sum = 0;
    
    uint32_t src_be = htonl(src_ip);
    uint32_t dst_be = htonl(dst_ip);
    
    uint16_t* s = (uint16_t*)&src_be;
    sum += s[0]; sum += s[1];
    
    uint16_t* d = (uint16_t*)&dst_be;
    sum += d[0]; sum += d[1];
    
    sum += htons(IP_PROTO_UDP);
    sum += htons(len);
    
    uint16_t* ptr = (uint16_t*)packet;
    int count = len;
    while (count > 1) {
        sum += *ptr++;
        count -= 2;
    }
    if (count > 0) {
        sum += *(uint8_t*)ptr;
    }
    
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

void net_arp_send_request(uint32_t target_ip) {
    if (!default_if) return;
    uint8_t buf[256]; 
    memset(buf, 0, 256);
    eth_header_t* eth = (eth_header_t*)buf;
    arp_packet_t* arp = (arp_packet_t*)(buf + sizeof(eth_header_t));

    memset(eth->dest, 0xFF, 6);
    memcpy(eth->src, default_if->mac, 6);
    eth->type = htons(ETHERTYPE_ARP);

    arp->hw_type = htons(1); 
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6; 
    arp->proto_len = 4;
    arp->opcode = htons(1); 
    memcpy(arp->sender_mac, default_if->mac, 6);
    
    arp->sender_ip = htonl(default_if->ip_addr);
    arp->target_ip = htonl(target_ip);

    default_if->send(default_if, buf, sizeof(eth_header_t) + sizeof(arp_packet_t));
}

int net_resolve_arp(uint32_t ip, uint8_t* mac_out) {
    return arp_resolve(ip, mac_out);
}

// Optimized IP packet sending - minimal debug output
int net_send_raw_ip(uint32_t dest_ip, uint8_t proto, const uint8_t* data, uint32_t len) {
    if (!default_if) {
#if NET_DEBUG_ERRORS
        s_printf("[NET] No Default Interface!\n");
#endif
        return -1;
    }

    mac_addr_t final_dest_mac;
    memset(final_dest_mac.addr, 0, 6);

    if (dest_ip == 0xFFFFFFFF) {
        memset(final_dest_mac.addr, 0xFF, 6);
    } else {
        if (default_if->ip_addr == 0 || default_if->gateway == 0) {
#if NET_DEBUG_ERRORS
            s_printf("[NET] Interface not configured\n");
#endif
            return -1;
        }
        
        // Determine if destination is local
        uint32_t dest_net = dest_ip & default_if->netmask;
        uint32_t my_net = default_if->ip_addr & default_if->netmask;
        int is_local = (dest_net == my_net);
        
        uint32_t route_ip = is_local ? dest_ip : default_if->gateway;
        
        if (route_ip == 0) {
#if NET_DEBUG_ERRORS
            s_printf("[NET] Routing error\n");
#endif
            return -1;
        }

        if(net_resolve_arp(route_ip, final_dest_mac.addr) != 0) {
#if NET_DEBUG_ERRORS
            s_printf("[NET] ARP Failed\n");
#endif
            return -1;
        }
    }
    
    uint32_t total_len = sizeof(eth_header_t) + sizeof(ip_header_t) + len;
    uint8_t* packet = (uint8_t*)kmalloc(total_len);
    if (!packet) return -1;
    memset(packet, 0, total_len);

    eth_header_t* eth = (eth_header_t*)packet;
    ip_header_t* ip = (ip_header_t*)(packet + sizeof(eth_header_t));
    uint8_t* payload = (uint8_t*)(packet + sizeof(eth_header_t) + sizeof(ip_header_t));

    memcpy(eth->dest, final_dest_mac.addr, 6);
    memcpy(eth->src, default_if->mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip->version = 4;
    ip->ihl = 5;
    ip->ttl = 64;
    ip->proto = proto;
    
    // IP addresses in network byte order
    uint8_t* ip_src = (uint8_t*)&ip->src_ip;
    ip_src[0] = (default_if->ip_addr >> 24) & 0xFF;
    ip_src[1] = (default_if->ip_addr >> 16) & 0xFF;
    ip_src[2] = (default_if->ip_addr >> 8) & 0xFF;
    ip_src[3] = default_if->ip_addr & 0xFF;
    
    uint8_t* ip_dst = (uint8_t*)&ip->dest_ip;
    ip_dst[0] = (dest_ip >> 24) & 0xFF;
    ip_dst[1] = (dest_ip >> 16) & 0xFF;
    ip_dst[2] = (dest_ip >> 8) & 0xFF;
    ip_dst[3] = dest_ip & 0xFF;
    
    ip->len = htons(sizeof(ip_header_t) + len);
    ip->checksum = 0;
    ip->checksum = checksum(ip, sizeof(ip_header_t));

    memcpy(payload, data, len);
    
    int res = default_if->send(default_if, packet, total_len);
    kfree(packet);
    return res;
}

int net_send_udp_packet(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
                        const uint8_t* data, uint32_t len) {
    if (!default_if) return -1;
    
    uint32_t udp_len = sizeof(udp_header_t) + len;
    uint8_t* udp_buf = (uint8_t*)kmalloc(udp_len);
    if (!udp_buf) return -1;
    
    udp_header_t* udp = (udp_header_t*)udp_buf;
    uint8_t* payload = udp_buf + sizeof(udp_header_t);
    
    udp->src_port = htons(src_port); 
    udp->dest_port = htons(dest_port);
    udp->length = htons(udp_len);
    memcpy(payload, data, len);
    
    udp->checksum = 0;

    int res = net_send_raw_ip(dest_ip, IP_PROTO_UDP, udp_buf, udp_len);
    kfree(udp_buf);
    return res;
}

// Optimized packet handling - minimal debug output
void net_handle_packet(uint8_t* data, uint32_t len) {
    if (!default_if || !data) return;
    eth_header_t* eth = (eth_header_t*)data;
    uint16_t type = ntohs(eth->type);

    if (type == ETHERTYPE_IP) {
        ip_header_t* ip = (ip_header_t*)(data + sizeof(eth_header_t));
        int ip_hdr_len = ip->ihl * 4;
        
        uint32_t src_ip = ntohl(ip->src_ip);
        uint32_t dst_ip = ntohl(ip->dest_ip);
        
        if (ip->proto == IP_PROTO_ICMP) {
            icmp_header_t* icmp = (icmp_header_t*)(data + sizeof(eth_header_t) + ip_hdr_len);
            
            if (icmp->type == ICMP_ECHO_REPLY) {
                extern void net_ping_reply_received(void);
                net_ping_reply_received();
            }
        }
        else if (ip->proto == IP_PROTO_UDP) {
            udp_header_t* udp = (udp_header_t*)(data + sizeof(eth_header_t) + ip_hdr_len);
            uint8_t* payload = data + sizeof(eth_header_t) + ip_hdr_len + sizeof(udp_header_t);
            uint32_t payload_len = ntohs(udp->length) - sizeof(udp_header_t);
            
            // DHCP
            if (ntohs(udp->dest_port) == 68) {
                extern void dhcp_process_packet(uint8_t*, uint32_t);
                dhcp_process_packet(payload, payload_len);
            }
            
            socket_process_packet(payload, payload_len, src_ip, ntohs(udp->src_port),
                                dst_ip, ntohs(udp->dest_port), IPPROTO_UDP);
        }
        else if (ip->proto == IP_PROTO_TCP) {
            extern void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip);
            tcp_handle_packet(data + sizeof(eth_header_t) + ip_hdr_len,
                            len - sizeof(eth_header_t) - ip_hdr_len,
                            src_ip, dst_ip);
        }
    }
    else if (type == ETHERTYPE_ARP) {
        arp_receive(data, len);
    }
}

void net_dhcp_discover() { extern int dhcp_discover(void); dhcp_discover(); }

void net_init() {
    net_init_arp();
}

void net_set_ip(uint32_t ip) {
    if(default_if) {
        default_if->ip_addr = ip;
        my_ip.addr = ip;
        net_is_connected = 1;
    }
}

void net_set_gateway(uint32_t gateway) {
    if(default_if) {
        default_if->gateway = gateway;
        gateway_ip.addr = gateway;
    }
}

// Simple ICMP Echo Request (Ping)
static volatile int ping_received = 0;
static uint16_t ping_id = 0x1234;
static uint16_t ping_seq = 0;

void net_ping_reply_received(void) {
    ping_received = 1;
}

int ping(uint32_t dest_ip, uint32_t timeout_ticks) {
    if (!default_if) return -1;
    
    uint8_t dest_mac[6];
    if (net_resolve_arp(dest_ip, dest_mac) != 0) {
#if NET_DEBUG_ERRORS
        s_printf("[PING] ARP resolve failed\n");
#endif
        return -1;
    }
    
    uint32_t packet_len = sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(icmp_header_t);
    uint8_t* packet = (uint8_t*)kmalloc(packet_len);
    if (!packet) return -1;
    
    // Ethernet header
    eth_header_t* eth = (eth_header_t*)packet;
    memcpy(eth->dest, dest_mac, 6);
    memcpy(eth->src, default_if->mac, 6);
    eth->type = htons(ETHERTYPE_IP);
    
    // IP header
    ip_header_t* ip = (ip_header_t*)(packet + sizeof(eth_header_t));
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->len = htons(sizeof(ip_header_t) + sizeof(icmp_header_t));
    ip->id = 0;
    ip->frag_offset = 0;
    ip->ttl = 64;
    ip->proto = IP_PROTO_ICMP;
    ip->checksum = 0;
    
    uint8_t* ip_src = (uint8_t*)&ip->src_ip;
    ip_src[0] = (default_if->ip_addr >> 24) & 0xFF;
    ip_src[1] = (default_if->ip_addr >> 16) & 0xFF;
    ip_src[2] = (default_if->ip_addr >> 8) & 0xFF;
    ip_src[3] = default_if->ip_addr & 0xFF;
    
    uint8_t* ip_dst = (uint8_t*)&ip->dest_ip;
    ip_dst[0] = (dest_ip >> 24) & 0xFF;
    ip_dst[1] = (dest_ip >> 16) & 0xFF;
    ip_dst[2] = (dest_ip >> 8) & 0xFF;
    ip_dst[3] = dest_ip & 0xFF;
    
    ip->checksum = checksum(ip, sizeof(ip_header_t));
    
    // ICMP Echo Request
    icmp_header_t* icmp = (icmp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ip_header_t));
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(ping_id);
    icmp->seq = htons(ping_seq++);
    icmp->checksum = checksum(icmp, sizeof(icmp_header_t));
    
    ping_received = 0;
    
    int result = default_if->send(default_if, packet, packet_len);
    kfree(packet);
    
    if (result != 0) {
#if NET_DEBUG_ERRORS
        s_printf("[PING] Send failed\n");
#endif
        return -1;
    }
    
    // Wait for response with polling
    uint32_t start = get_tick_count();
    while ((get_tick_count() - start) < timeout_ticks) {
        rtl8139_poll();
        
        if (ping_received) {
            return 0;
        }
    }
    
    return -1;  // Timeout
}

// Optimized net_get_ip - no debug output
uint32_t net_get_ip() { 
    if (default_if && default_if->ip_addr != 0) {
        return default_if->ip_addr;
    }
    return my_ip.addr; 
}
