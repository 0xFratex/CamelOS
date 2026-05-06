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
#define TCP_WINDOW_SIZE 16384 // Increased to 16KB
#define TCP_MSS 1460
#define TCP_RETRANSMIT_TIMEOUT 2000  // ms (initial retransmit timeout in ticks)
#define TCP_MAX_RETRANSMIT     5     // Max retransmit attempts before giving up


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

// Send TCP packet - OPTIMIZED with static buffer to prevent stack overflow
// CRITICAL: tcp_send can be called from deep call chains (e.g., interrupt → tcp_handle_packet → tcp_send)
// Using a 1500-byte stack buffer was causing stack overflow with 16KB kernel stack
int tcp_send(tcp_connection_t* conn, uint8_t flags, uint8_t* data, uint16_t len) {
    static uint8_t packet[1500];  // Static to avoid stack overflow in deep call chains
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
    conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
    conn->retransmit_count = 0;
    conn->last_ack_time = timer_get_ticks();

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
    conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
    conn->retransmit_count = 0;
    conn->last_ack_time = timer_get_ticks();

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

// Forward declaration - needed because tcp_handle_incoming_syn is static and called
// from tcp_handle_packet before its definition
static int tcp_handle_incoming_syn(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint32_t seq);

// Process incoming TCP packet - OPTIMIZED
void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    tcp_header_t* tcp = (tcp_header_t*)packet;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dest_port);
    uint8_t pkt_flags = tcp->flags;

    // Find connection
    tcp_connection_t* conn = tcp_find_connection(dst_ip, dst_port, src_ip, src_port);
    if (!conn) {
        /* No existing connection found. Check if there's a listener on this port. */
        if (pkt_flags & TCP_SYN) {
            /* Incoming SYN to a potentially listening port */
            tcp_handle_incoming_syn(dst_port, src_ip, src_port, dst_ip, ntohl(tcp->seq_num));
        }
        /* Otherwise, silently drop the packet */
        return;
    }

    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = pkt_flags;

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
        case TCP_SYN_RECEIVED:
            /* Server side: waiting for ACK to complete 3-way handshake */
            if (flags & TCP_ACK) {
                if (ack == conn->snd_nxt) {
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;

                    if (conn->on_state_change) {
                        conn->on_state_change(TCP_SYN_RECEIVED, TCP_ESTABLISHED);
                    }
                }
            }
            break;

        case TCP_SYN_SENT:
            if (flags & TCP_SYN && flags & TCP_ACK) {
                if (ack == conn->snd_nxt) {
                    conn->rcv_nxt = seq + 1;
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;
                    conn->retransmit_count = 0;  /* Reset on successful handshake */

                    // Send ACK
                    tcp_send(conn, TCP_ACK, NULL, 0);

                    if (conn->on_state_change) {
                        conn->on_state_change(TCP_SYN_SENT, TCP_ESTABLISHED);
                    }
                }
            }
            break;

        case TCP_ESTABLISHED:
            // Handle ACK — reset retransmit timer on acknowledgment
            if (flags & TCP_ACK) {
                if (ack > conn->snd_una) {
                    conn->snd_una = ack;
                    conn->retransmit_count = 0;  /* ACK received, reset retransmit */
                    conn->last_ack_time = timer_get_ticks();
                }
            }

            // Handle data
            if (len > (tcp->data_offset >> 2) * 4) {
                uint16_t data_len = len - (tcp->data_offset >> 2) * 4;
                uint8_t* data = packet + (tcp->data_offset >> 2) * 4;

                // Check sequence number
                if (seq == conn->rcv_nxt) {
                    // Add to receive buffer with BOUNDS CHECK against actual buffer size
                    if (conn->recv_tail + data_len <= sizeof(conn->recv_buffer)) {
                        memcpy(conn->recv_buffer + conn->recv_tail, data, data_len);
                        conn->recv_tail += data_len;
                    } else if (conn->recv_tail < sizeof(conn->recv_buffer)) {
                        // Partial write: fill remaining space
                        uint32_t remaining = sizeof(conn->recv_buffer) - conn->recv_tail;
                        memcpy(conn->recv_buffer + conn->recv_tail, data, remaining);
                        conn->recv_tail = sizeof(conn->recv_buffer);
                    }
                    // If buffer is completely full, data is silently dropped (window closed)

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
            if (flags & TCP_FIN && flags & TCP_ACK) {
                // Simultaneous close: received FIN+ACK
                conn->rcv_nxt = seq + 1;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_CLOSED;
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_FIN_WAIT1, TCP_CLOSED);
                }
            } else if (flags & TCP_ACK) {
                conn->state = TCP_FIN_WAIT2;
            } else if (flags & TCP_FIN) {
                // Got FIN without ACK
                conn->rcv_nxt = seq + 1;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_CLOSING;
            }
            break;

        case TCP_FIN_WAIT2:
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + 1;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_CLOSED;
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_FIN_WAIT2, TCP_CLOSED);
                }
            }
            break;

        case TCP_CLOSING:
            if (flags & TCP_ACK) {
                conn->state = TCP_CLOSED;
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_CLOSING, TCP_CLOSED);
                }
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

    // Add to send buffer with bounds check against actual buffer size
    if (conn->send_tail + len <= sizeof(conn->send_buffer)) {
        memcpy(conn->send_buffer + conn->send_tail, data, len);
        conn->send_tail += len;
    } else if (conn->send_tail < sizeof(conn->send_buffer)) {
        // Partial write
        uint32_t remaining = sizeof(conn->send_buffer) - conn->send_tail;
        memcpy(conn->send_buffer + conn->send_tail, data, remaining);
        conn->send_tail = sizeof(conn->send_buffer);
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

// Send data over a TCP connection (void* wrapper for app layer)
int tcp_conn_send(void* conn_ptr, const void* data, int len) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    if (!data || len <= 0) return -1;
    return tcp_send_data(conn, (uint8_t*)data, (uint16_t)len);
}

