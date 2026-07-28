// core/socket.c - BSD-like socket API for CamelOS
// Provides: k_socket, k_connect, k_sendto, k_recvfrom, k_close, k_accept,
//           k_bind, k_listen, k_socket_set_nonblocking
//
// TCP sockets are backed by tcp_connection_t (core/tcp.c).
// UDP sockets use a per-socket ring buffer fed by socket_process_packet().
//
// All receive paths poll the NIC (via net_poll → rtl8139_poll / rtl8169_poll)
// and drive the TCP retransmit timer so that handshakes and data transfer
// work correctly even when the caller is in a tight recv loop.

#include "socket.h"
#include "tcp.h"
#include "net.h"
#include "net_if.h"
#include "arp.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/timer.h"

// ============================================================================
// CONSTANTS
// ============================================================================
#define MAX_SOCKETS         64
#define UDP_RX_BUF_SIZE     8192    // Per-socket UDP receive buffer
#define UDP_MAX_DGRAMS      16      // Max queued datagrams per UDP socket
#define CONNECT_TIMEOUT     2000    // ~40 seconds at 50 Hz
#define RECV_BLOCK_TIMEOUT  500     // ~10 seconds at 50 Hz (blocking recv)

// Socket types
#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

// Socket states
#define SOCK_STATE_FREE         0
#define SOCK_STATE_CREATED      1
#define SOCK_STATE_CONNECTING   2
#define SOCK_STATE_CONNECTED    3
#define SOCK_STATE_LISTENING    4
#define SOCK_STATE_CLOSED       5

// ============================================================================
// UDP DATAGRAM QUEUE
// ============================================================================
typedef struct {
    uint8_t  data[UDP_RX_BUF_SIZE];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
    int      valid;
} udp_dgram_t;

// ============================================================================
// SOCKET STRUCTURE
// ============================================================================
typedef struct {
    int      state;
    int      type;              // SOCK_STREAM or SOCK_DGRAM
    int      nonblocking;

    // TCP backing
    tcp_connection_t* tcp_conn;
    uint16_t local_port;

    // UDP backing
    udp_dgram_t  udp_rx[UDP_MAX_DGRAMS];
    int          udp_rx_head;
    int          udp_rx_tail;
    int          udp_rx_count;

    // Bound address (for UDP servers / listeners)
    uint32_t bind_ip;
    uint16_t bind_port;

    // TCP listener id (for SOCK_STREAM servers)
    int      listener_id;

    // Remote address (filled by connect or recvfrom)
    uint32_t remote_ip;
    uint16_t remote_port;
} socket_t;

// ============================================================================
// GLOBAL SOCKET TABLE
// ============================================================================
static socket_t sockets[MAX_SOCKETS];
static int sockets_initialized = 0;

// ============================================================================
// EXTERNAL DECLARATIONS
// ============================================================================
extern void     net_poll(void);
extern void     tcp_retransmit_check(void);
extern void     tcp_process_listeners(void);
extern uint32_t net_get_ip(void);
extern int      net_send_udp_packet(uint32_t dest_ip, uint16_t src_port,
                                    uint16_t dest_port,
                                    const uint8_t* data, uint32_t len);
extern uint16_t htons(uint16_t v);
extern uint16_t ntohs(uint16_t v);
extern uint32_t htonl(uint32_t v);
extern uint32_t ntohl(uint32_t v);

// tcp.c exports
extern tcp_connection_t* tcp_connect_with_ptr(uint32_t remote_ip,
                                              uint16_t remote_port);
extern int      tcp_conn_recv(void* conn_ptr, void* buf, int max_len);
extern int      tcp_conn_send(void* conn_ptr, const void* data, int len);
extern int      tcp_conn_is_established(void* conn_ptr);
extern uint16_t tcp_conn_get_local_port(void* conn_ptr);
extern int      tcp_listen(uint16_t port, uint32_t bind_ip);
extern int      tcp_accept(int listener_id, tcp_connection_t* out_conn);
extern int      tcp_close_listener(int listener_id);

