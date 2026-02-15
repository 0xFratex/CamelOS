// core/tcp.c - Optimized TCP implementation with proper state machine
#include "tcp.h"
#include "net.h"
#include "net_if.h"
#include "socket.h"
#include "memory.h"
#include "timer.h"
#include "string.h"
#include "../hal/drivers/serial.h"

// External declaration for printk from string.c
extern void printk(const char* fmt, ...);

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production, 1 for debugging
// ============================================================================
#define TCP_DEBUG_ENABLED     0
#define TCP_DEBUG_STATE       0    // Log state transitions
#define TCP_DEBUG_PACKETS     0    // Log packet details
#define TCP_DEBUG_ERRORS      0    // Log errors (set to 0 for production)

#define TCP_MAX_CONNECTIONS 32
#define TCP_WINDOW_SIZE 4096
#define TCP_MSS 1460
#define TCP_RETRANSMIT_TIMEOUT 1000 // ms


static tcp_connection_t tcp_connections[TCP_MAX_CONNECTIONS];
static uint16_t tcp_next_port = 49152; // Start of ephemeral ports

// TCP FSM states are defined in tcp.h

// Byte order conversion helpers
static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x & 0xFF0000) >> 8) | ((x >> 24) & 0xFF);
}

// Find or allocate connection - OPTIMIZED
static tcp_connection_t* tcp_find_connection(uint32_t local_ip_net, uint16_t local_port,
                                           uint32_t remote_ip_net, uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state != TCP_CLOSED) {
            if (tcp_connections[i].local_ip == local_ip_net &&
                tcp_connections[i].local_port == local_port &&
                tcp_connections[i].remote_ip == remote_ip_net &&
                tcp_connections[i].remote_port == remote_port) {
                return &tcp_connections[i];
            }
        }
    }
    return NULL;
}

static tcp_connection_t* tcp_alloc_connection() {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_CLOSED) {
            memset(&tcp_connections[i], 0, sizeof(tcp_connection_t));
            return &tcp_connections[i];
        }
    }
    return NULL;
}

// TCP checksum calculation - OPTIMIZED
uint16_t tcp_checksum(uint8_t* packet, uint16_t len, uint32_t src_ip, uint32_t dst_ip) {
    uint32_t sum = 0;

    // Pseudo header
    sum += ntohs((src_ip >> 16) & 0xFFFF);
    sum += ntohs(src_ip & 0xFFFF);
    sum += ntohs((dst_ip >> 16) & 0xFFFF);
    sum += ntohs(dst_ip & 0xFFFF);
    
    sum += htons(IPPROTO_TCP);
    sum += htons(len);

    // Sum all 16-bit words in the packet
    int i;
    for (i = 0; i + 1 < len; i += 2) {
        sum += *((uint16_t*)(packet + i));
    }
    // Handle odd length
    if (i < len) {
        sum += packet[i];
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

// Send TCP packet - OPTIMIZED
int tcp_send(tcp_connection_t* conn, uint8_t flags, uint8_t* data, uint16_t len) {
    uint8_t packet[1500];
    tcp_header_t* tcp = (tcp_header_t*)packet;

    memset(tcp, 0, sizeof(tcp_header_t));

    tcp->src_port = htons(conn->local_port);
    tcp->dest_port = htons(conn->remote_port);
    tcp->seq_num = htonl(conn->snd_nxt);
    tcp->ack_num = htonl(conn->rcv_nxt);
    tcp->data_offset = 5 << 4;  // 5 * 4 = 20 bytes header
    tcp->flags = flags;
    tcp->window = htons(TCP_WINDOW_SIZE);
    tcp->urgent_ptr = 0;

    uint16_t header_len = 20;  // Base header length in bytes
    
    if (flags & TCP_SYN) {
        // Add MSS option (4 bytes) + NOP padding
        packet[20] = 2;    // Kind: MSS
        packet[21] = 4;    // Length: 4 bytes
        packet[22] = 0x05; // MSS = 1460 (high byte)
        packet[23] = 0xB4; // MSS = 1460 (low byte)
        packet[24] = 1;    // NOP
        packet[25] = 1;    // NOP
        packet[26] = 1;    // NOP
        packet[27] = 1;    // NOP
        header_len = 28;
        tcp->data_offset = 7 << 4;
    }

    // Copy data if any
    if (data && len > 0) {
        memcpy(packet + header_len, data, len);
    }

    // Calculate checksum
    uint16_t tcp_len = header_len + len;
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(packet, tcp_len, conn->local_ip, conn->remote_ip);
    
    // Send via IP layer
    return net_send_raw_ip(conn->remote_ip, IPPROTO_TCP, packet, tcp_len);
}

// TCP connection establishment
int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    // Find unused local port
    uint16_t local_port = tcp_next_port++;
    if (tcp_next_port > 65535) tcp_next_port = 49152;

    // Allocate connection
    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) {
        return -1;
    }

    conn->state = TCP_SYN_SENT;
    conn->local_ip = net_get_ip();
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->snd_nxt = 1;
    conn->connect_time = timer_get_ticks();

    // Send SYN
    tcp_send(conn, TCP_SYN, NULL, 0);
    conn->snd_nxt++;

    return local_port;
}

