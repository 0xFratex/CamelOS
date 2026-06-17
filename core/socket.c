// core/socket.c - Optimized socket implementation
#include "socket.h"
#include "net.h"
#include "tcp.h"
#include "memory.h"
#include "string.h"
#include "../hal/cpu/timer.h"
#include "../hal/drivers/serial.h"

extern void net_poll(void);
extern uint32_t k_get_free_mem(void);
extern int tcp_send(void* conn, uint8_t flags, const void* data, uint32_t len);

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production
// ============================================================================
#define SOCKET_DEBUG_ENABLED   0
#define SOCKET_DEBUG_ERRORS    0    // Disable error logs for production

#define MAX_SOCKETS 64
#define SOCKET_TIMEOUT 10000 // 10 seconds for TCP operations
#define POLL_BATCH_SIZE 32   // Increased batch size for faster polling

typedef struct {
    int fd;
    int domain;
    int type;
    int protocol;
    uint8_t state;

    // Connection info
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    // Buffers
    uint8_t* recv_buffer;
    uint32_t recv_buffer_size;
    uint32_t recv_head;
    uint32_t recv_tail;

    uint8_t* send_buffer;
    uint32_t send_buffer_size;
    uint32_t send_head;
    uint32_t send_tail;

    // TCP state
    tcp_connection_t* tcp_conn;
    int listener_id;        // TCP listener slot ID (-1 if not listening)

    // Blocking/non-blocking
    int blocking;
    uint32_t timeout;

    // Event handlers
    void (*on_data)(int fd, uint8_t* data, uint32_t len);
    void (*on_connect)(int fd);
    void (*on_close)(int fd);
} socket_t;

static socket_t sockets[MAX_SOCKETS];
static int next_fd = 3; // Start after stdin/stdout/stderr

// Initialize socket system
void socket_init_system() {
    memset(sockets, 0, sizeof(sockets));
    next_fd = 3;
}

// Find free socket
static socket_t* socket_alloc() {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].fd == 0) {
            memset(&sockets[i], 0, sizeof(socket_t));
            sockets[i].fd = next_fd++;
            sockets[i].blocking = 1;
            sockets[i].timeout = SOCKET_TIMEOUT;
            sockets[i].listener_id = -1;

            // Allocate default buffers (32KB — needed for HTTP responses with TLS overhead)
            sockets[i].recv_buffer_size = 32768 + 16;
            sockets[i].recv_buffer = (uint8_t*)kmalloc(32768 + 16);
            sockets[i].send_buffer_size = 8192 + 16;
            sockets[i].send_buffer = (uint8_t*)kmalloc(8192 + 16);

            // Check for allocation failure
            if (!sockets[i].recv_buffer || !sockets[i].send_buffer) {
                s_printf("[MEM] socket_alloc FAILED free=");
                char buf[32];
                int_to_str(k_get_free_mem(), buf);
                s_printf(buf);
                s_printf("\n");
                // Free what was allocated
                if (sockets[i].recv_buffer) kfree(sockets[i].recv_buffer);
                if (sockets[i].send_buffer) kfree(sockets[i].send_buffer);
                sockets[i].fd = 0; // Mark as free
                return NULL;
            }

            return &sockets[i];
        }
    }
    return NULL;
}

// Find socket by fd
static socket_t* socket_get(int fd) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].fd == fd) {
            return &sockets[i];
        }
    }
    return NULL;
}

// Main socket() function
int k_socket(int domain, int type, int protocol) {
    if (domain != AF_INET) {
        return -1;
    }

    socket_t* sock = socket_alloc();
    if (!sock) {
        return -1;
    }

    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol;
    sock->state = SOCKET_UNCONNECTED;
    sock->local_ip = net_get_ip();

    return sock->fd;
}

// bind() function
int k_bind(int fd, const sockaddr_in_t* addr) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    sock->local_port = ntohs(addr->sin_port);

    // If port is 0, assign ephemeral port
    if (sock->local_port == 0) {
        sock->local_port = 49152 + (fd % 16384);
    }

    return 0;
}