// ============================================================================
// INITIALIZATION
// ============================================================================
void socket_init_system(void) {
    memset(sockets, 0, sizeof(sockets));
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sockets[i].state = SOCK_STATE_FREE;
        sockets[i].listener_id = -1;
    }
    sockets_initialized = 1;
    s_printf("[SOCKET] Socket system initialized (%d slots)\n", MAX_SOCKETS);
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================
static socket_t* sock_get(int fd) {
    if (fd < 0 || fd >= MAX_SOCKETS) return 0;
    if (sockets[fd].state == SOCK_STATE_FREE) return 0;
    return &sockets[fd];
}

static int sock_alloc(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].state == SOCK_STATE_FREE) {
            memset(&sockets[i], 0, sizeof(socket_t));
            sockets[i].listener_id = -1;
            return i;
        }
    }
    return -1;
}

// Poll the NIC and drive TCP housekeeping.
// Called from every blocking loop so that incoming packets are processed
// and retransmit timers fire even when the application is stuck in recv().
static inline void sock_poll_network(void) {
    net_poll();               // → rtl8139_poll() or rtl8169_poll()
    tcp_retransmit_check();   // SYN / data / FIN retransmits
    tcp_process_listeners();  // move pending → established for accept()
}

// ============================================================================
// k_socket — create a new socket
// ============================================================================
int k_socket(int domain, int type, int protocol) {
    (void)domain;
    (void)protocol;

    if (!sockets_initialized) socket_init_system();

    int fd = sock_alloc();
    if (fd < 0) {
        s_printf("[SOCKET] k_socket: no free slots\n");
        return -1;
    }

    sockets[fd].state       = SOCK_STATE_CREATED;
    sockets[fd].type        = type;
    sockets[fd].nonblocking = 0;
    sockets[fd].tcp_conn    = 0;
    sockets[fd].listener_id = -1;

    return fd;
}

// ============================================================================
// k_connect — connect a TCP socket to a remote host
// ============================================================================
// Log evidence from the codebase:
//   [TCP] k_connect: connecting to 172.217.30.142:443
//   [TCP] Local IP: 10.0.2.15
//   [TCP] SYN sent, local_port=49152, waiting for SYN-ACK...
//   [TCP] waiting... loop=50 elapsed=49 ticks cbr=176 read_off=176
//   [TCP] Connection ESTABLISHED after 653 ticks (loop=651)
//
// The function:
//   1. Validates the socket and extracts the remote address.
//   2. Calls tcp_connect_with_ptr() which allocates a tcp_connection_t,
//      sends the SYN, and returns immediately (non-blocking at TCP level).
//   3. Enters a polling loop that drives the NIC and TCP retransmit timer
//      until the connection reaches TCP_ESTABLISHED or times out.
// ============================================================================
int k_connect(int fd, const sockaddr_in_t* addr) {
    socket_t* s = sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_STREAM) return -1;   // connect is TCP-only
    if (!addr) return -1;

    uint32_t remote_ip   = addr->sin_addr;   // already network byte order
    uint16_t remote_port = ntohs(addr->sin_port);

    s_printf("[TCP] k_connect: connecting to %d.%d.%d.%d:%d\n",
             (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
             (remote_ip >> 8)  & 0xFF,  remote_ip & 0xFF,
             remote_port);
    s_printf("[TCP] Local IP: %d.%d.%d.%d\n",
             (net_get_ip() >> 24) & 0xFF, (net_get_ip() >> 16) & 0xFF,
             (net_get_ip() >> 8)  & 0xFF,  net_get_ip() & 0xFF);

    // Kick off the TCP three-way handshake (sends SYN, returns conn ptr)
    tcp_connection_t* conn = tcp_connect_with_ptr(remote_ip, remote_port);
    if (!conn) {
        s_printf("[TCP] k_connect: tcp_connect_with_ptr failed\n");
        return -1;
    }

    s->tcp_conn    = conn;
    s->remote_ip   = remote_ip;
    s->remote_port = remote_port;
    s->local_port  = tcp_conn_get_local_port(conn);
    s->state       = SOCK_STATE_CONNECTING;

    s_printf("[TCP] SYN sent, local_port=%d, waiting for SYN-ACK...\n",
             s->local_port);

    // ---- Polling loop: wait for ESTABLISHED ----
    uint32_t start = get_tick_count();
    int loop = 0;

    while (1) {
        // Drive the NIC and TCP state machine
        sock_poll_network();

        // Check connection state
        if (tcp_conn_is_established(conn)) {
            uint32_t elapsed = get_tick_count() - start;
            s_printf("[TCP] Connection ESTABLISHED after %d ticks (loop=%d)\n",
                     elapsed, loop);
            s->state = SOCK_STATE_CONNECTED;
            return 0;
        }

        // If the TCP layer closed the connection (RST, timeout, etc.)
        if (conn->state == TCP_CLOSED) {
            s_printf("[TCP] k_connect: connection closed during handshake\n");
            s->state   = SOCK_STATE_CLOSED;
            s->tcp_conn = 0;
            return -1;
        }

        // Timeout check
        uint32_t elapsed = get_tick_count() - start;
        if (elapsed > CONNECT_TIMEOUT) {
            s_printf("[TCP] TIMEOUT after %d ticks\n", elapsed);
            // Force-close the half-open connection
            conn->state = TCP_CLOSED;
            s->state    = SOCK_STATE_CLOSED;
            s->tcp_conn = 0;
            return -1;
        }

        // Progress logging every 50 iterations (~1 second)
        loop++;
        if ((loop % 50) == 0) {
            extern uint16_t rtl8139_get_cbr(void);  // optional debug
            s_printf("[TCP] waiting... loop=%d elapsed=%d ticks\n",
                     loop, elapsed);
        }

        // Brief pause to avoid hammering the bus
        for (volatile int i = 0; i < 200; i++)
            asm volatile("pause");
    }

    // unreachable
    return -1;
}