// Helper function for socket layer - returns connection pointer
tcp_connection_t* tcp_connect_with_ptr(uint32_t remote_ip, uint16_t remote_port) {
    uint16_t local_port = tcp_next_port++;
    if (tcp_next_port > 65535) tcp_next_port = 49152;

    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) {
        return NULL;
    }

    conn->state = TCP_SYN_SENT;
    conn->local_ip = net_get_ip();
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->snd_nxt = 1;
    conn->connect_time = timer_get_ticks();

    // Send SYN
    tcp_send(conn, TCP_SYN, NULL, 0);
    conn->snd_nxt++;

    return conn;
}

// Get local port from connection
uint16_t tcp_conn_get_local_port(void* conn_ptr) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    return conn ? conn->local_port : 0;
}

// Check if connection is established
int tcp_conn_is_established(void* conn_ptr) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    return conn && conn->state == TCP_ESTABLISHED;
}

// Process incoming TCP packet - OPTIMIZED
void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    tcp_header_t* tcp = (tcp_header_t*)packet;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dest_port);

    // Find connection
    tcp_connection_t* conn = tcp_find_connection(dst_ip, dst_port, src_ip, src_port);
    if (!conn) {
        return;
    }

    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;

    // Handle RST
    if (flags & TCP_RST) {
        conn->state = TCP_CLOSED;
        if (conn->on_state_change) {
            conn->on_state_change(conn->state, TCP_CLOSED);
        }
        return;
    }

    // Update window
    conn->window = ntohs(tcp->window);

    // Process based on current state
    switch (conn->state) {
        case TCP_SYN_SENT:
            if (flags & TCP_SYN && flags & TCP_ACK) {
                if (ack == conn->snd_nxt) {
                    conn->rcv_nxt = seq + 1;
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;

                    // Send ACK
                    tcp_send(conn, TCP_ACK, NULL, 0);

                    if (conn->on_state_change) {
                        conn->on_state_change(TCP_SYN_SENT, TCP_ESTABLISHED);
                    }
                }
            }
            break;

        case TCP_ESTABLISHED:
            // Handle data
            if (len > (tcp->data_offset >> 2) * 4) {
                uint16_t data_len = len - (tcp->data_offset >> 2) * 4;
                uint8_t* data = packet + (tcp->data_offset >> 2) * 4;

                // Check sequence number
                if (seq == conn->rcv_nxt) {
                    // Add to receive buffer
                    if (conn->recv_tail + data_len <= TCP_WINDOW_SIZE) {
                        memcpy(conn->recv_buffer + conn->recv_tail, data, data_len);
                        conn->recv_tail += data_len;
                    } else {
                        // Wrap around
                        uint16_t first_part = TCP_WINDOW_SIZE - conn->recv_tail;
                        memcpy(conn->recv_buffer + conn->recv_tail, data, first_part);
                        memcpy(conn->recv_buffer, data + first_part, data_len - first_part);
                        conn->recv_tail = data_len - first_part;
                    }

                    conn->rcv_nxt += data_len;

                    // Call data callback
                    if (conn->on_data) {
                        conn->on_data(data, data_len, conn->callback_user_data);
                    }
                }

                // Send ACK for received data
                tcp_send(conn, TCP_ACK, NULL, 0);
            }

            // Handle FIN
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + 1;
                conn->state = TCP_CLOSE_WAIT;

                // Send ACK for FIN
                tcp_send(conn, TCP_ACK, NULL, 0);

                // Send our FIN
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                conn->state = TCP_LAST_ACK;
            }
            break;

        case TCP_FIN_WAIT1:
            if (flags & TCP_ACK) {
                conn->state = TCP_FIN_WAIT2;
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_ACK) {
                conn->state = TCP_CLOSED;
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_LAST_ACK, TCP_CLOSED);
                }
            }
            break;
    }

    // Update last ACK time
    conn->last_ack_time = timer_get_ticks();
}

// Send data over TCP connection
int tcp_send_data(tcp_connection_t* conn, uint8_t* data, uint16_t len) {
    if (conn->state != TCP_ESTABLISHED) {
        return -1;
    }

    // Add to send buffer
    if (conn->send_tail + len <= TCP_WINDOW_SIZE) {
        memcpy(conn->send_buffer + conn->send_tail, data, len);
        conn->send_tail += len;
    } else {
        // Wrap around
        uint16_t first_part = TCP_WINDOW_SIZE - conn->send_tail;
        memcpy(conn->send_buffer + conn->send_tail, data, first_part);
        memcpy(conn->send_buffer, data + first_part, len - first_part);
        conn->send_tail = len - first_part;
    }

    // Send data (in chunks of MSS)
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        tcp_send(conn, TCP_ACK | TCP_PSH, data + sent, chunk);
        conn->snd_nxt += chunk;
        sent += chunk;
    }

    return sent;
}

// Initialize TCP subsystem
void tcp_init() {
    memset(tcp_connections, 0, sizeof(tcp_connections));
}

// Set data callback for a TCP connection
void tcp_conn_set_data_callback(void* conn_ptr, void (*callback)(uint8_t*, uint16_t, void*), void* user_data) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn) {
        conn->on_data = callback;
        conn->callback_user_data = user_data;
    }
}

// Set state change callback for a TCP connection
void tcp_conn_set_state_callback(void* conn_ptr, void (*callback)(int, int)) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn) {
        conn->on_state_change = callback;
    }
}
