/**
 * CamelOS Pipe IPC Implementation
 *
 * Provides anonymous pipes and named pipes (FIFOs) for inter-process
 * communication using circular buffers with blocking semantics.
 *
 * Features:
 *   - Circular buffer (4KB per pipe) with read/write positions
 *   - Blocking I/O with scheduler integration
 *   - Non-blocking mode via ioctl
 *   - Atomic writes up to PIPE_BUF_SIZE
 *   - Named pipes (FIFOs) with filesystem path binding
 *   - Per-fd flags (read/write end, nonblock)
 */

#include "pipe.h"
#include "memory.h"
#include "string.h"
#include "scheduler.h"
#include "../hal/drivers/serial.h"

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define PIPE_FD_BASE      1000    /* Pipe fd numbers start here */
#define PIPE_FD_MAX       256     /* Maximum pipe file descriptors */

/* ========================================================================== */
/* Global State                                                               */
/* ========================================================================== */

static pipe_t      g_pipes[PIPE_MAX_PIPES];
static pipe_fd_t   g_pipe_fds[PIPE_FD_MAX];
static named_pipe_t* g_named_pipe_list = NULL;

/* Statistics */
static pipe_stats_t g_pipe_stats;

/* Next pipe ID to allocate */
static int g_next_pipe_id = 1;

/* ========================================================================== */
/* Internal Helpers                                                           */
/* ========================================================================== */

/**
 * Find a free slot in the pipe table.
 * Returns pointer to the slot, or NULL if none available.
 */
static pipe_t* pipe_alloc_slot(void) {
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (!g_pipes[i].alive) {
            return &g_pipes[i];
        }
    }
    return NULL;
}

/**
 * Find a free slot in the pipe fd table.
 * Returns the index, or -1 if none available.
 */
static int pipe_fd_alloc_slot(void) {
    for (int i = 0; i < PIPE_FD_MAX; i++) {
        if (g_pipe_fds[i].flags == 0 && g_pipe_fds[i].pipe_id == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * Convert a user-facing fd number to an index in g_pipe_fds.
 * Returns the index, or -1 if out of range.
 */
static int pipe_fd_to_index(int fd) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_FD_MAX) {
        return -1;
    }
    return fd - PIPE_FD_BASE;
}

/**
 * Read bytes from the circular buffer into dst.
 * Returns the number of bytes actually read.
 * Advances read_pos and decrements count.
 */
static uint32_t pipe_buffer_read(pipe_t* pipe, void* dst, size_t count) {
    uint8_t* dst_bytes = (uint8_t*)dst;
    uint32_t to_read = (uint32_t)count;

    if (to_read > pipe->count) {
        to_read = pipe->count;
    }

    for (uint32_t i = 0; i < to_read; i++) {
        dst_bytes[i] = (uint8_t)pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
    }

    pipe->count -= to_read;
    return to_read;
}

/**
 * Write bytes from src into the circular buffer.
 * Returns the number of bytes actually written.
 * Advances write_pos and increments count.
 */
static uint32_t pipe_buffer_write(pipe_t* pipe, const void* src, size_t count) {
    const uint8_t* src_bytes = (const uint8_t*)src;
    uint32_t available = PIPE_BUF_SIZE - pipe->count;
    uint32_t to_write = (uint32_t)count;

    if (to_write > available) {
        to_write = available;
    }

    for (uint32_t i = 0; i < to_write; i++) {
        pipe->buffer[pipe->write_pos] = (char)src_bytes[i];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
    }

    pipe->count += to_write;
    return to_write;
}

/**
 * Look up a named_pipe by its filesystem path.
 * Returns pointer to the named_pipe_t, or NULL if not found.
 */
static named_pipe_t* named_pipe_find(const char* path) {
    named_pipe_t* np = g_named_pipe_list;
    while (np) {
        if (strcmp(np->path, path) == 0) {
            return np;
        }
        np = np->next;
    }
    return NULL;
}

/* ========================================================================== */
/* Initialization                                                             */
/* ========================================================================== */

void pipe_init(void) {
    /* Zero out the pipe table */
    memset(g_pipes, 0, sizeof(g_pipes));
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        g_pipes[i].alive = 0;
        g_pipes[i].id = 0;
    }

    /* Zero out the fd table */
    memset(g_pipe_fds, 0, sizeof(g_pipe_fds));

    /* Clear named pipe list */
    g_named_pipe_list = NULL;

    /* Reset statistics */
    memset(&g_pipe_stats, 0, sizeof(g_pipe_stats));

    /* Reset ID counter */
    g_next_pipe_id = 1;

    s_printf("[PIPE] Pipe IPC subsystem initialized\n");
}

