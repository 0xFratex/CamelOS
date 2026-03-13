// core/net.h
#ifndef NET_H
#define NET_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP  0x0800
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

// ICMP Types
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8
#define PORT_DHCP_SERVER 67
#define PORT_DHCP_CLIENT 68

static inline uint16_t htons(uint16_t v) { return (v << 8) | (v >> 8); }
static inline uint32_t htonl(uint32_t v) { return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF); }
#define ntohs htons
#define ntohl htonl

// MAC/IP Types
typedef struct { uint8_t addr[6]; } __attribute__((packed)) mac_addr_t;
typedef union { uint8_t parts[4]; uint32_t addr; } ip_addr_t;
typedef struct { uint8_t parts[16]; } ipv6_addr_t;

// Headers
typedef struct { uint8_t dest[6]; uint8_t src[6]; uint16_t type; } __attribute__((packed)) eth_header_t;
typedef struct { uint16_t hw_type; uint16_t proto_type; uint8_t hw_len; uint8_t proto_len; uint16_t opcode; uint8_t sender_mac[6]; uint32_t sender_ip; uint8_t target_mac[6]; uint32_t target_ip; } __attribute__((packed)) arp_packet_t;
typedef struct { uint8_t ihl : 4; uint8_t version : 4; uint8_t tos; uint16_t len; uint16_t id; uint16_t frag_offset; uint8_t ttl; uint8_t proto; uint16_t checksum; uint32_t src_ip; uint32_t dest_ip; } __attribute__((packed)) ip_header_t;
typedef struct { uint8_t type; uint8_t code; uint16_t checksum; uint16_t id; uint16_t seq; } __attribute__((packed)) icmp_header_t;
typedef struct { uint16_t src_port; uint16_t dest_port; uint16_t length; uint16_t checksum; } __attribute__((packed)) udp_header_t;


// TCP Flags
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

// DHCP
typedef struct { uint8_t op; uint8_t htype; uint8_t hlen; uint8_t hops; uint32_t xid; uint16_t secs; uint16_t flags; uint32_t ciaddr; uint32_t yiaddr; uint32_t siaddr; uint32_t giaddr; uint8_t chaddr[16]; uint8_t sname[64]; uint8_t file[128]; uint32_t magic; uint8_t options[0]; } __attribute__((packed)) dhcp_packet_t;

extern ip_addr_t my_ip;
extern mac_addr_t my_mac;
extern ip_addr_t gateway_ip;
extern mac_addr_t gateway_mac;
extern int net_is_connected;

void net_init();
void net_handle_packet(uint8_t* data, uint32_t len);
int net_send_ping(uint32_t dest_ip);
int net_check_ping_reply(int* latency_ms);
void net_dhcp_discover();
int net_send_raw_ip(uint32_t dest_ip, uint8_t proto, const uint8_t* data, uint32_t len);
uint32_t net_get_ip();
void net_add_static_arp(uint32_t ip, uint8_t* mac);
void net_set_ip(uint32_t ip);
void net_set_gateway(uint32_t gateway);
void net_set_dns(uint32_t dns);
int net_send_udp_packet(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port, const uint8_t* data, uint32_t len);
uint16_t udp_checksum(uint8_t* packet, uint16_t len, uint32_t src_ip, uint32_t dst_ip);
void debug_packet(const char* direction, uint8_t* packet, uint32_t len);
void debug_ip(const char* label, uint32_t ip);

// DHCP functions
int dhcp_discover(void);

// Utility functions
void ip_to_str(uint32_t ip, char* out);
uint32_t ip_parse(const char* str);
void mac_to_str(uint8_t* mac, char* str);
void int_to_hex(uint32_t n, char* buf);

#endif