// connect() function - OPTIMIZED
int k_connect(int fd, const sockaddr_in_t* addr) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    sock->remote_ip = addr->sin_addr;
    sock->remote_port = ntohs(addr->sin_port);

    // For TCP sockets
    if (sock->type == SOCK_STREAM) {
        char rip_str[16];
        extern void ip_to_str(uint32_t ip, char* buf);
        ip_to_str(sock->remote_ip, rip_str);
        s_printf("[TCP] k_connect: connecting to %s:%d\n", rip_str, sock->remote_port);

        // Check if network is configured
        extern uint32_t net_get_ip(void);
        uint32_t my_ip = net_get_ip();
        ip_to_str(my_ip, rip_str);
        s_printf("[TCP] Local IP: %s\n", rip_str);
        if (my_ip == 0) {
            s_printf("[TCP] ERROR: net_get_ip() returned 0 — network not configured\n");
            return -1;
        }

        // Establish TCP connection
        sock->tcp_conn = tcp_connect_with_ptr(sock->remote_ip, sock->remote_port);
        if (!sock->tcp_conn) {
            s_printf("[TCP] ERROR: tcp_connect_with_ptr returned NULL\n");
            return -1;
        }

        sock->local_port = tcp_conn_get_local_port(sock->tcp_conn);
        sock->state = SOCKET_CONNECTING;
        s_printf("[TCP] SYN sent, local_port=%d, waiting for SYN-ACK...\n", sock->local_port);

        // Wait for connection (blocking) - OPTIMIZED polling
        if (sock->blocking) {
            uint32_t start = get_tick_count();
            uint32_t timeout_ticks = sock->timeout / 10; // Convert to ticks
            int loop_count = 0;
            uint16_t last_cbr = 0;

            while (sock->state != SOCKET_CONNECTED) {
                loop_count++;

                // Poll NIC — small batch to avoid CPU starvation
                for (int i = 0; i < 4; i++) {
                    net_poll();
                }

                // Check if connection is established
                if (tcp_conn_is_established(sock->tcp_conn)) {
                    sock->state = SOCKET_CONNECTED;
                    socket_setup_tcp_callbacks(fd);
                    s_printf("[TCP] Connection ESTABLISHED after %d ticks (loop=%d)\n", get_tick_count() - start, loop_count);
                    break;
                }

                // Check if the connection was closed by the remote side (RST or
                // other error). If the TCP state is CLOSED, SYN_SENT failed.
                // Return an error immediately instead of waiting for the full
                // 20-second timeout — this makes "connection refused" (RST) and
                // other SYN failures fail fast so the browser can show an error
                // page quickly.
                if (sock->tcp_conn) {
                    tcp_connection_t* conn = (tcp_connection_t*)sock->tcp_conn;
                    if (conn->state == TCP_CLOSED) {
                        sock->state = SOCKET_ERROR;
                        s_printf("[TCP] Connection CLOSED by remote (RST?) after %d ticks (loop=%d)\n",
                                 get_tick_count() - start, loop_count);
                        return -1;
                    }
                }

                // Every 50 iterations (~1 second), log the CBR register to see
                // if the NIC is receiving any packets during the wait.
                if ((loop_count % 50) == 0) {
                    extern uint16_t inw(uint16_t port);
                    uint16_t cbr = inw(0xc000 + 0x3A) % 32768;
                    uint16_t capr = inw(0xc000 + 0x38);
                    uint16_t read_off = (capr + 16) % 32768;
                    s_printf("[TCP] waiting... loop=%d elapsed=%d ticks cbr=%d read_off=%d (delta=%d)\n",
                             loop_count, get_tick_count() - start, cbr, read_off, (int)cbr - (int)read_off);
                    last_cbr = cbr;
                }

                // Check timeout
                uint32_t elapsed = get_tick_count() - start;
                if (elapsed > timeout_ticks) {
                    sock->state = SOCKET_ERROR;
                    s_printf("[TCP] TIMEOUT after %d ticks (conn state=%d, loop=%d)\n", elapsed,
                             sock->tcp_conn ? ((tcp_connection_t*)sock->tcp_conn)->state : -1, loop_count);
                    return -1;
                }

                // Process GUI events to prevent system freeze
                extern void http_process_events(void);
                http_process_events();
            }
        }
    }

    return 0;
}