// Receive data from a TCP connection (void* wrapper for app layer)
int tcp_conn_recv(void* conn_ptr, void* buf, int max_len) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (!conn || !buf || max_len <= 0) return -1;

    int available = (int)(conn->recv_tail - conn->recv_head);
    if (available <= 0) return 0;

    int to_read = (available < max_len) ? available : max_len;
    memcpy(buf, conn->recv_buffer + conn->recv_head, to_read);
    conn->recv_head += to_read;

    // Reset buffer pointers when all data consumed to reclaim space
    if (conn->recv_head == conn->recv_tail) {
        conn->recv_head = 0;
        conn->recv_tail = 0;
    }

    return to_read;
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
void tcp_conn_set_state_callback(void* conn_ptr, void (*callback)(uint8_t, uint8_t)) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn) {
        conn->on_state_change = callback;
    }
}

// ============================================================================
// TCP Listen/Accept Implementation (Server-Side Sockets)
// ============================================================================

static tcp_listener_t tcp_listeners[TCP_MAX_LISTENERS];

/**
 * Find a listener for the given local port.
 */
tcp_listener_t* tcp_find_listener(uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (tcp_listeners[i].in_use && tcp_listeners[i].port == port) {
            return &tcp_listeners[i];
        }
    }
    return NULL;
}

/**
 * Start listening for incoming connections on a port.
 * Returns a listener ID (>=0) on success, -1 on failure.
 */
int tcp_listen(uint16_t port, uint32_t bind_ip) {
    if (port == 0) return -1;

    /* Check if already listening on this port */
    if (tcp_find_listener(port)) {
        s_printf("[TCP] Port %d already in listen\n", port);
        return -1;
    }

    /* Find a free listener slot */
    int slot = -1;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!tcp_listeners[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        s_printf("[TCP] No free listener slots\n");
        return -1;
    }

    /* Initialize the listener */
    memset(&tcp_listeners[slot], 0, sizeof(tcp_listener_t));
    tcp_listeners[slot].in_use = 1;
    tcp_listeners[slot].port = port;
    tcp_listeners[slot].bind_ip = bind_ip;
    tcp_listeners[slot].pending_count = 0;
    tcp_listeners[slot].established_count = 0;

    s_printf("[TCP] Listening on port %d\n", port);
    return slot;
}

/**
 * Accept a pending connection from a listener.
 * Returns 0 on success (out_conn set), -1 if no connections ready.
 */
int tcp_accept(int listener_id, tcp_connection_t** out_conn) {
    if (listener_id < 0 || listener_id >= TCP_MAX_LISTENERS) return -1;
    if (!out_conn) return -1;

    tcp_listener_t* listener = &tcp_listeners[listener_id];
    if (!listener->in_use) return -1;

    /* Check if there are any established connections */
    if (listener->established_count <= 0) {
        return -1;  /* No connections ready */
    }

    /* Dequeue the first established connection */
    tcp_connection_t* conn = listener->established[0];
    *out_conn = conn;

    /* Shift the array */
    for (int i = 1; i < listener->established_count; i++) {
        listener->established[i - 1] = listener->established[i];
    }
    listener->established_count--;
    listener->established[listener->established_count] = NULL;

    s_printf("[TCP] Accepted connection on port %d from remote\n", listener->port);
    return 0;
}

