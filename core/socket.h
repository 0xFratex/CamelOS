// core/socket.h
#ifndef SOCKET_H
#define SOCKET_H

#include "../include/types.h"
#include "net_if.h"

// Domains
#define AF_INET     2

// Types
#define SOCK_STREAM 1 // TCP
#define SOCK_DGRAM  2 // UDP
#define SOCK_RAW    3

// Protocols
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_ICMP 1

// Socket States
#define SS_UNCONNECTED 0
#define SS_CONNECTING  1
#define SS_CONNECTED   2
#define SS_LISTENING   3
#define SS_CLOSED      4

// Socket connection states
#define SOCKET_UNCONNECTED 0
#define SOCKET_CONNECTING  1
#define SOCKET_CONNECTED   2
#define SOCKET_ERROR       3

// TCP States are defined in tcp.h

// Socket options
#define SOL_SOCKET      1
#define SO_RCVTIMEO     1
#define SO_SNDTIMEO     2
#define SO_REUSEADDR    3

// Types
typedef unsigned int socklen_t;

// Structures
struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} sockaddr_in_t;

extern uint16_t ephemeral_port;

// IP address conversion
int inet_aton(const char* cp, struct in_addr* inp);

// Socket Control Block
typedef struct socket_t {
    int id;
    int domain;
    int type;
    int protocol;
    int state;

    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    // TCP Specific
    uint32_t tcp_seq;
    uint32_t tcp_ack;
    int tcp_state;

    // Receive Buffer (Circular FIFO)
    uint8_t* rx_buffer;
    uint32_t rx_size;
    uint32_t rx_head;
    uint32_t rx_tail;

    // Timeouts (in ms)
    uint32_t recv_timeout;
    uint32_t send_timeout;

    struct socket_t* next;
} k_socket_t;

// Kernel Internal API
void socket_init_system();
int k_socket(int domain, int type, int protocol);
int k_bind(int sockfd, const sockaddr_in_t* addr);
int k_connect(int sockfd, const sockaddr_in_t* addr);
int k_sendto(int sockfd, const void* buf, size_t len, int flags, const sockaddr_in_t* dest);
int k_recvfrom(int sockfd, void* buf, size_t len, int flags, sockaddr_in_t* src);
int k_setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int k_close(int sockfd);

// System call interfaces
int sys_socket(int domain, int type, int protocol);
int sys_bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int sys_connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t sys_send(int fd, const void* buf, size_t len, int flags);
ssize_t sys_recv(int fd, void* buf, size_t len, int flags);
int sys_close(int fd);
int sys_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);

// Callback from Network Stack
int socket_process_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, int proto);

// TCP socket setup
void socket_setup_tcp_callbacks(int fd);

#endif