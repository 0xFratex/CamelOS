// core/net_dhcp.c
#include "net.h"
#include "net_dhcp.h"
#include "net_if.h"
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

// Store the server IP from DHCP Offer so we can send it in DHCPREQUEST
static uint32_t dhcp_server_ip = 0;

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

    // FIX: Send the actual packet buffer data, not (uint8_t*)&packet which
    // sends only the 4-byte pointer. Previously sizeof(packet) was 4 bytes
    // (just the pointer size), sending garbage instead of the DHCP packet.
    uint32_t packet_len = (uint32_t)((uint8_t*)opt - packet_buf);
    uint32_t broadcast_ip = 0xFFFFFFFF;
    net_send_udp_packet(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet_buf, packet_len);

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
    // FIX: Use the stored server IP from the DHCP Offer instead of 0.
    // Previously this was always 0, which broke DHCPREQUEST because the
    // server couldn't determine which offer was being accepted.
    *opt++ = 54;
    *opt++ = 4;
    memcpy(opt, &dhcp_server_ip, 4);
    opt += 4;

    // End option
    *opt++ = 255;

    // FIX: Send the actual packet buffer data, not (uint8_t*)&packet
    uint32_t packet_len = (uint32_t)((uint8_t*)opt - packet_buf);
    uint32_t broadcast_ip = 0xFFFFFFFF;
    net_send_udp_packet(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet_buf, packet_len);

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

    // FIX: Store the server IP so dhcp_request() can use it in the
    // Server Identifier option (option 54). Without this, DHCPREQUEST
    // always sent server_ip=0, causing the DHCP server to reject or
    // ignore the request.
    dhcp_server_ip = server_ip;

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
            // FIX: Store subnet mask in the interface struct
            uint32_t subnet_mask;
            memcpy(&subnet_mask, &opts[i+2], 4);
            if(default_if) {
                default_if->netmask = ntohl(subnet_mask);
            }
        } else if(opts[i] == 3 && opts[i+1] >= 4) { // Router
            uint32_t gateway;
            memcpy(&gateway, &opts[i+2], 4);
            if(default_if) {
                default_if->gateway = ntohl(gateway);
                gateway_ip.addr = ntohl(gateway);
            }
        } else if(opts[i] == 6 && opts[i+1] >= 4) { // DNS
            // FIX: Store DNS server via net_set_dns()
            uint32_t dns_ip;
            memcpy(&dns_ip, &opts[i+2], 4);
            net_set_dns(ntohl(dns_ip));
        }

        if(opts[i] == 0) i++;
        else i += opts[i+1] + 2;
    }

    dhcp_state = 3; // Bound
    s_printf("[DHCP] Network configured\n");
}

// FIX: Handle DHCP NAK — reset state so we can retry discovery
static void dhcp_handle_nak(dhcp_packet_t* dhcp) {
    (void)dhcp;
    s_printf("[DHCP] NAK received — resetting to idle\n");
    dhcp_state = 0;
    dhcp_server_ip = 0;
}

void dhcp_process_packet(uint8_t* payload, uint32_t len) {
    if(len < sizeof(dhcp_packet_t)) return;

    dhcp_packet_t* dhcp = (dhcp_packet_t*)payload;

    // Check if it's for us
    if(dhcp->xid != dhcp_xid) return;
    if(!default_if || memcmp(dhcp->chaddr, default_if->mac, 6) != 0) return;

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
    } else if(msg_type == 6) { // NAK
        dhcp_handle_nak(dhcp);
    }
}

// ============================================================================
// High-level DHCP auto-configure function
// Attempts the full DORA sequence (Discover-Offer-Request-Ack)
// Returns 0 on success (IP configured), -1 on failure (use static fallback)
// ============================================================================

int dhcp_auto_configure(void) {
    extern void rtl8139_poll(void);

    s_printf("[DHCP] Starting auto-configuration...\n");

    // Send Discover
    if (dhcp_discover() != 0) {
        s_printf("[DHCP] Failed to send discover\n");
        return -1;
    }

    // Wait for Offer + Request + ACK with timeout
    // The DORA sequence: Discover -> (wait) Offer -> Request -> (wait) ACK
    // We poll the network and let the DHCP packet handler process responses
    uint32_t start = 0;
    extern uint32_t get_tick_count(void);
    start = get_tick_count();

    // FIX: Increased timeout from ~5 seconds to ~10 seconds for better
    // reliability on slow or congested networks
    while (dhcp_state != 3) {
        // Poll network to receive DHCP responses
        rtl8139_poll();

        // FIX: Process TCP listeners during DHCP polling so that any
        // pending TCP connections (e.g., from earlier requests) don't
        // stall while we wait for DHCP to complete.
        extern void tcp_process_listeners(void);
        tcp_process_listeners();

        // Check for timeout (10 seconds = 1000 ticks at 100Hz)
        uint32_t elapsed = get_tick_count() - start;
        if (elapsed > 1000) {
            s_printf("[DHCP] Auto-configuration timed out\n");
            dhcp_state = 0;  // Reset state
            return -1;
        }

        // Small delay to avoid busy-wait
        for (volatile int i = 0; i < 1000; i++) asm volatile("pause");
    }

    if (dhcp_state == 3) {
        s_printf("[DHCP] Auto-configuration successful\n");
        return 0;
    }

    return -1;
}