/**
 * Close a listener and free all pending connections.
 */
int tcp_close_listener(int listener_id) {
    if (listener_id < 0 || listener_id >= TCP_MAX_LISTENERS) return -1;

    tcp_listener_t* listener = &tcp_listeners[listener_id];
    if (!listener->in_use) return -1;

    /* Close all pending connections */
    for (int i = 0; i < listener->pending_count; i++) {
        if (listener->pending[i]) {
            /* Send RST and close */
            tcp_send(listener->pending[i], TCP_RST, NULL, 0);
            listener->pending[i]->state = TCP_CLOSED;
            listener->pending[i] = NULL;
        }
    }

    /* Close all established connections */
    for (int i = 0; i < listener->established_count; i++) {
        if (listener->established[i]) {
            tcp_send(listener->established[i], TCP_FIN | TCP_ACK, NULL, 0);
            listener->established[i]->state = TCP_FIN_WAIT1;
            listener->established[i] = NULL;
        }
    }

    listener->in_use = 0;
    listener->pending_count = 0;
    listener->established_count = 0;

    s_printf("[TCP] Closed listener\n");
    return 0;
}

/**
 * Process incoming SYN packets for listeners.
 * Called from tcp_handle_packet when no existing connection is found.
 * This implements the server side of the TCP three-way handshake.
 */
static int tcp_handle_incoming_syn(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint32_t seq) {
    tcp_listener_t* listener = tcp_find_listener(dst_port);
    if (!listener) return -1;

    /* Check backlog */
    if (listener->pending_count >= TCP_LISTEN_BACKLOG) {
        s_printf("[TCP] Listen backlog full on port %d\n", dst_port);
        return -1;
    }

    /* Allocate a new connection for this incoming request */
    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) return -1;

    /* Set up the connection as SYN_RECEIVED */
    conn->state = TCP_SYN_RECEIVED;
    conn->local_ip = dst_ip;
    conn->remote_ip = src_ip;
    conn->local_port = dst_port;
    conn->remote_port = src_port;
    conn->rcv_nxt = seq + 1;
    conn->snd_nxt = (uint32_t)(timer_get_ticks() ^ (src_port << 16));  /* ISS */
    if (conn->snd_nxt == 0) conn->snd_nxt = 1;
    conn->connect_time = timer_get_ticks();
    conn->window = TCP_WINDOW_SIZE;
    conn->mss = TCP_MSS;

    /* Send SYN-ACK */
    tcp_send(conn, TCP_SYN | TCP_ACK, NULL, 0);
    conn->snd_nxt++;

    /* Add to pending list */
    listener->pending[listener->pending_count++] = conn;

    s_printf("[TCP] SYN received on port %d from remote, sent SYN-ACK\n", dst_port);
    return 0;
}

/**
 * Periodically process listeners: check if any pending connections
 * have completed the three-way handshake and move them to established.
 */
void tcp_process_listeners(void) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!tcp_listeners[i].in_use) continue;

        tcp_listener_t* listener = &tcp_listeners[i];

        /* Check pending connections for state changes */
        for (int j = 0; j < listener->pending_count; j++) {
            tcp_connection_t* conn = listener->pending[j];
            if (!conn) continue;

            if (conn->state == TCP_ESTABLISHED) {
                /* Move to established queue */
                if (listener->established_count < TCP_LISTEN_BACKLOG) {
                    listener->established[listener->established_count++] = conn;
                    s_printf("[TCP] Connection established on port %d\n", listener->port);
                }

                /* Remove from pending */
                for (int k = j; k < listener->pending_count - 1; k++) {
                    listener->pending[k] = listener->pending[k + 1];
                }
                listener->pending_count--;
                listener->pending[listener->pending_count] = NULL;
                j--;  /* Recheck this index */
            }
            else if (conn->state == TCP_CLOSED) {
                /* Connection was reset before completing handshake */
                for (int k = j; k < listener->pending_count - 1; k++) {
                    listener->pending[k] = listener->pending[k + 1];
                }
                listener->pending_count--;
                listener->pending[listener->pending_count] = NULL;
                j--;
            }
        }
    }
}