// ============================================================================
// k_sendto — send data on a TCP or UDP socket
// ============================================================================
int k_sendto(int fd, const void* data, size_t len, int flags,
             const sockaddr_in_t* dest) {
    (void)flags;
    socket_t* s = sock_get(fd);
    if (!s || !data || len <= 0) return -1;

    if (s->type == SOCK_STREAM) {
        // ---- TCP send ----
        if (!s->tcp_conn) return -1;
        if (s->tcp_conn->state != TCP_ESTABLISHED &&
            s->tcp_conn->state != TCP_CLOSE_WAIT) {
            return -1;
        }
        return tcp_conn_send(s->tcp_conn, data, len);

    } else if (s->type == SOCK_DGRAM) {
        // ---- UDP send ----
        uint32_t dip;
        uint16_t dport;

        if (dest) {
            dip   = dest->sin_addr;
            dport = ntohs(dest->sin_port);
        } else {
            // Use the address from a previous connect() or sendto()
            dip   = s->remote_ip;
            dport = s->remote_port;
        }

        if (dip == 0 || dport == 0) return -1;

        // Assign an ephemeral local port if not yet bound
        if (s->local_port == 0) {
            static uint16_t udp_next_port = 49200;
            s->local_port = udp_next_port++;
            if (udp_next_port > 65535) udp_next_port = 49200;
        }

        int ret = net_send_udp_packet(dip, s->local_port, dport,
                                      (const uint8_t*)data, (uint32_t)len);
        return (ret == 0) ? len : -1;
    }

    return -1;
}