/* ========================================================================== */
/* Anonymous Pipe                                                             */
/* ========================================================================== */

int pipe_create(int fd_out[2]) {
    if (!fd_out) {
        s_printf("[PIPE] pipe_create: fd_out is NULL\n");
        return -1;
    }

    /* Allocate a pipe slot */
    pipe_t* pipe = pipe_alloc_slot();
    if (!pipe) {
        s_printf("[PIPE] pipe_create: no free pipe slots\n");
        return -1;
    }

    /* Initialize the pipe */
    pipe->id = g_next_pipe_id++;
    memset(pipe->buffer, 0, PIPE_BUF_SIZE);
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->ref_count = 2;
    pipe->read_waiter = NULL;
    pipe->write_waiter = NULL;
    pipe->alive = 1;

    /* Allocate read-end fd */
    int read_idx = pipe_fd_alloc_slot();
    if (read_idx < 0) {
        s_printf("[PIPE] pipe_create: no free fd slots for read end\n");
        pipe->alive = 0;
        return -1;
    }

    /* Allocate write-end fd */
    int write_idx = pipe_fd_alloc_slot();
    if (write_idx < 0) {
        s_printf("[PIPE] pipe_create: no free fd slots for write end\n");
        g_pipe_fds[read_idx].flags = 0;
        g_pipe_fds[read_idx].pipe_id = 0;
        pipe->alive = 0;
        return -1;
    }

    /* Initialize read-end fd */
    g_pipe_fds[read_idx].pipe_id = pipe->id;
    g_pipe_fds[read_idx].flags = PIPE_FLAG_READ;
    g_pipe_fds[read_idx].fd_type = 0;   /* read end */
    g_pipe_fds[read_idx].owner = scheduler_get_current();

    /* Initialize write-end fd */
    g_pipe_fds[write_idx].pipe_id = pipe->id;
    g_pipe_fds[write_idx].flags = PIPE_FLAG_WRITE;
    g_pipe_fds[write_idx].fd_type = 1;   /* write end */
    g_pipe_fds[write_idx].owner = scheduler_get_current();

    /* Return fd numbers to caller */
    fd_out[0] = PIPE_FD_BASE + read_idx;   /* read fd */
    fd_out[1] = PIPE_FD_BASE + write_idx;  /* write fd */

    /* Update statistics */
    g_pipe_stats.active_pipes++;
    g_pipe_stats.total_created++;

    s_printf("[PIPE] pipe_create: pipe id=");
    {
        char buf[16];
        int_to_str(pipe->id, buf);
        s_printf(buf);
    }
    s_printf(" rfd=");
    {
        char buf[16];
        int_to_str(fd_out[0], buf);
        s_printf(buf);
    }
    s_printf(" wfd=");
    {
        char buf[16];
        int_to_str(fd_out[1], buf);
        s_printf(buf);
    }
    s_printf("\n");

    return 0;
}

