#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <types.h> // uint32_t etc

#define AF_INET 2
#define SOCK_DGRAM 2
#define SOCK_STREAM 1

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} sockaddr_in;

// API macros forwarding to kernel pointer (implied global `sys`)
#define socket(d,t,p) sys->socket(d,t,p)
#define bind(s,a,al) sys->bind(s,a,al)
#define connect(s,a,al) sys->connect(s,a,al)
#define sendto(s,b,l,f,d,dl) sys->sendto(s,b,l,f,d,dl)
#define recvfrom(s,b,l,f,sa,al) sys->recvfrom(s,b,l,f,sa,al)
#define close(fd) sys->close(fd)

#endif