// sendto() function
int k_sendto(int fd, const void* buf, size_t len, int flags, const sockaddr_in_t* dest_addr) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    uint32_t dest_ip;
    uint16_t dest_port;

    if (dest_addr) {
        dest_ip = dest_addr->sin_addr;
        dest_port = ntohs(dest_addr->sin_port);
    } else {
        if (sock->state != SOCKET_CONNECTED) return -1;
        dest_ip = sock->remote_ip;
        dest_port = sock->remote_port;
    }

    // For UDP sockets
    if (sock->type == SOCK_DGRAM) {
        // Assign local port if not bound
        if (sock->local_port == 0) {
            sock->local_port = 49152 + (fd % 16384);
        }

        return net_send_udp_packet(dest_ip, sock->local_port, dest_port, (const uint8_t*)buf, len);
    }

    // For TCP sockets
    if (sock->type == SOCK_STREAM && sock->tcp_conn) {
        return tcp_send_data(sock->tcp_conn, (uint8_t*)buf, len);
    }

    return -1;
}

// Callback for TCP data - OPTIMIZED with memcpy
static void socket_tcp_data_callback(uint8_t* data, uint16_t len, void* user_data) {
    socket_t* sock = (socket_t*)user_data;
    if (!sock || !sock->recv_buffer) return;
    
    // Calculate available space
    uint32_t available_space;
    if (sock->recv_tail >= sock->recv_head) {
        available_space = sock->recv_buffer_size - (sock->recv_tail - sock->recv_head) - 1;
    } else {
        available_space = sock->recv_head - sock->recv_tail - 1;
    }
    
    // Limit to available space
    if (len > available_space) {
        len = available_space;
    }
    
    // Add data to socket receive buffer - handle wrap-around
    uint32_t first_part = sock->recv_buffer_size - sock->recv_tail;
    if (first_part >= len) {
        // No wrap-around needed
        memcpy(sock->recv_buffer + sock->recv_tail, data, len);
        sock->recv_tail = (sock->recv_tail + len) % sock->recv_buffer_size;
    } else {
        // Wrap-around needed
        memcpy(sock->recv_buffer + sock->recv_tail, data, first_part);
        memcpy(sock->recv_buffer, data + first_part, len - first_part);
        sock->recv_tail = len - first_part;
    }
}

// Set up TCP callbacks for a socket
void socket_setup_tcp_callbacks(int fd) {
    socket_t* sock = socket_get(fd);
    if (!sock || !sock->tcp_conn) return;
    
    extern void tcp_conn_set_data_callback(void* conn, void (*callback)(uint8_t*, uint16_t, void*), void* user_data);
    tcp_conn_set_data_callback(sock->tcp_conn, socket_tcp_data_callback, sock);
}

