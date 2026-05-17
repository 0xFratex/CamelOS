// core/bsd_syscall.h - BSD/macOS Syscall Translation Layer for CamelOS
// Translates common BSD syscalls (open, read, write, mmap, etc.) and
// mach_msg traps to CamelOS kernel API equivalents
// This layer sits between Mach-O loaded binaries and the kernel
#ifndef BSD_SYSCALL_H
#define BSD_SYSCALL_H

#include "../include/types.h"

// ============================================================================
// BSD Syscall Numbers (macOS XNU numbering)
// These are the syscall numbers that macOS apps would use via int 0x80
// ============================================================================

// Process
#define SYS_BSD_exit        1
#define SYS_BSD_fork        2
#define SYS_BSD_read        3
#define SYS_BSD_write       4
#define SYS_BSD_open        5
#define SYS_BSD_close       6
#define SYS_BSD_wait4       7
#define SYS_BSD_execve      59
#define SYS_BSD_getpid      20
#define SYS_BSD_getuid      24
#define SYS_BSD_geteuid     25
#define SYS_BSD_getgid      26
#define SYS_BSD_getegid     27

// File
#define SYS_BSD_chdir       12
#define SYS_BSD_chmod       15
#define SYS_BSD_chown       16
#define SYS_BSD_getcwd      183  // macOS syscall number for __getcwd
#define SYS_BSD_stat        38
#define SYS_BSD_lstat       40
#define SYS_BSD_dup         41
#define SYS_BSD_dup2        90
#define SYS_BSD_mkdir       136
#define SYS_BSD_rmdir       137
#define SYS_BSD_rename      128
#define SYS_BSD_unlink      10
#define SYS_BSD_truncate    129
#define SYS_BSD_ftruncate   130
#define SYS_BSD_link        9
#define SYS_BSD_symlink     57
#define SYS_BSD_readlink    58

// Memory
#define SYS_BSD_mmap        197
#define SYS_BSD_munmap      73
#define SYS_BSD_mprotect    74
#define SYS_BSD_msync       65
#define SYS_BSD_madvise     75
#define SYS_BSD_brk         17
#define SYS_BSD_sbrk        69  // Not a real BSD syscall but common

// I/O
#define SYS_BSD_ioctl       54
#define SYS_BSD_fcntl       92
#define SYS_BSD_select      93
#define SYS_BSD_poll        94
#define SYS_BSD_readv       120
#define SYS_BSD_writev      121

// Network (socket)
#define SYS_BSD_socket      97
#define SYS_BSD_bind        104
#define SYS_BSD_connect     98
#define SYS_BSD_listen      106
#define SYS_BSD_accept      30
#define SYS_BSD_sendto      133
#define SYS_BSD_recvfrom    29
#define SYS_BSD_sendmsg     102
#define SYS_BSD_recvmsg     27
#define SYS_BSD_shutdown    134
#define SYS_BSD_setsockopt  105
#define SYS_BSD_getsockopt  118
#define SYS_BSD_getsockname 32
#define SYS_BSD_getpeername 31

// Signals
#define SYS_BSD_sigaction   46
#define SYS_BSD_sigprocmask 48
#define SYS_BSD_kill        37
#define SYS_BSD_sigreturn   52

// Time
#define SYS_BSD_gettimeofday 116
#define SYS_BSD_settimeofday 122
#define SYS_BSD_clock_gettime 232
#define SYS_BSD_nanosleep   240

// File descriptor management
#define SYS_BSD_pipe        42
#define SYS_BSD_getdtablesize 89
#define SYS_BSD_fpathconf   113

// ============================================================================
// BSD file descriptor table (maps BSD fd to CamelOS handles)
// ============================================================================

#define BSD_MAX_FDS  32

typedef enum {
    FD_TYPE_NONE = 0,
    FD_TYPE_FILE,       // PFS32 file handle
    FD_TYPE_DIR,        // Directory listing
    FD_TYPE_SOCKET,     // Network socket
    FD_TYPE_PIPE,       // Pipe (unidirectional)
    FD_TYPE_STDIN,      // Standard input
    FD_TYPE_STDOUT,     // Standard output
    FD_TYPE_STDERR      // Standard error
} bsd_fd_type_t;

typedef struct {
    int in_use;
    bsd_fd_type_t type;
    int kernel_handle;   // Corresponding kernel handle/fd
    char path[256];      // For file FDs
    int flags;           // Open flags (O_RDONLY, O_WRONLY, O_RDWR)
    int offset;          // Current file offset
    int is_dir;          // Directory flag
} bsd_fd_entry_t;