int pipe_read(int pipe_fd, void* buf, size_t count) {
    if (!buf || count == 0) {
        return -1;
    }

    /* Resolve fd to internal structure */
    int idx = pipe_fd_to_index(pipe_fd);
    if (idx < 0) {
        s_printf("[PIPE] pipe_read: invalid fd\n");
        return -1;
    }

    pipe_fd_t* pfd = &g_pipe_fds[idx];

    /* Must be a read-end */
    if (pfd->fd_type != 0 || !(pfd->flags & PIPE_FLAG_READ)) {
        s_printf("[PIPE] pipe_read: fd is not a read end\n");
        return -1;
    }

    /* Check for closed fd */
    if (pfd->flags & PIPE_FLAG_CLOSED) {
        return -1;
    }

    /* Get the underlying pipe */
    pipe_t* pipe = pipe_get_by_id(pfd->pipe_id);
    if (!pipe || !pipe->alive) {
        s_printf("[PIPE] pipe_read: pipe not found\n");
        return -1;
    }

    /* If buffer is empty */
    if (pipe->count == 0) {
        if (pipe->writers == 0) {
            /* No writers left: EOF */
            return 0;
        }

        /* Would block in non-blocking mode */
        if (pfd->flags & PIPE_FLAG_NONBLOCK) {
            return -1;  /* EAGAIN */
        }

        /* Block the current task waiting for data */
        pipe->read_waiter = scheduler_get_current();
        s_printf("[PIPE] pipe_read: blocking on empty pipe\n");
        scheduler_block(BLOCK_REASON_IO);

        /*
         * When we are unblocked, the writer has put data in or writers==0.
         * Re-check conditions. If still empty and no writers, EOF.
         */
        if (pipe->count == 0 && pipe->writers == 0) {
            return 0;
        }

        /* If still empty after unblock (spurious), return 0 */
        if (pipe->count == 0) {
            return 0;
        }
    }

    /* Read from the circular buffer */
    uint32_t bytes_read = pipe_buffer_read(pipe, buf, count);

    /* If a writer was blocked because the buffer was full, unblock it */
    if (pipe->write_waiter) {
        task_t* waiter = pipe->write_waiter;
        pipe->write_waiter = NULL;
        scheduler_unblock(waiter);
    }

    return (int)bytes_read;
}

int pipe_write(int pipe_fd, const void* buf, size_t count) {
    if (!buf || count == 0) {
        return -1;
    }

    /* Resolve fd to internal structure */
    int idx = pipe_fd_to_index(pipe_fd);
    if (idx < 0) {
        s_printf("[PIPE] pipe_write: invalid fd\n");
        return -1;
    }

    pipe_fd_t* pfd = &g_pipe_fds[idx];

    /* Must be a write-end */
    if (pfd->fd_type != 1 || !(pfd->flags & PIPE_FLAG_WRITE)) {
        s_printf("[PIPE] pipe_write: fd is not a write end\n");
        return -1;
    }

    /* Check for closed fd */
    if (pfd->flags & PIPE_FLAG_CLOSED) {
        return -1;
    }

    /* Get the underlying pipe */
    pipe_t* pipe = pipe_get_by_id(pfd->pipe_id);
    if (!pipe || !pipe->alive) {
        s_printf("[PIPE] pipe_write: pipe not found\n");
        return -1;
    }

    /* No readers: SIGPIPE */
    if (pipe->readers == 0) {
        s_printf("[PIPE] pipe_write: no readers (SIGPIPE)\n");
        return -1;
    }

    /*
     * Atomic write semantics:
     * If count <= PIPE_BUF_SIZE, the write must be atomic (all or nothing).
     * If the buffer does not have enough space, block until it does
     * (or return -1 in non-blocking mode).
     */
    if (count <= PIPE_BUF_SIZE) {
        uint32_t available = PIPE_BUF_SIZE - pipe->count;

        if (available < count) {
            /* Not enough space */
            if (pipe->readers == 0) {
                return -1;  /* SIGPIPE */
            }

            if (pfd->flags & PIPE_FLAG_NONBLOCK) {
                return -1;  /* EAGAIN */
            }

            /* Block until space is available */
            pipe->write_waiter = scheduler_get_current();
            s_printf("[PIPE] pipe_write: blocking on full pipe\n");
            scheduler_block(BLOCK_REASON_IO);

            /*
             * Unblocked — re-check. Reader may have consumed data,
             * or readers may have gone away.
             */
            if (pipe->readers == 0) {
                return -1;  /* SIGPIPE */
            }

            available = PIPE_BUF_SIZE - pipe->count;
            if (available < count) {
                /* Still not enough space after unblock — partial or fail */
                if (available == 0) {
                    return -1;
                }
                /* For atomic writes, we should write nothing if we can't
                 * fit the whole thing. But since we were unblocked, a
                 * reader freed some space. If still not enough, return -1.
                 */
                return -1;
            }
        }

        /* Write the data atomically */
        uint32_t written = pipe_buffer_write(pipe, buf, count);

        /* If a reader was blocked waiting for data, unblock it */
        if (pipe->read_waiter) {
            task_t* waiter = pipe->read_waiter;
            pipe->read_waiter = NULL;
            scheduler_unblock(waiter);
        }

        return (int)written;
    }

    /*
     * Large write (count > PIPE_BUF_SIZE):
     * Write as much as we can in a non-atomic fashion.
     */
    uint32_t available = PIPE_BUF_SIZE - pipe->count;

    if (available == 0) {
        if (pipe->readers == 0) {
            return -1;
        }

        if (pfd->flags & PIPE_FLAG_NONBLOCK) {
            return -1;
        }

        /* Block until space is available */
        pipe->write_waiter = scheduler_get_current();
        s_printf("[PIPE] pipe_write: blocking on full pipe (large)\n");
        scheduler_block(BLOCK_REASON_IO);

        if (pipe->readers == 0) {
            return -1;
        }

        available = PIPE_BUF_SIZE - pipe->count;
        if (available == 0) {
            return -1;
        }
    }

    /* Write what we can */
    uint32_t to_write = (count < available) ? (uint32_t)count : available;
    uint32_t written = pipe_buffer_write(pipe, buf, to_write);

    /* Unblock any waiting reader */
    if (pipe->read_waiter) {
        task_t* waiter = pipe->read_waiter;
        pipe->read_waiter = NULL;
        scheduler_unblock(waiter);
    }

    return (int)written;
}

