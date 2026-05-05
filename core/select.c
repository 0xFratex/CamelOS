// core/select.c - POSIX-compatible select/poll for CamelOS
#include "select.h"
#include "socket.h"
#include "net.h"
#include "string.h"
#include "memory.h"
#include "tcp.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/timer.h"

// ---------------------------------------------------------------------------
// Externals from other modules
// ---------------------------------------------------------------------------
extern void rtl8139_poll(void);
extern int  k_socket_has_data(int fd);
extern void tcp_process_listeners(void);

// ---------------------------------------------------------------------------
// Helper: poll the network stack once (batch poll + TCP listener processing)
// ---------------------------------------------------------------------------
static void poll_network_once(void) {
    for (int i = 0; i < 32; i++) {
        rtl8139_poll();
    }
    tcp_process_listeners();
}

// ---------------------------------------------------------------------------
// k_select - POSIX-compatible select()
//
// Iterates fd_sets from 0 to nfds-1.
//   readfds  : keeps bit set only if socket has data available
//   writefds : always keeps bit set (non-blocking send)
//   errorfds : clears all bits (no error detection yet)
//
// If no fds are ready:
//   - timeout == NULL          : polls network indefinitely until data arrives
//   - timeout tv_sec==0 && tv_usec==0 : single non-blocking check, returns 0
//   - otherwise                : polls network until timeout expires or data ready
//
// Returns count of ready fds, or -1 on error.
// ---------------------------------------------------------------------------
int k_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds, struct timeval* timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        return -1;
    }

    // Compute the timeout deadline in ticks (0 = no wait, -1 = infinite)
    int32_t deadline_ticks;   // -1 = infinite, 0 = already expired
    uint32_t start_tick = 0;

    if (timeout == NULL) {
        deadline_ticks = -1;  // infinite
    } else {
        uint32_t ms = (uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
        if (ms == 0) {
            deadline_ticks = 0;  // non-blocking: single check only
        } else {
            // Convert ms to ticks (assuming ~100 Hz timer, 1 tick = 10 ms)
            deadline_ticks = (int32_t)(ms / 10);
            if (deadline_ticks == 0) deadline_ticks = 1;
            start_tick = get_tick_count();
        }
    }

    int ready_count;

    for (;;) {
        ready_count = 0;

        // Check readfds
        if (readfds) {
            for (int fd = 0; fd < nfds; fd++) {
                if (FD_ISSET(fd, readfds)) {
                    int has_data = k_socket_has_data(fd);
                    if (has_data == 1) {
                        ready_count++;
                    } else {
                        // Socket has no data (or doesn't exist) – clear the bit
                        FD_CLR(fd, readfds);
                    }
                }
            }
        }

        // Check writefds – sockets are always writable in CamelOS
        if (writefds) {
            for (int fd = 0; fd < nfds; fd++) {
                if (FD_ISSET(fd, writefds)) {
                    // Verify the socket exists
                    int has_data = k_socket_has_data(fd);
                    if (has_data >= 0) {
                        // Socket exists – always writable
                        ready_count++;
                    } else {
                        // Socket doesn't exist – clear the bit
                        FD_CLR(fd, writefds);
                    }
                }
            }
        }

        // Check errorfds – no error detection mechanism yet, clear all
        if (errorfds) {
            for (int fd = 0; fd < nfds; fd++) {
                if (FD_ISSET(fd, errorfds)) {
                    FD_CLR(fd, errorfds);
                }
            }
        }

        // If fds are ready, return immediately
        if (ready_count > 0) {
            return ready_count;
        }

        // Handle timeout logic
        if (timeout != NULL) {
            uint32_t ms = (uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
            if (ms == 0) {
                // Non-blocking check – return 0 if nothing ready
                return 0;
            }
            // Check if deadline has passed
            uint32_t elapsed = get_tick_count() - start_tick;
            if (elapsed >= (uint32_t)deadline_ticks) {
                return 0;  // Timeout expired, no fds ready
            }
        }

        // Poll the network and try again
        poll_network_once();
    }

    // Unreachable, but keeps compiler happy
    return -1;
}

// ---------------------------------------------------------------------------
// k_poll - poll() with pollfd_t structures
//
//   POLLIN   : data available to read
//   POLLOUT  : socket is writable (always true in CamelOS)
//   POLLNVAL : invalid fd (socket not found)
//
// timeout_ms:
//    -1  : wait indefinitely
//     0  : non-blocking, single check
//    >0  : wait up to timeout_ms milliseconds
//
// Returns count of fds with non-zero revents, or -1 on error.
// ---------------------------------------------------------------------------
int k_poll(pollfd_t* fds, uint32_t nfds, int timeout_ms) {
    if (!fds && nfds > 0) {
        return -1;
    }

    // Compute deadline in ticks
    int32_t deadline_ticks = 0;  // 0 = non-blocking single check
    uint32_t start_tick = 0;

    if (timeout_ms == -1) {
        deadline_ticks = -1;  // infinite
    } else if (timeout_ms > 0) {
        deadline_ticks = (int32_t)(timeout_ms / 10);
        if (deadline_ticks == 0) deadline_ticks = 1;
        start_tick = get_tick_count();
    }

    int ready_count;

    for (;;) {
        ready_count = 0;

        for (uint32_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;

            // Skip entries with fd < 0 (negative fd is ignored per POSIX)
            if (fds[i].fd < 0) {
                continue;
            }

            // Check if socket exists
            int has_data = k_socket_has_data(fds[i].fd);

            if (has_data == -1) {
                // Socket doesn't exist
                fds[i].revents |= POLLNVAL;
                ready_count++;
                continue;
            }

            // Check POLLIN
            if (fds[i].events & POLLIN) {
                if (has_data == 1) {
                    fds[i].revents |= POLLIN;
                }
            }

            // Check POLLOUT – always writable in CamelOS
            if (fds[i].events & POLLOUT) {
                fds[i].revents |= POLLOUT;
            }

            if (fds[i].revents != 0) {
                ready_count++;
            }
        }

        // If fds are ready, return immediately
        if (ready_count > 0) {
            return ready_count;
        }

        // Handle timeout logic
        if (timeout_ms == 0) {
            // Non-blocking – return 0 if nothing ready
            return 0;
        }

        if (timeout_ms > 0) {
            uint32_t elapsed = get_tick_count() - start_tick;
            if (elapsed >= (uint32_t)deadline_ticks) {
                return 0;  // Timeout expired
            }
        }
        // timeout_ms == -1: wait indefinitely

        // Poll the network and try again
        poll_network_once();
    }

    // Unreachable, but keeps compiler happy
    return -1;
}
