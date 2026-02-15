// core/socket.c - Optimized socket implementation
#include "socket.h"
#include "net.h"
#include "tcp.h"
#include "memory.h"
#include "string.h"
#include "../hal/cpu/timer.h"
#include "../hal/drivers/serial.h"

extern void rtl8139_poll(void);

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production
// ============================================================================
#define SOCKET_DEBUG_ENABLED   0
#define SOCKET_DEBUG_ERRORS    0    // Disable error logs for production

#define MAX_SOCKETS 64
#define SOCKET_TIMEOUT 5000 // 5 seconds (reduced from 10)
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

            // Allocate default buffers
            sockets[i].recv_buffer_size = 8192;
            sockets[i].recv_buffer = (uint8_t*)kmalloc(8192);
            sockets[i].send_buffer_size = 8192;
            sockets[i].send_buffer = (uint8_t*)kmalloc(8192);

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
        // Establish TCP connection
        sock->tcp_conn = tcp_connect_with_ptr(sock->remote_ip, sock->remote_port);
        if (!sock->tcp_conn) {
            return -1;
        }
        
        sock->local_port = tcp_conn_get_local_port(sock->tcp_conn);
        sock->state = SOCKET_CONNECTING;

        // Wait for connection (blocking) - OPTIMIZED polling
        if (sock->blocking) {
            uint32_t start = get_tick_count();
            uint32_t timeout_ticks = sock->timeout / 10; // Convert to ticks
            
            while (sock->state != SOCKET_CONNECTED) {
                // Batch poll for efficiency
                for (int i = 0; i < POLL_BATCH_SIZE; i++) {
                    rtl8139_poll();
                }
                
                // Check if connection is established
                if (tcp_conn_is_established(sock->tcp_conn)) {
                    sock->state = SOCKET_CONNECTED;
                    socket_setup_tcp_callbacks(fd);
                    break;
                }
                
                // Check timeout
                uint32_t elapsed = get_tick_count() - start;
                if (elapsed > timeout_ticks) {
                    sock->state = SOCKET_ERROR;
                    return -1;
                }
                asm volatile("pause");
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
            // Batch poll for efficiency
            for (int i = 0; i < POLL_BATCH_SIZE; i++) {
                rtl8139_poll();
            }

            // Check timeout
            uint32_t elapsed = get_tick_count() - start;
            if (elapsed > timeout_ticks) {
                return -1;  // Timeout
            }

            // Recalculate available data
            if (sock->recv_tail >= sock->recv_head) {
                available = sock->recv_tail - sock->recv_head;
            } else {
                available = sock->recv_buffer_size - sock->recv_head + sock->recv_tail;
            }
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

    // For TCP, send FIN
    if (sock->type == SOCK_STREAM && sock->tcp_conn) {
        // TODO: Send TCP FIN
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