int pipe_close(int pipe_fd) {
    int idx = pipe_fd_to_index(pipe_fd);
    if (idx < 0) {
        s_printf("[PIPE] pipe_close: invalid fd\n");
        return -1;
    }

    pipe_fd_t* pfd = &g_pipe_fds[idx];

    if (pfd->flags == 0 && pfd->pipe_id == 0) {
        s_printf("[PIPE] pipe_close: fd not in use\n");
        return -1;
    }

    /* Already closed */
    if (pfd->flags & PIPE_FLAG_CLOSED) {
        return -1;
    }

    /* Get the pipe */
    pipe_t* pipe = pipe_get_by_id(pfd->pipe_id);
    if (!pipe) {
        /* Pipe already gone — just free the fd */
        pfd->flags = 0;
        pfd->pipe_id = 0;
        pfd->fd_type = 0;
        pfd->owner = NULL;
        return -1;
    }

    /* Decrement reference count */
    pipe->ref_count--;

    /* Close the appropriate end */
    if (pfd->fd_type == 0) {
        /* Closing read end */
        pipe->readers--;
        if (pipe->readers <= 0) {
            pipe->readers = 0;
            /* If a writer is blocked, unblock it (will get SIGPIPE/-1) */
            if (pipe->write_waiter) {
                task_t* waiter = pipe->write_waiter;
                pipe->write_waiter = NULL;
                scheduler_unblock(waiter);
            }
        }
    } else if (pfd->fd_type == 1) {
        /* Closing write end */
        pipe->writers--;
        if (pipe->writers <= 0) {
            pipe->writers = 0;
            /* If a reader is blocked, unblock it (will get 0/EOF) */
            if (pipe->read_waiter) {
                task_t* waiter = pipe->read_waiter;
                pipe->read_waiter = NULL;
                scheduler_unblock(waiter);
            }
        }
    }

    /* If ref_count reaches 0, free the pipe slot */
    if (pipe->ref_count <= 0) {
        pipe->alive = 0;
        pipe->id = 0;
        pipe->read_waiter = NULL;
        pipe->write_waiter = NULL;

        g_pipe_stats.active_pipes--;
        g_pipe_stats.total_destroyed++;
    }

    /* Free the fd slot */
    pfd->flags = 0;
    pfd->pipe_id = 0;
    pfd->fd_type = 0;
    pfd->owner = NULL;

    s_printf("[PIPE] pipe_close: fd closed\n");
    return 0;
}

