// core/select.h - POSIX-compatible select/poll for CamelOS
#ifndef SELECT_H
#define SELECT_H

#include "../include/types.h"

// ---------------------------------------------------------------------------
// fd_set definitions
// ---------------------------------------------------------------------------
#define FD_SETSIZE 64

typedef struct {
    long fds_array[FD_SETSIZE / (8 * sizeof(long))];
} fd_set;

// fd_set manipulation macros
#define FD_ZERO(set) do { \
    int _i; \
    for (_i = 0; _i < (int)(FD_SETSIZE / (8 * sizeof(long))); _i++) \
        (set)->fds_array[_i] = 0; \
} while (0)

#define FD_SET(fd, set) do { \
    if ((fd) >= 0 && (fd) < FD_SETSIZE) \
        (set)->fds_array[(fd) / (8 * sizeof(long))] |= (1L << ((fd) % (8 * sizeof(long)))); \
} while (0)

#define FD_CLR(fd, set) do { \
    if ((fd) >= 0 && (fd) < FD_SETSIZE) \
        (set)->fds_array[(fd) / (8 * sizeof(long))] &= ~(1L << ((fd) % (8 * sizeof(long)))); \
} while (0)

#define FD_ISSET(fd, set) \
    (((fd) >= 0 && (fd) < FD_SETSIZE) ? \
        ((set)->fds_array[(fd) / (8 * sizeof(long))] & (1L << ((fd) % (8 * sizeof(long))))) != 0 : 0)

// ---------------------------------------------------------------------------
// struct timeval (guard against redefinition from socket.h)
// ---------------------------------------------------------------------------
#ifndef _STRUCT_TIMEVAL_DEFINED
#define _STRUCT_TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

// ---------------------------------------------------------------------------
// poll() definitions
// ---------------------------------------------------------------------------
typedef struct {
    int   fd;
    short events;
    short revents;
} pollfd_t;

#define POLLIN   0x0001
#define POLLOUT  0x0002
#define POLLERR  0x0004
#define POLLHUP  0x0008
#define POLLNVAL 0x0010

// ---------------------------------------------------------------------------
// Kernel API
// ---------------------------------------------------------------------------
int k_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds, struct timeval* timeout);
int k_poll(pollfd_t* fds, uint32_t nfds, int timeout_ms);

#endif /* SELECT_H */