// ============================================================================
// k_recvfrom — receive data from a TCP or UDP socket
// ============================================================================
// Return values (matching the semantics observed in the codebase):
//   > 0  : bytes read
//   == 0 : TCP connection closed by peer (EOF)
//   < 0  : no data available (non-blocking) or error
//
// For TCP the function:
//   1. Polls the NIC so new segments reach the TCP recv_buffer.
//   2. Drives the TCP retransmit timer.
//   3. Reads from tcp_conn_recv().
//   4. Detects EOF when the connection is CLOSED / TIME_WAIT.
//
// For UDP the function:
//   1. Polls the NIC.
//   2. Dequeues the oldest datagram from the per-socket ring buffer.
// ============================================================================
int k_recvfrom(int fd, void* buf, size_t len, int flags,
               sockaddr_in_t* addr) {
    (void)flags;
    socket_t* s = sock_get(fd);
    if (!s || !buf || len <= 0) return -1;

    if (s->type == SOCK_STREAM) {
        // ===================== TCP RECV =====================
        tcp_connection_t* conn = s->tcp_conn;
        if (!conn) return -1;

        // Poll the NIC and TCP housekeeping
        sock_poll_network();

        // Try to read from the TCP receive buffer
        int n = tcp_conn_recv(conn, buf, len);
        if (n > 0) {
            return n;
        }

        // No data in buffer.  Check whether the connection is still alive.
        // TCP_CLOSED  → peer sent FIN and we ACKed, or RST received
        // TCP_TIME_WAIT → connection fully torn down
        if (conn->state == TCP_CLOSED || conn->state == TCP_TIME_WAIT) {
            // If there is still unread data in the buffer, return it first
            // (tcp_conn_recv already returned 0 above, so truly empty).
            s_printf("[TCP] k_recvfrom: connection closed by remote "
                     "(state=%d), returning EOF\n", conn->state);
            return 0;   // EOF
        }

        // Connection is alive but no data yet.
        if (s->nonblocking) {
            return -1;  // EWOULDBLOCK
        }

        // Blocking mode: wait up to RECV_BLOCK_TIMEOUT for data.
        uint32_t start = get_tick_count();
        while ((get_tick_count() - start) < RECV_BLOCK_TIMEOUT) {
            sock_poll_network();

            n = tcp_conn_recv(conn, buf, len);
            if (n > 0) return n;

            if (conn->state == TCP_CLOSED || conn->state == TCP_TIME_WAIT) {
                return 0;  // EOF
            }

            // Brief pause
            for (volatile int i = 0; i < 300; i++)
                asm volatile("pause");
        }

        return -1;  // timeout, no data

    } else if (s->type == SOCK_DGRAM) {
        // ===================== UDP RECV =====================

        // Poll the NIC so socket_process_packet() can enqueue datagrams
        sock_poll_network();

        // Dequeue the oldest datagram
        if (s->udp_rx_count == 0) {
            if (s->nonblocking) return -1;

            // Blocking: wait for a datagram
            uint32_t start = get_tick_count();
            while ((get_tick_count() - start) < RECV_BLOCK_TIMEOUT) {
                sock_poll_network();
                if (s->udp_rx_count > 0) break;
                for (volatile int i = 0; i < 300; i++)
                    asm volatile("pause");
            }
            if (s->udp_rx_count == 0) return -1;  // timeout
        }

        // Pop from ring buffer
        udp_dgram_t* dg = &s->udp_rx[s->udp_rx_head];
        if (!dg->valid) return -1;

        int copy_len = (dg->len < (uint16_t)len) ? dg->len : (uint16_t)len;
        memcpy(buf, dg->data, copy_len);

        // Fill source address if requested
        if (addr) {
            addr->sin_family = AF_INET;
            addr->sin_port   = htons(dg->src_port);
            addr->sin_addr   = dg->src_ip;
        }

        dg->valid = 0;
        s->udp_rx_head = (s->udp_rx_head + 1) % UDP_MAX_DGRAMS;
        s->udp_rx_count--;

        return copy_len;
    }

    return -1;
}

// ============================================================================
// k_bind — bind a socket to a local address/port
// ============================================================================
int k_bind(int fd, const sockaddr_in_t* addr) {
    socket_t* s = sock_get(fd);
    if (!s || !addr) return -1;

    s->bind_ip   = addr->sin_addr;
    s->bind_port = ntohs(addr->sin_port);
    s->local_port = s->bind_port;

    return 0;
}

// ============================================================================
// k_listen — mark a TCP socket as a passive listener
// ============================================================================
int k_listen(int fd, int backlog) {
    (void)backlog;
    socket_t* s = sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_STREAM) return -1;
    if (s->bind_port == 0) return -1;  // must bind first

    int lid = tcp_listen(s->bind_port, s->bind_ip);
    if (lid < 0) return -1;

    s->listener_id = lid;
    s->state       = SOCK_STATE_LISTENING;

    s_printf("[SOCKET] Listening on port %d (listener=%d)\n",
             s->bind_port, lid);
    return 0;
}