int pipe_ioctl(int pipe_fd, int cmd, void* arg) {
    int idx = pipe_fd_to_index(pipe_fd);
    if (idx < 0) {
        return -1;
    }

    pipe_fd_t* pfd = &g_pipe_fds[idx];
    if (pfd->flags == 0 && pfd->pipe_id == 0) {
        return -1;
    }

    pipe_t* pipe = pipe_get_by_id(pfd->pipe_id);
    if (!pipe || !pipe->alive) {
        return -1;
    }

    switch (cmd) {
    case PIPE_IOCTL_SET_NONBLOCK: {
        int enable = arg ? *(int*)arg : 0;
        if (enable) {
            pfd->flags |= PIPE_FLAG_NONBLOCK;
        } else {
            pfd->flags &= ~PIPE_FLAG_NONBLOCK;
        }
        return 0;
    }

    case PIPE_IOCTL_GET_SIZE:
        return PIPE_BUF_SIZE;

    case PIPE_IOCTL_GET_COUNT:
        return (int)pipe->count;

    default:
        s_printf("[PIPE] pipe_ioctl: unknown command\n");
        return -1;
    }
}

/* ========================================================================== */
/* Named Pipe (FIFO)                                                          */
/* ========================================================================== */

int pipe_mkfifo(const char* path, int mode) {
    if (!path || strlen(path) == 0) {
        s_printf("[PIPE] pipe_mkfifo: invalid path\n");
        return -1;
    }

    /* Check if a FIFO already exists at this path */
    if (named_pipe_find(path)) {
        s_printf("[PIPE] pipe_mkfifo: FIFO already exists\n");
        return -1;
    }

    /* Create a new pipe for the FIFO */
    int fds[2];
    if (pipe_create(fds) != 0) {
        s_printf("[PIPE] pipe_mkfifo: failed to create underlying pipe\n");
        return -1;
    }

    /* Close the temporary fds we just created */
    pipe_close(fds[0]);
    pipe_close(fds[1]);

    /*
     * After closing both fds, ref_count reached 0 and the pipe was freed.
     * We now allocate a fresh persistent pipe slot that is NOT tied to
     * any fd. Fds will be created when the FIFO is opened.
     */
    pipe_t* pipe = pipe_alloc_slot();
    if (!pipe) {
        s_printf("[PIPE] pipe_mkfifo: no free pipe slot for FIFO\n");
        return -1;
    }

    pipe->id = g_next_pipe_id++;
    memset(pipe->buffer, 0, PIPE_BUF_SIZE);
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->readers = 0;       /* No readers yet — will increment on open */
    pipe->writers = 0;       /* No writers yet */
    pipe->ref_count = 0;     /* Will increment as fds are opened */
    pipe->read_waiter = NULL;
    pipe->write_waiter = NULL;
    pipe->alive = 1;

    g_pipe_stats.active_pipes++;
    g_pipe_stats.total_created++;

    /* Allocate named_pipe entry */
    named_pipe_t* np = (named_pipe_t*)kmalloc(sizeof(named_pipe_t));
    if (!np) {
        s_printf("[PIPE] pipe_mkfifo: out of memory\n");
        pipe->alive = 0;
        g_pipe_stats.active_pipes--;
        return -1;
    }

    strncpy(np->path, path, sizeof(np->path) - 1);
    np->path[sizeof(np->path) - 1] = '\0';
    np->pipe_id = pipe->id;
    np->mode = mode;
    np->next = g_named_pipe_list;
    g_named_pipe_list = np;

    g_pipe_stats.active_fifos++;

    s_printf("[PIPE] pipe_mkfifo: created FIFO at ");
    s_printf(path);
    s_printf("\n");

    return 0;
}