// ============================================================================
// TCP Retransmission Timer
// ============================================================================

/**
 * tcp_retransmit_check - Check all connections for retransmission timeout.
 *
 * Should be called periodically (e.g., from the timer tick or main loop).
 * For each active connection that has unacknowledged data and whose
 * retransmit timer has expired, retransmits the last segment.
 * Implements exponential backoff: each retransmit doubles the timeout.
 *
 * After TCP_MAX_RETRANSMIT attempts, the connection is closed with a RST.
 */
void tcp_retransmit_check(void)
{
    uint32_t now = timer_get_ticks();

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t* conn = &tcp_connections[i];

        /* Skip closed or idle connections */
        if (conn->state == TCP_CLOSED) continue;
        if (conn->retransmit_timeout == 0) continue;

        /* Check if the retransmit timer has expired */
        uint32_t elapsed = now - conn->last_ack_time;
        if (elapsed < conn->retransmit_timeout) continue;

        /* Too many retransmits — give up and close the connection */
        if (conn->retransmit_count >= TCP_MAX_RETRANSMIT) {
            s_printf("[TCP] Max retransmits exceeded for port %d, closing\n",
                     conn->remote_port);
            tcp_send(conn, TCP_RST, NULL, 0);
            conn->state = TCP_CLOSED;
            if (conn->on_state_change) {
                conn->on_state_change(TCP_ESTABLISHED, TCP_CLOSED);
            }
            continue;
        }

        /* Retransmit based on current state */
        conn->retransmit_count++;

        /* Exponential backoff: double the timeout for next retransmit */
        conn->retransmit_timeout *= 2;
        /* Cap at 30 seconds (assuming ~50 ticks/sec, that's 1500 ticks) */
        if (conn->retransmit_timeout > 1500) {
            conn->retransmit_timeout = 1500;
        }

        /* Reset the timer for the next check */
        conn->last_ack_time = now;

        switch (conn->state) {
            case TCP_SYN_SENT:
                /* Retransmit SYN */
                s_printf("[TCP] Retransmitting SYN to port %d (attempt %d)\n",
                         conn->remote_port, conn->retransmit_count);
                tcp_send(conn, TCP_SYN, NULL, 0);
                break;

            case TCP_SYN_RECEIVED:
                /* Retransmit SYN-ACK */
                s_printf("[TCP] Retransmitting SYN-ACK to port %d (attempt %d)\n",
                         conn->remote_port, conn->retransmit_count);
                tcp_send(conn, TCP_SYN | TCP_ACK, NULL, 0);
                break;

            case TCP_ESTABLISHED:
                /* Retransmit unacknowledged data from the send buffer.
                 * We resend from send_head up to send_tail in MSS-sized chunks,
                 * starting from the sequence number that hasn't been ACKed. */
                if (conn->send_tail > conn->send_head) {
                    s_printf("[TCP] Retransmitting data on port %d (attempt %d, %d bytes)\n",
                             conn->remote_port, conn->retransmit_count,
                             conn->send_tail - conn->send_head);

                    uint32_t data_len = conn->send_tail - conn->send_head;
                    uint32_t offset = 0;

                    /* Save and temporarily set snd_nxt to snd_una for retransmit */
                    uint32_t saved_nxt = conn->snd_nxt;
                    conn->snd_nxt = conn->snd_una;

                    while (offset < data_len) {
                        uint16_t chunk = (data_len - offset > TCP_MSS)
                                         ? TCP_MSS : (uint16_t)(data_len - offset);
                        tcp_send(conn, TCP_ACK | TCP_PSH,
                                 conn->send_buffer + conn->send_head + offset, chunk);
                        conn->snd_nxt += chunk;
                        offset += chunk;
                    }

                    /* Restore snd_nxt if it advanced beyond what we retransmitted */
                    if (saved_nxt > conn->snd_nxt) {
                        conn->snd_nxt = saved_nxt;
                    }
                } else {
                    /* No data in send buffer — just send a keep-alive ACK */
                    tcp_send(conn, TCP_ACK, NULL, 0);
                }
                break;

            case TCP_FIN_WAIT1:
                /* Retransmit FIN */
                s_printf("[TCP] Retransmitting FIN to port %d (attempt %d)\n",
                         conn->remote_port, conn->retransmit_count);
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                break;

            case TCP_LAST_ACK:
                /* Retransmit FIN */
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                break;

            default:
                /* Other states: no retransmit action needed */
                break;
        }
    }
}