// ============================================================================
// k_accept — accept an incoming TCP connection
// ============================================================================
// Returns a NEW file descriptor for the established connection, or -1 if
// no pending connections are available.
//
// The function:
//   1. Polls the NIC and TCP listener queue.
//   2. Calls tcp_accept() to dequeue an established tcp_connection_t.
//   3. Allocates a new socket fd and attaches the connection to it.
// ============================================================================
int k_accept(int fd, sockaddr_in_t* client_addr) {
    socket_t* s = sock_get(fd);
    if (!s) return -1;
    if (s->state != SOCK_STATE_LISTENING) return -1;
    if (s->listener_id < 0) return -1;

    // Poll until a connection is ready (or timeout)
    uint32_t start = get_tick_count();
    while ((get_tick_count() - start) < RECV_BLOCK_TIMEOUT) {
        sock_poll_network();

        tcp_connection_t conn_copy;
        memset(&conn_copy, 0, sizeof(conn_copy));

        if (tcp_accept(s->listener_id, &conn_copy) == 0) {
            // Got an established connection — allocate a new socket for it
            int new_fd = sock_alloc();
            if (new_fd < 0) {
                s_printf("[SOCKET] k_accept: no free socket slots\n");
                return -1;
            }

            // Find the actual connection in the TCP table by matching
            // the 4-tuple, so we hold a stable pointer (conn_copy is a
            // stack copy and would dangle).
            tcp_connection_t* real_conn = 0;
            extern tcp_connection_t tcp_connections[];  // from tcp.c
            for (int i = 0; i < 32; i++) {
                if (tcp_connections[i].state != TCP_CLOSED &&
                    tcp_connections[i].local_port  == conn_copy.local_port &&
                    tcp_connections[i].remote_port == conn_copy.remote_port &&
                    tcp_connections[i].remote_ip   == conn_copy.remote_ip) {
                    real_conn = &tcp_connections[i];
                    break;
                }
            }

            if (!real_conn) {
                s_printf("[SOCKET] k_accept: could not find live connection\n");
                sockets[new_fd].state = SOCK_STATE_FREE;
                return -1;
            }

            sockets[new_fd].state       = SOCK_STATE_CONNECTED;
            sockets[new_fd].type        = SOCK_STREAM;
            sockets[new_fd].tcp_conn    = real_conn;
            sockets[new_fd].local_port  = real_conn->local_port;
            sockets[new_fd].remote_ip   = real_conn->remote_ip;
            sockets[new_fd].remote_port = real_conn->remote_port;
            sockets[new_fd].nonblocking = s->nonblocking;  // inherit

            s_printf("[SOCKET] Accepted connection on port %d → fd=%d\n",
                     s->bind_port, new_fd);

            // Fill client address if requested
            if (client_addr) {
                client_addr->sin_family = AF_INET;
                client_addr->sin_port   = htons(real_conn->remote_port);
                client_addr->sin_addr   = real_conn->remote_ip;
            }

            return new_fd;
        }

        // No connection ready yet
        if (s->nonblocking) return -1;

        for (volatile int i = 0; i < 300; i++)
            asm volatile("pause");
    }

    return -1;  // timeout
}

// ============================================================================
// k_close — close a socket
// ============================================================================
int k_close(int fd) {
    socket_t* s = sock_get(fd);
    if (!s) return -1;

    if (s->type == SOCK_STREAM && s->tcp_conn) {
        tcp_connection_t* conn = s->tcp_conn;

        if (conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT) {
            // Initiate graceful close: send FIN
            extern int tcp_send(tcp_connection_t*, uint8_t, uint8_t*, uint16_t);
            tcp_send(conn, TCP_FIN | TCP_ACK, 0, 0);
            conn->snd_nxt++;
            conn->state = (conn->state == TCP_ESTABLISHED)
                        ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
            conn->last_ack_time    = get_tick_count();
            conn->retransmit_timeout = 100;  // arm FIN retransmit
            conn->retransmit_count   = 0;
        } else {
            conn->state = TCP_CLOSED;
        }

        s->tcp_conn = 0;
    }

    if (s->type == SOCK_STREAM && s->listener_id >= 0) {
        tcp_close_listener(s->listener_id);
        s->listener_id = -1;
    }

    // Clear the slot
    memset(s, 0, sizeof(socket_t));
    s->state       = SOCK_STATE_FREE;
    s->listener_id = -1;

    return 0;
}