int pipe_open_fifo(const char* path, int flags) {
    if (!path) {
        return -1;
    }

    /* Look up the named pipe */
    named_pipe_t* np = named_pipe_find(path);

    if (!np) {
        /* Create the FIFO implicitly if it doesn't exist */
        if (pipe_mkfifo(path, 0666) != 0) {
            s_printf("[PIPE] pipe_open_fifo: failed to create FIFO\n");
            return -1;
        }
        np = named_pipe_find(path);
        if (!np) {
            return -1;
        }
    }

    /* Get the underlying pipe */
    pipe_t* pipe = pipe_get_by_id(np->pipe_id);
    if (!pipe || !pipe->alive) {
        s_printf("[PIPE] pipe_open_fifo: underlying pipe not found\n");
        return -1;
    }

    /* Allocate an fd slot */
    int fd_idx = pipe_fd_alloc_slot();
    if (fd_idx < 0) {
        s_printf("[PIPE] pipe_open_fifo: no free fd slots\n");
        return -1;
    }

    /* Determine if this is a read or write open */
    int fd_type;
    int fd_flags;

    if (flags & PIPE_FLAG_WRITE) {
        fd_type = 1;               /* write end */
        fd_flags = PIPE_FLAG_WRITE;
    } else {
        fd_type = 0;               /* read end (default) */
        fd_flags = PIPE_FLAG_READ;
    }

    if (flags & PIPE_FLAG_NONBLOCK) {
        fd_flags |= PIPE_FLAG_NONBLOCK;
    }

    /* Initialize the fd */
    g_pipe_fds[fd_idx].pipe_id = pipe->id;
    g_pipe_fds[fd_idx].flags = fd_flags;
    g_pipe_fds[fd_idx].fd_type = fd_type;
    g_pipe_fds[fd_idx].owner = scheduler_get_current();

    /* Update pipe counters */
    if (fd_type == 0) {
        pipe->readers++;
    } else {
        pipe->writers++;
    }
    pipe->ref_count++;

    /* If a waiter of the opposite type was blocked, unblock it */
    if (fd_type == 0 && pipe->write_waiter) {
        /* Reader opened — unblock a blocked writer so it can try writing */
        /* Actually, writers block because the buffer is full, not because
         * there are no readers (they check readers==0 for SIGPIPE).
         * However, in blocking FIFO open semantics, a writer might be
         * waiting for a reader to appear. We unblock to let it re-evaluate. */
    }
    if (fd_type == 1 && pipe->read_waiter) {
        /* Writer opened — a blocked reader may now have data coming.
         * But the writer hasn't written yet, so no need to unblock
         * unless the reader was blocked on empty pipe with writers>0. */
    }

    s_printf("[PIPE] pipe_open_fifo: opened ");
    s_printf(path);
    s_printf(" as ");
    s_printf(fd_type == 0 ? "read" : "write");
    s_printf("\n");

    return PIPE_FD_BASE + fd_idx;
}

/* ========================================================================== */
/* Internal Accessors                                                         */
/* ========================================================================== */

pipe_t* pipe_get_by_id(int id) {
    if (id <= 0) return NULL;

    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (g_pipes[i].alive && g_pipes[i].id == id) {
            return &g_pipes[i];
        }
    }
    return NULL;
}

pipe_fd_t* pipe_fd_get(int fd) {
    int idx = pipe_fd_to_index(fd);
    if (idx < 0) return NULL;

    pipe_fd_t* pfd = &g_pipe_fds[idx];
    if (pfd->flags == 0 && pfd->pipe_id == 0) {
        return NULL;
    }

    return pfd;
}

/* ========================================================================== */
/* Statistics                                                                 */
/* ========================================================================== */

pipe_stats_t* pipe_get_stats(void) {
    /* Refresh active count from actual state */
    int active = 0;
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (g_pipes[i].alive) {
            active++;
        }
    }
    g_pipe_stats.active_pipes = active;
    return &g_pipe_stats;
}
