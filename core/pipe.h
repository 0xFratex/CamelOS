#ifndef PIPE_H
#define PIPE_H

#include "../include/types.h"
#include "task.h"

/* Pipe buffer size (4KB = 1 page) */
#define PIPE_BUF_SIZE       4096
#define PIPE_MAX_PIPES      64

/* Pipe flags */
#define PIPE_FLAG_READ      0x01
#define PIPE_FLAG_WRITE     0x02
#define PIPE_FLAG_NONBLOCK  0x04
#define PIPE_FLAG_CLOSED    0x08

/* Pipe file descriptor structure */
typedef struct pipe_fd {
    int pipe_id;           /* Back-reference to pipe */
    int flags;             /* PIPE_FLAG_* combination */
    int fd_type;           /* 0 = read end, 1 = write end */
    task_t* owner;         /* Owning process */
} pipe_fd_t;

/* Pipe structure (shared buffer with read/write pointers) */
typedef struct pipe {
    int id;                       /* Pipe identifier */
    char buffer[PIPE_BUF_SIZE];   /* Circular buffer */
    uint32_t read_pos;            /* Read position in buffer */
    uint32_t write_pos;           /* Write position in buffer */
    uint32_t count;               /* Bytes currently in buffer */
    int readers;                  /* Number of active read ends */
    int writers;                  /* Number of active write ends */
    int ref_count;                /* Total reference count */
    task_t* read_waiter;          /* Task blocked waiting to read */
    task_t* write_waiter;         /* Task blocked waiting to write */
    int alive;                    /* 1 if pipe is in use, 0 if free */
} pipe_t;

/* Named pipe (FIFO) structure */
typedef struct named_pipe {
    char path[128];               /* Filesystem path for the FIFO */
    int pipe_id;                  /* Associated pipe ID */
    int mode;                     /* Permissions mode */
    struct named_pipe* next;      /* Linked list */
} named_pipe_t;

/* === Initialization === */
void pipe_init(void);

/* === Anonymous Pipe === */
int pipe_create(int fd_out[2]);   /* Create pipe, return read/write fds in fd_out */
int pipe_read(int pipe_fd, void* buf, size_t count);
int pipe_write(int pipe_fd, const void* buf, size_t count);
int pipe_close(int pipe_fd);
int pipe_ioctl(int pipe_fd, int cmd, void* arg);

/* === Named Pipe (FIFO) === */
int pipe_mkfifo(const char* path, int mode);
int pipe_open_fifo(const char* path, int flags);

/* === Internal === */
pipe_t* pipe_get_by_id(int id);
pipe_fd_t* pipe_fd_get(int fd);

/* === Statistics === */
typedef struct {
    int active_pipes;
    int total_created;
    int total_destroyed;
    int active_fifos;
} pipe_stats_t;

pipe_stats_t* pipe_get_stats(void);

/* === Pipe ioctl commands === */
#define PIPE_IOCTL_SET_NONBLOCK   1
#define PIPE_IOCTL_GET_SIZE       2
#define PIPE_IOCTL_GET_COUNT      3

#endif /* PIPE_H */