// recvfrom() function - OPTIMIZED
// FIXED: Reduced POLL_BATCH_SIZE to prevent CPU starvation in nested loops.
// Previously, POLL_BATCH_SIZE=32 caused tls_recv_all -> k_recvfrom -> 32x rtl8139_poll()
// -> http_process_events -> timer_sleep(1) on every iteration, which added ~32ms
// of latency per recv attempt and caused the browser to appear frozen.
int k_recvfrom(int fd, void* buf, size_t len, int flags, sockaddr_in_t* src_addr) {
    socket_t* sock = socket_get(fd);
    if (!sock) {
        return -1;
    }

    // Calculate available data
    uint32_t available;
    if (sock->recv_tail >= sock->recv_head) {
        available = sock->recv_tail - sock->recv_head;
    } else {
        available = sock->recv_buffer_size - sock->recv_head + sock->recv_tail;
    }

    if (available == 0) {
        if (!sock->blocking) {
            return -1;
        }

        // Wait for data with optimized polling
        uint32_t start = get_tick_count();
        uint32_t timeout_ticks = sock->timeout / 10;

        while (available == 0) {
            // Poll NIC a small number of times — just enough to process
            // any pending packets without excessive CPU usage
            for (int i = 0; i < 4; i++) {
                net_poll();
            }

            // Recalculate available data AFTER polling, so we pick up any
            // data that was just delivered via the TCP on_data callback.
            // This MUST happen before the EOF check below — otherwise, if
            // the data packet and the FIN packet arrive in the same poll
            // batch, the EOF check fires and returns 0, silently discarding
            // the data that was just added to the recv buffer.
            if (sock->recv_tail >= sock->recv_head) {
                available = sock->recv_tail - sock->recv_head;
            } else {
                available = sock->recv_buffer_size - sock->recv_head + sock->recv_tail;
            }

            // If we got data, break out and return it to the caller.
            if (available > 0) {
                break;
            }

            // No data yet. Check if the TCP connection was closed by the
            // remote side. When the server sends FIN, the connection enters
            // TCP_CLOSE_WAIT (or TCP_CLOSED if we also sent our FIN). In
            // either case, no more data will arrive — return 0 (end of
            // stream) so the caller (browser's recv loop) knows the response
            // is complete.
            //
            // This check runs ONLY when available == 0, so we never discard
            // buffered data by returning EOF prematurely.
            if (sock->type == SOCK_STREAM && sock->tcp_conn) {
                tcp_connection_t* conn = (tcp_connection_t*)sock->tcp_conn;
                if (conn->state == TCP_CLOSE_WAIT ||
                    conn->state == TCP_CLOSED     ||
                    conn->state == TCP_TIME_WAIT  ||
                    conn->state == TCP_LAST_ACK) {
                    s_printf("[TCP] k_recvfrom: connection closed by remote (state=%d), returning EOF\n",
                             conn->state);
                    return 0;  // End of stream
                }
            }

            // Check timeout
            uint32_t elapsed = get_tick_count() - start;
            if (elapsed > timeout_ticks) {
                return -1;  // Timeout
            }

            // Process GUI events to prevent system freeze
            extern void http_process_events(void);
            http_process_events();
        }
    }

    // Read data - handle wrap-around
    uint32_t to_read = (available < len) ? available : len;
    uint8_t* buffer = (uint8_t*)buf;

    uint32_t first_part = sock->recv_buffer_size - sock->recv_head;
    if (first_part >= to_read) {
        // No wrap-around
        memcpy(buffer, sock->recv_buffer + sock->recv_head, to_read);
        sock->recv_head = (sock->recv_head + to_read) % sock->recv_buffer_size;
    } else {
        // Wrap-around
        memcpy(buffer, sock->recv_buffer + sock->recv_head, first_part);
        memcpy(buffer + first_part, sock->recv_buffer, to_read - first_part);
        sock->recv_head = to_read - first_part;
    }

    // Fill source address if requested
    if (src_addr) {
        src_addr->sin_family = AF_INET;
        src_addr->sin_addr = sock->remote_ip;
        src_addr->sin_port = htons(sock->remote_port);
    }

    return to_read;
}

// close() function
int k_close(int fd) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    // If this is a listening socket, close the listener
    if (sock->listener_id >= 0) {
        tcp_close_listener(sock->listener_id);
        sock->listener_id = -1;
    }

    // For TCP, properly close the connection
    if (sock->type == SOCK_STREAM && sock->tcp_conn) {
        tcp_connection_t* conn = sock->tcp_conn;

        // Clear callbacks FIRST to prevent use-after-free
        conn->on_data = NULL;
        conn->on_state_change = NULL;
        conn->callback_user_data = NULL;

        // Send FIN if connection is established
        if (conn->state == TCP_ESTABLISHED) {
            tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
            conn->state = TCP_FIN_WAIT1;
            // Arm a retransmit timeout so that if the peer never ACKs our
            // FIN (and tcp_retransmit_check never runs - now fixed in
            // hal/cpu/timer.c), the connection still transitions to
            // CLOSED within a bounded time and frees its slot in the
            // 32-entry tcp_connections[] table. Previously a lost FIN
            // could leave the slot in FIN_WAIT1 forever, eventually
            // causing tcp_alloc_connection() to return NULL after
            // ~32 navigations.
            conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
            conn->last_ack_time = timer_get_ticks();
        } else if (conn->state == TCP_CLOSE_WAIT) {
            tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
            conn->state = TCP_LAST_ACK;
            conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
            conn->last_ack_time = timer_get_ticks();
        } else {
            // For any other state, just mark closed immediately
            conn->state = TCP_CLOSED;
        }
    }

    // Free buffers
    if (sock->recv_buffer) kfree(sock->recv_buffer);
    if (sock->send_buffer) kfree(sock->send_buffer);

    // Clear socket
    memset(sock, 0, sizeof(socket_t));

    return 0;
}