// ============================================================================
// BSD stat structure (simplified)
// ============================================================================

typedef struct {
    uint32_t st_dev;       // Device
    uint32_t st_ino;       // Inode number
    uint16_t st_mode;      // File mode (permissions + type)
    uint16_t st_nlink;     // Number of links
    uint32_t st_uid;       // Owner UID
    uint32_t st_gid;       // Group GID
    uint32_t st_rdev;      // Device (if special)
    uint32_t st_size;      // File size in bytes
    uint32_t st_atime;     // Access time
    uint32_t st_mtime;     // Modification time
    uint32_t st_ctime;     // Change time
    uint32_t st_blksize;   // Preferred block size
    uint32_t st_blocks;    // Number of blocks
} bsd_stat_t;

// File mode bits
#define BSD_S_IFMT    0xF000  // File type mask
#define BSD_S_IFDIR   0x4000  // Directory
#define BSD_S_IFREG   0x8000  // Regular file
#define BSD_S_IFLNK   0xA000  // Symbolic link
#define BSD_S_IRUSR   0x0100  // Owner read
#define BSD_S_IWUSR   0x0080  // Owner write
#define BSD_S_IXUSR   0x0040  // Owner execute

// Open flags
#define BSD_O_RDONLY   0x0000
#define BSD_O_WRONLY   0x0001
#define BSD_O_RDWR     0x0002
#define BSD_O_CREAT    0x0200
#define BSD_O_TRUNC    0x0400
#define BSD_O_APPEND   0x0008
#define BSD_O_EXCL     0x0800

// ============================================================================
// API Functions
// ============================================================================

// Initialize the BSD syscall translation layer
void bsd_syscall_init(void);

// Main BSD syscall handler - called from the syscall interrupt handler
// when a BSD syscall number is detected
int32_t bsd_syscall_handler(uint32_t syscall_num,
                            uint32_t arg1, uint32_t arg2, uint32_t arg3,
                            uint32_t arg4, uint32_t arg5);

// Individual syscall implementations
int bsd_open(const char* path, int flags, int mode);
int bsd_close(int fd);
int bsd_read(int fd, void* buf, uint32_t count);
int bsd_write(int fd, const void* buf, uint32_t count);
int bsd_stat(const char* path, bsd_stat_t* buf);
int bsd_lstat(const char* path, bsd_stat_t* buf);
int bsd_mkdir(const char* path, int mode);
int bsd_unlink(const char* path);
int bsd_rename(const char* oldpath, const char* newpath);
int bsd_chdir(const char* path);
int bsd_getcwd(char* buf, uint32_t size);
void* bsd_mmap(void* addr, uint32_t length, int prot, int flags, int fd, uint32_t offset);
int bsd_munmap(void* addr, uint32_t length);
int bsd_mprotect(void* addr, uint32_t length, int prot);
int bsd_getpid(void);
int bsd_getuid(void);
int bsd_getgid(void);
int bsd_socket(int domain, int type, int protocol);
int bsd_bind(int sockfd, const void* addr, uint32_t addrlen);
int bsd_connect(int sockfd, const void* addr, uint32_t addrlen);
int bsd_listen(int sockfd, int backlog);
int bsd_accept(int sockfd, void* addr, uint32_t* addrlen);
int bsd_send(int sockfd, const void* buf, uint32_t len, int flags);
int bsd_recv(int sockfd, void* buf, uint32_t len, int flags);
int bsd_sendto(int sockfd, const void* buf, uint32_t len, int flags, const void* addr, uint32_t addrlen);
int bsd_recvfrom(int sockfd, void* buf, uint32_t len, int flags, void* addr, uint32_t* addrlen);
int bsd_shutdown(int sockfd, int how);
int bsd_gettimeofday(void* tv, void* tz);
int bsd_nanosleep(const void* req, void* rem);
int bsd_ioctl(int fd, uint32_t request, void* arg);
int bsd_fcntl(int fd, int cmd, int arg);
int bsd_dup(int fd);
int bsd_dup2(int oldfd, int newfd);
int bsd_pipe(int pipefd[2]);
int bsd_select(int nfds, void* readfds, void* writefds, void* exceptfds, const void* timeout);

// Allocate a file descriptor
int bsd_alloc_fd(bsd_fd_type_t type, int kernel_handle, const char* path, int flags);

// Get the FD entry
bsd_fd_entry_t* bsd_get_fd(int fd);

#endif // BSD_SYSCALL_H
