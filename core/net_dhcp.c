// core/net_dhcp.c
#include "net.h"
#include "net_dhcp.h"
#include "socket.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"

extern net_if_t* default_if;
extern ip_addr_t my_ip;
extern ip_addr_t gateway_ip;
extern int net_is_connected;

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC_COOKIE 0x63825363

static uint32_t dhcp_xid = 0x12345678;
static int dhcp_state = 0; // 0=idle, 1=discovering, 2=requesting, 3=bound

int dhcp_discover(void) {
    s_printf("[DHCP] Starting discovery...\n");

    uint8_t packet_buf[512];
    dhcp_packet_t* packet = (dhcp_packet_t*)packet_buf;
    memset(packet, 0, sizeof(dhcp_packet_t) + 64); // Extra space for options

    packet->op = 1; // BOOTREQUEST
    packet->htype = 1; // Ethernet
    packet->hlen = 6;
    packet->xid = dhcp_xid;
    packet->magic = htonl(DHCP_MAGIC_COOKIE);

    // Get MAC address
    if(!default_if) return -1;
    memcpy(packet->chaddr, default_if->mac, 6);

    // Add DHCP options
    uint8_t* opt = packet->options;

    // Message type: Discover
    *opt++ = 53; // DHCP Message Type
    *opt++ = 1;
    *opt++ = 1; // Discover

    // Requested IP (optional)
    *opt++ = 50;
    *opt++ = 4;
    uint32_t requested_ip = 0; // 0 = any
    memcpy(opt, &requested_ip, 4);
    opt += 4;

    // Parameter Request List
    *opt++ = 55; // Parameter Request List
    *opt++ = 3;  // Length
    *opt++ = 1;  // Subnet Mask
    *opt++ = 3;  // Router
    *opt++ = 6;  // DNS

    // End option
    *opt++ = 255;

    // Send broadcast using existing UDP function
    uint32_t broadcast_ip = 0xFFFFFFFF;
    net_send_udp_packet(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, (uint8_t*)&packet, sizeof(packet));

    dhcp_state = 1; // Discovering
    s_printf("[DHCP] Discovery sent\n");
    return 0;
}

int dhcp_request(uint32_t offered_ip) {
    s_printf("[DHCP] Requesting IP...\n");

    uint8_t packet_buf[512];
    dhcp_packet_t* packet = (dhcp_packet_t*)packet_buf;
    memset(packet, 0, sizeof(dhcp_packet_t) + 64); // Extra space for options

    packet->op = 1; // BOOTREQUEST
    packet->htype = 1; // Ethernet
    packet->hlen = 6;
    packet->xid = dhcp_xid;
    packet->magic = htonl(DHCP_MAGIC_COOKIE);

    // Get MAC address
    if(!default_if) return -1;
    memcpy(packet->chaddr, default_if->mac, 6);

    // Add DHCP options
    uint8_t* opt = packet->options;

    // Message type: Request
    *opt++ = 53; // DHCP Message Type
    *opt++ = 1;
    *opt++ = 3; // Request

    // Requested IP
    *opt++ = 50;
    *opt++ = 4;
    memcpy(opt, &offered_ip, 4);
    opt += 4;

    // Server Identifier (required in request)
    *opt++ = 54;
    *opt++ = 4;
    uint32_t server_ip = 0; // From offer - TODO: store from offer
    memcpy(opt, &server_ip, 4);
    opt += 4;

    // End option
    *opt++ = 255;

    // Send broadcast using existing UDP function
    uint32_t broadcast_ip = 0xFFFFFFFF;
    net_send_udp_packet(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, (uint8_t*)&packet, sizeof(packet));

    dhcp_state = 2; // Requesting
    s_printf("[DHCP] Request sent\n");
    return 0;
}

void dhcp_handle_offer(dhcp_packet_t* dhcp) {
    if(dhcp_state != 1) return;

    uint32_t offered_ip = ntohl(dhcp->yiaddr);
    s_printf("[DHCP] Offer received: ");
    char ip_str[16];
    ip_to_str(offered_ip, ip_str);
    s_printf(ip_str);
    s_printf("\n");

    // Parse options for server IP
    uint8_t* opts = dhcp->options;
    uint32_t server_ip = 0;

    for(int i = 0; i < 308 && opts[i] != 255; ) {
        if(opts[i] == 54 && opts[i+1] >= 4) { // Server Identifier
            memcpy(&server_ip, &opts[i+2], 4);
            break;
        }
        if(opts[i] == 0) i++;
        else i += opts[i+1] + 2;
    }

    if(server_ip) {
        dhcp_request(offered_ip);
    }
}

void dhcp_handle_ack(dhcp_packet_t* dhcp) {
    if(dhcp_state != 2) return;

    uint32_t assigned_ip = ntohl(dhcp->yiaddr);
    s_printf("[DHCP] ACK received: ");
    char ip_str[16];
    ip_to_str(assigned_ip, ip_str);
    s_printf(ip_str);
    s_printf("\n");

    // Set network configuration
    if(default_if) {
        default_if->ip_addr = assigned_ip;
        my_ip.addr = assigned_ip;
        net_is_connected = 1;
    }

    // Parse options
    uint8_t* opts = dhcp->options;
    for(int i = 0; i < 308 && opts[i] != 255; ) {
        if(opts[i] == 1 && opts[i+1] >= 4) { // Subnet Mask
            // Could store subnet mask
        } else if(opts[i] == 3 && opts[i+1] >= 4) { // Router
            uint32_t gateway;
            memcpy(&gateway, &opts[i+2], 4);
            if(default_if) {
                default_if->gateway = ntohl(gateway);
                gateway_ip.addr = ntohl(gateway);
            }
        } else if(opts[i] == 6 && opts[i+1] >= 4) { // DNS
            // Could store DNS servers
        }

        if(opts[i] == 0) i++;
        else i += opts[i+1] + 2;
    }

    dhcp_state = 3; // Bound
    s_printf("[DHCP] Network configured\n");
}

void dhcp_process_packet(uint8_t* payload, uint32_t len) {
    if(len < sizeof(dhcp_packet_t)) return;

    dhcp_packet_t* dhcp = (dhcp_packet_t*)payload;

    // Check if it's for us
    if(dhcp->xid != dhcp_xid) return;
    if(memcmp(dhcp->chaddr, default_if->mac, 6) != 0) return;

    // Check message type in options
    uint8_t* opts = dhcp->options;
    uint8_t msg_type = 0;

    for(int i = 0; i < 308 && opts[i] != 255; ) {
        if(opts[i] == 53 && opts[i+1] >= 1) { // Message Type
            msg_type = opts[i+2];
            break;
        }
        if(opts[i] == 0) i++;
        else i += opts[i+1] + 2;
    }

    if(msg_type == 2) { // Offer
        dhcp_handle_offer(dhcp);
    } else if(msg_type == 5) { // ACK
        dhcp_handle_ack(dhcp);
    }
}