// setsockopt() function
int k_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_RCVTIMEO:
                if (optlen >= sizeof(struct timeval)) {
                    struct timeval* tv = (struct timeval*)optval;
                    sock->timeout = tv->tv_sec * 1000 + tv->tv_usec / 1000;
                }
                break;

            case SO_SNDTIMEO:
                if (optlen >= sizeof(struct timeval)) {
                    struct timeval* tv = (struct timeval*)optval;
                    sock->timeout = tv->tv_sec * 1000 + tv->tv_usec / 1000;
                }
                break;
        }
    }

    return 0;
}

// Convenience helper to flip a socket into non-blocking mode. In non-blocking
// mode, k_recvfrom / k_sendto return -1 immediately when no data is available
// or the send buffer is full, instead of busy-waiting up to sock->timeout.
// Used by core/dns.c so the resolver's own per-retry budget is the real
// upper bound on a DNS lookup, not the socket layer's internal 10s wait.
int k_socket_set_nonblocking(int fd) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;
    sock->blocking = 0;
    return 0;
}

// Process incoming UDP packet - called from net.c
int socket_process_packet(uint8_t* data, uint32_t len, uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port, int protocol) {
    // Find socket matching this packet
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].fd != 0 && sockets[i].type == SOCK_DGRAM) {
            // For UDP, match local port
            if (sockets[i].local_port == dst_port) {
                // Add to receive buffer
                socket_t* sock = &sockets[i];
                
                // Calculate available space
                uint32_t available_space;
                if (sock->recv_tail >= sock->recv_head) {
                    available_space = sock->recv_buffer_size - (sock->recv_tail - sock->recv_head) - 1;
                } else {
                    available_space = sock->recv_head - sock->recv_tail - 1;
                }
                
                if (len <= available_space) {
                    // Store source info
                    sock->remote_ip = src_ip;
                    sock->remote_port = src_port;
                    
                    // Copy data
                    uint32_t first_part = sock->recv_buffer_size - sock->recv_tail;
                    if (first_part >= len) {
                        memcpy(sock->recv_buffer + sock->recv_tail, data, len);
                        sock->recv_tail += len;
                    } else {
                        memcpy(sock->recv_buffer + sock->recv_tail, data, first_part);
                        memcpy(sock->recv_buffer, data + first_part, len - first_part);
                        sock->recv_tail = len - first_part;
                    }
                    
                    // Call callback if set
                    if (sock->on_data) {
                        sock->on_data(sock->fd, data, len);
                    }
                }
                return 0;  // Packet handled
            }
        }
    }
    return -1;  // No matching socket
}

// listen() function - set socket to LISTEN state
int k_listen(int fd, int backlog) {
    (void)backlog;
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    // Only TCP sockets can listen
    if (sock->type != SOCK_STREAM) return -1;

    // Must be bound to a port
    if (sock->local_port == 0) return -1;

    // Call the TCP layer to start listening
    int listener_id = tcp_listen(sock->local_port, sock->local_ip);
    if (listener_id < 0) return -1;

    sock->listener_id = listener_id;
    sock->state = SOCKET_CONNECTED;  // Mark as listening/active

    s_printf("[SOCKET] Listening on port %d (listener_id=%d)\n", sock->local_port, listener_id);
    return 0;
}