// ============================================================================
// k_socket_set_nonblocking — toggle non-blocking mode
// ============================================================================
int k_socket_set_nonblocking(int fd) {
    socket_t* s = sock_get(fd);
    if (!s) return -1;
    s->nonblocking = 1;
    return 0;
}

int k_socket_set_blocking(int fd) {
    socket_t* s = sock_get(fd);
    if (!s) return -1;
    s->nonblocking = 0;
    return 0;
}

// ============================================================================
// socket_process_packet — called from net_handle_packet() for UDP
// ============================================================================
// Delivers an incoming UDP datagram to the matching socket's ring buffer.
// Called from the NIC ISR / poll path (net.c → net_handle_packet).
//
//   payload      – UDP payload (after the 8-byte UDP header)
//   payload_len  – length of payload
//   src_ip       – source IP   (host byte order, from ntohl in net.c)
//   src_port     – source port (host byte order)
//   dst_ip       – dest IP     (host byte order)
//   dst_port     – dest port   (host byte order)
//   proto        – IP protocol (IPPROTO_UDP = 17)
// ============================================================================
int socket_process_packet(uint8_t* payload, uint32_t payload_len,
                          uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port,
                          int proto) {
    if (proto != 17) return 0;  // UDP only

    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_t* s = &sockets[i];
        if (s->state == SOCK_STATE_FREE) continue;
        if (s->type != SOCK_DGRAM) continue;

        // Match on local port.  If the socket is bound to a specific IP,
        // also match on that; otherwise accept any destination IP.
        if (s->local_port != dst_port && s->bind_port != dst_port)
            continue;
        if (s->bind_ip != 0 && s->bind_ip != dst_ip)
            continue;

        // Enqueue the datagram
        if (s->udp_rx_count >= UDP_MAX_DGRAMS) {
            // Buffer full — drop the oldest datagram
            s->udp_rx_head = (s->udp_rx_head + 1) % UDP_MAX_DGRAMS;
            s->udp_rx_count--;
        }

        udp_dgram_t* dg = &s->udp_rx[s->udp_rx_tail];
        uint16_t copy_len = (payload_len > UDP_RX_BUF_SIZE)
                          ? UDP_RX_BUF_SIZE : (uint16_t)payload_len;
        memcpy(dg->data, payload, copy_len);
        dg->len      = copy_len;
        dg->src_ip   = src_ip;
        dg->src_port = src_port;
        dg->valid    = 1;

        s->udp_rx_tail = (s->udp_rx_tail + 1) % UDP_MAX_DGRAMS;
        s->udp_rx_count++;

        return 0;  // delivered to first matching socket
    }

    // No matching socket — silently drop
    return 0;
}

// ============================================================================
// k_socket_has_data — check if a socket has pending data (for select/poll)
// ============================================================================
int k_socket_has_data(int fd) {
    socket_t* s = sock_get(fd);
    if (!s) return 0;

    if (s->type == SOCK_STREAM) {
        if (!s->tcp_conn) return 0;
        // Check if there's data in the TCP receive buffer
        int available = (int)(s->tcp_conn->recv_tail - s->tcp_conn->recv_head);
        return (available > 0) ? 1 : 0;
    } else if (s->type == SOCK_DGRAM) {
        return (s->udp_rx_count > 0) ? 1 : 0;
    }
    return 0;
}

// ============================================================================
// k_socket_is_listening — check if a socket is in listening state
// ============================================================================
int k_socket_is_listening(int fd) {
    socket_t* s = sock_get(fd);
    if (!s) return 0;
    return (s->state == SOCK_STATE_LISTENING) ? 1 : 0;
}