// accept() function - accept an incoming connection on a listening socket
int k_accept(int fd, sockaddr_in_t* addr) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    // Must be a listening socket
    if (sock->listener_id < 0) return -1;

    // Process listeners to move pending connections to established queue
    tcp_process_listeners();

    // Poll the network to catch any pending SYN-ACK completions
    for (int i = 0; i < 4; i++) {
        net_poll();
    }
    tcp_process_listeners();

    // Try to accept a connection from the TCP layer
    tcp_connection_t* new_conn = NULL;
    if (tcp_accept(sock->listener_id, &new_conn) != 0) {
        // No connection ready
        if (sock->blocking) {
            // Block until a connection arrives or timeout
            uint32_t start = get_tick_count();
            uint32_t timeout_ticks = sock->timeout / 10;

            while (1) {
                for (int i = 0; i < 4; i++) {
                    net_poll();
                }
                tcp_process_listeners();

                if (tcp_accept(sock->listener_id, &new_conn) == 0) {
                    break;  // Got a connection!
                }

                uint32_t elapsed = get_tick_count() - start;
                if (elapsed > timeout_ticks) {
                    return -1;  // Timeout
                }

                // Process GUI events to prevent system freeze
                extern void http_process_events(void);
                http_process_events();
            }
        } else {
            return -1;  // Non-blocking, no connection ready
        }
    }

    if (!new_conn) return -1;

    // Create a new socket for the accepted connection
    socket_t* new_sock = socket_alloc();
    if (!new_sock) {
        // No free sockets - reject the connection
        new_conn->state = TCP_CLOSED;
        return -1;
    }

    new_sock->type = SOCK_STREAM;
    new_sock->protocol = IPPROTO_TCP;
    new_sock->tcp_conn = new_conn;
    new_sock->listener_id = -1;
    new_sock->local_ip = new_conn->local_ip;
    new_sock->local_port = new_conn->local_port;
    new_sock->remote_ip = new_conn->remote_ip;
    new_sock->remote_port = new_conn->remote_port;
    new_sock->state = SOCKET_CONNECTED;

    // Set up TCP callbacks for the new socket
    socket_setup_tcp_callbacks(new_sock->fd);

    // Fill in the peer address if requested
    if (addr) {
        addr->sin_family = AF_INET;
        addr->sin_addr = new_conn->remote_ip;
        addr->sin_port = htons(new_conn->remote_port);
    }

    s_printf("[SOCKET] Accepted connection fd=%d from ", new_sock->fd);
    // Print remote IP/port info would go here
    s_printf("\n");

    return new_sock->fd;
}

// getsockname() function
int k_getsockname(int fd, sockaddr_in_t* addr) {
    socket_t* sock = socket_get(fd);
    if (!sock || !addr) return -1;
    
    addr->sin_family = AF_INET;
    addr->sin_addr = sock->local_ip;
    addr->sin_port = htons(sock->local_port);
    
    return 0;
}

// getpeername() function
int k_getpeername(int fd, sockaddr_in_t* addr) {
    socket_t* sock = socket_get(fd);
    if (!sock || !addr) return -1;
    
    addr->sin_family = AF_INET;
    addr->sin_addr = sock->remote_ip;
    addr->sin_port = htons(sock->remote_port);
    
    return 0;
}

// Check if a socket has data available in its receive buffer
// Returns 1 if data is available, 0 if not, -1 on error
int k_socket_has_data(int fd) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;

    // Calculate available data
    uint32_t available;
    if (sock->recv_tail >= sock->recv_head) {
        available = sock->recv_tail - sock->recv_head;
    } else {
        available = sock->recv_buffer_size - sock->recv_head + sock->recv_tail;
    }

    return (available > 0) ? 1 : 0;
}

// Check if a socket is in listening state
// Returns 1 if listening, 0 if not, -1 on error
int k_socket_is_listening(int fd) {
    socket_t* sock = socket_get(fd);
    if (!sock) return -1;
    return (sock->listener_id >= 0) ? 1 : 0;
}
