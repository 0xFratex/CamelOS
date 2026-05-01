// core/bsd_syscall.c - BSD/macOS Syscall Translation Layer Implementation
// Translates macOS/XNU BSD syscall numbers to CamelOS kernel API calls
// Provides a POSIX-compatible interface for Mach-O loaded binaries

#include "bsd_syscall.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../sys/cdl_defs.h"
#include "../fs/pfs32.h"
#include "../hal/drivers/serial.h"

// Kernel API
extern kernel_api_t g_kernel_api;

// --- State ---
static bsd_fd_entry_t g_fd_table[BSD_MAX_FDS];

// --- Initialization ---

void bsd_syscall_init(void) {
    memset(g_fd_table, 0, sizeof(g_fd_table));

    // Pre-allocate standard file descriptors
    // fd 0 = stdin (keyboard)
    g_fd_table[0].in_use = 1;
    g_fd_table[0].type = FD_TYPE_STDIN;
    g_fd_table[0].kernel_handle = -1;
    g_fd_table[0].flags = BSD_O_RDONLY;

    // fd 1 = stdout (serial/screen)
    g_fd_table[1].in_use = 1;
    g_fd_table[1].type = FD_TYPE_STDOUT;
    g_fd_table[1].kernel_handle = -1;
    g_fd_table[1].flags = BSD_O_WRONLY;

    // fd 2 = stderr (serial)
    g_fd_table[2].in_use = 1;
    g_fd_table[2].type = FD_TYPE_STDERR;
    g_fd_table[2].kernel_handle = -1;
    g_fd_table[2].flags = BSD_O_WRONLY;

    s_printf("[BSD] Syscall translation layer initialized\n");
}

// --- FD Management ---

int bsd_alloc_fd(bsd_fd_type_t type, int kernel_handle, const char* path, int flags) {
    // Start from fd 3 (0-2 are stdio)
    for (int i = 3; i < BSD_MAX_FDS; i++) {
        if (!g_fd_table[i].in_use) {
            g_fd_table[i].in_use = 1;
            g_fd_table[i].type = type;
            g_fd_table[i].kernel_handle = kernel_handle;
            g_fd_table[i].flags = flags;
            g_fd_table[i].offset = 0;
            g_fd_table[i].is_dir = 0;
            if (path) {
                strncpy(g_fd_table[i].path, path, 255);
                g_fd_table[i].path[255] = 0;
            } else {
                g_fd_table[i].path[0] = 0;
            }
            return i;
        }
    }
    return -1;  // No free FDs
}

bsd_fd_entry_t* bsd_get_fd(int fd) {
    if (fd < 0 || fd >= BSD_MAX_FDS) return 0;
    if (!g_fd_table[fd].in_use) return 0;
    return &g_fd_table[fd];
}

// --- Main Syscall Handler ---

int32_t bsd_syscall_handler(uint32_t syscall_num,
                            uint32_t arg1, uint32_t arg2, uint32_t arg3,
                            uint32_t arg4, uint32_t arg5) {
    switch (syscall_num) {
        // Process
        case SYS_BSD_exit:
            // In CamelOS, "exit" from a Mach-O app means return to caller
            return 0;
        case SYS_BSD_getpid:  return bsd_getpid();
        case SYS_BSD_getuid:  return bsd_getuid();
        case SYS_BSD_getgid:  return bsd_getgid();

        // File I/O
        case SYS_BSD_open:
            return bsd_open((const char*)arg1, (int)arg2, (int)arg3);
        case SYS_BSD_close:
            return bsd_close((int)arg1);
        case SYS_BSD_read:
            return bsd_read((int)arg1, (void*)arg2, (uint32_t)arg3);
        case SYS_BSD_write:
            return bsd_write((int)arg1, (const void*)arg2, (uint32_t)arg3);

        // File metadata
        case SYS_BSD_stat:
            return bsd_stat((const char*)arg1, (bsd_stat_t*)arg2);
        case SYS_BSD_lstat:
            return bsd_lstat((const char*)arg1, (bsd_stat_t*)arg2);
        case SYS_BSD_mkdir:
            return bsd_mkdir((const char*)arg1, (int)arg2);
        case SYS_BSD_unlink:
            return bsd_unlink((const char*)arg1);
        case SYS_BSD_rename:
            return bsd_rename((const char*)arg1, (const char*)arg2);
        case SYS_BSD_chdir:
            return bsd_chdir((const char*)arg1);

        // Memory
        case SYS_BSD_mmap:
            return (int32_t)bsd_mmap((void*)arg1, (uint32_t)arg2, (int)arg3, (int)arg4, (int)arg5, 0);
        case SYS_BSD_munmap:
            return bsd_munmap((void*)arg1, (uint32_t)arg2);
        case SYS_BSD_mprotect:
            return bsd_mprotect((void*)arg1, (uint32_t)arg2, (int)arg3);

        // Networking
        case SYS_BSD_socket:
            return bsd_socket((int)arg1, (int)arg2, (int)arg3);
        case SYS_BSD_bind:
            return bsd_bind((int)arg1, (const void*)arg2, (uint32_t)arg3);
        case SYS_BSD_connect:
            return bsd_connect((int)arg1, (const void*)arg2, (uint32_t)arg3);
        case SYS_BSD_listen:
            return bsd_listen((int)arg1, (int)arg2);
        case SYS_BSD_sendto:
            return bsd_sendto((int)arg1, (const void*)arg2, (uint32_t)arg3, (int)arg4, (const void*)arg5, 0);
        case SYS_BSD_recvfrom:
            return bsd_recvfrom((int)arg1, (void*)arg2, (uint32_t)arg3, (int)arg4, 0, 0);
        case SYS_BSD_shutdown:
            return bsd_shutdown((int)arg1, (int)arg2);

        // Time
        case SYS_BSD_gettimeofday:
            return bsd_gettimeofday((void*)arg1, (void*)arg2);
        case SYS_BSD_nanosleep:
            return bsd_nanosleep((const void*)arg1, (void*)arg2);

        // FD management
        case SYS_BSD_dup:
            return bsd_dup((int)arg1);
        case SYS_BSD_dup2:
            return bsd_dup2((int)arg1, (int)arg2);
        case SYS_BSD_pipe:
            return bsd_pipe((int*)arg1);
        case SYS_BSD_ioctl:
            return bsd_ioctl((int)arg1, (uint32_t)arg2, (void*)arg3);
        case SYS_BSD_fcntl:
            return bsd_fcntl((int)arg1, (int)arg2, (int)arg3);

        // Signals - stub out
        case SYS_BSD_sigaction:
        case SYS_BSD_sigprocmask:
            return 0;

        default:
            s_printf("[BSD] Unhandled syscall: ");
            char buf[16]; int_to_str(syscall_num, buf);
            s_printf(buf); s_printf("\n");
            return -1;
    }
}

// --- File I/O Implementations ---

int bsd_open(const char* path, int flags, int mode) {
    (void)mode;
    if (!path) return -1;

    // Check if file exists
    int exists = sys_fs_exists(path);
    int is_dir = sys_fs_is_dir(path);

    // Handle O_CREAT
    if (!exists && (flags & BSD_O_CREAT)) {
        int result = sys_fs_create(path, is_dir ? 1 : 0);
        if (result < 0) return -1;
    }

    // Allocate FD
    int fd = bsd_alloc_fd(is_dir ? FD_TYPE_DIR : FD_TYPE_FILE, -1, path, flags);
    if (fd < 0) return -1;

    g_fd_table[fd].is_dir = is_dir;

    return fd;
}

int bsd_close(int fd) {
    bsd_fd_entry_t* entry = bsd_get_fd(fd);
    if (!entry) return -1;

    switch (entry->type) {
        case FD_TYPE_FILE:
        case FD_TYPE_DIR:
            // PFS32 file handles are managed internally
            break;
        case FD_TYPE_SOCKET:
            // Close the kernel socket
            if (entry->kernel_handle >= 0) {
                // Use the socket close API
                g_kernel_api.close(entry->kernel_handle);
            }
            break;
        default:
            break;
    }

    entry->in_use = 0;
    return 0;
}

int bsd_read(int fd, void* buf, uint32_t count) {
    if (!buf) return -1;

    bsd_fd_entry_t* entry = bsd_get_fd(fd);
    if (!entry) return -1;

    switch (entry->type) {
        case FD_TYPE_STDIN: {
            // Read from keyboard
            int key = sys_get_key();
            if (key > 0 && key < 128) {
                ((char*)buf)[0] = (char)key;
                return 1;
            }
            return 0;
        }
        case FD_TYPE_FILE: {
            // Read from PFS32 file
            // For now, read the entire file and seek within it
            char temp[4096];
            int total = sys_fs_read(entry->path, temp, sizeof(temp));
            if (total <= 0) return 0;

            int remaining = total - entry->offset;
            if (remaining <= 0) return 0;

            int to_read = (count < (uint32_t)remaining) ? count : (uint32_t)remaining;
            memcpy(buf, temp + entry->offset, to_read);
            entry->offset += to_read;
            return to_read;
        }
        case FD_TYPE_SOCKET: {
            if (entry->kernel_handle >= 0) {
                return g_kernel_api.recv(entry->kernel_handle, buf, count, 0);
            }
            return -1;
        }
        default:
            return -1;
    }
}

int bsd_write(int fd, const void* buf, uint32_t count) {
    if (!buf) return -1;

    bsd_fd_entry_t* entry = bsd_get_fd(fd);
    if (!entry) return -1;

    switch (entry->type) {
        case FD_TYPE_STDOUT:
        case FD_TYPE_STDERR: {
            // Write to serial/screen
            sys_print((const char*)buf);
            return count;
        }
        case FD_TYPE_FILE: {
            // Write to PFS32 file
            // PFS32 write replaces the entire file, so this is limited
            // TODO: Implement file offset-based writing
            int result = sys_fs_write(entry->path, (char*)buf, count);
            return result >= 0 ? count : -1;
        }
        case FD_TYPE_SOCKET: {
            if (entry->kernel_handle >= 0) {
                return g_kernel_api.send(entry->kernel_handle, buf, count, 0);
            }
            return -1;
        }
        default:
            return -1;
    }
}

// --- File Metadata ---

int bsd_stat(const char* path, bsd_stat_t* buf) {
    if (!path || !buf) return -1;
    memset(buf, 0, sizeof(bsd_stat_t));

    int exists = sys_fs_exists(path);
    if (!exists) return -1;

    int is_dir = sys_fs_is_dir(path);

    buf->st_mode = is_dir ? BSD_S_IFDIR | BSD_S_IRUSR | BSD_S_IXUSR
                          : BSD_S_IFREG | BSD_S_IRUSR | BSD_S_IWUSR;
    buf->st_uid = 0;
    buf->st_gid = 0;

    // Try to get file size from pfs32
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) == 0) {
        buf->st_size = entry.file_size;
        buf->st_mtime = entry.modify_time;
        buf->st_atime = entry.access_time;
        buf->st_ctime = entry.create_time;
    }

    return 0;
}

int bsd_lstat(const char* path, bsd_stat_t* buf) {
    // No symlinks in CamelOS yet, same as stat
    return bsd_stat(path, buf);
}

int bsd_mkdir(const char* path, int mode) {
    (void)mode;
    if (!path) return -1;
    return sys_fs_create(path, 1);
}

int bsd_unlink(const char* path) {
    if (!path) return -1;
    return sys_fs_delete(path);
}

int bsd_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) return -1;
    return sys_fs_rename(oldpath, newpath);
}

int bsd_chdir(const char* path) {
    (void)path;
    // TODO: Track current working directory per-process
    return 0;
}

// --- Memory ---

void* bsd_mmap(void* addr, uint32_t length, int prot, int flags, int fd, uint32_t offset) {
    (void)prot; (void)offset;

    // MAP_ANONYMOUS (flags & 0x1000) - allocate without file backing
    if (flags & 0x1000 || fd < 0) {
        void* mem = kmalloc(length);
        if (mem && addr) {
            // If addr is a hint and we got a different address, that's OK
            // In CamelOS's flat memory model, we just allocate
        }
        return mem;
    }

    // File-backed mapping
    bsd_fd_entry_t* entry = bsd_get_fd(fd);
    if (!entry) return (void*)-1;

    void* mem = kmalloc(length);
    if (!mem) return (void*)-1;

    // Read file into mapped memory
    char temp[4096];
    int total = sys_fs_read(entry->path, temp, sizeof(temp));
    if (total > 0) {
        uint32_t to_copy = (length < (uint32_t)total) ? length : (uint32_t)total;
        memcpy(mem, temp, to_copy);
    }

    return mem;
}

int bsd_munmap(void* addr, uint32_t length) {
    (void)length;
    if (addr) kfree(addr);
    return 0;
}

int bsd_mprotect(void* addr, uint32_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    // CamelOS doesn't have memory protection yet
    return 0;
}

// --- Process ---

int bsd_getpid(void)  { return 1; }
int bsd_getuid(void)  { return 0; }
int bsd_getgid(void)  { return 0; }

// --- Networking ---

int bsd_socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    int sock = g_kernel_api.socket(domain, type, protocol);
    if (sock < 0) return -1;
    return bsd_alloc_fd(FD_TYPE_SOCKET, sock, 0, BSD_O_RDWR);
}

int bsd_bind(int sockfd, const void* addr, uint32_t addrlen) {
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.bind(entry->kernel_handle, addr, addrlen);
}

int bsd_connect(int sockfd, const void* addr, uint32_t addrlen) {
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.connect(entry->kernel_handle, addr, addrlen);
}

int bsd_listen(int sockfd, int backlog) {
    (void)backlog;
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    // CamelOS doesn't have listen() yet
    return 0;
}

int bsd_accept(int sockfd, void* addr, uint32_t* addrlen) {
    (void)addr; (void)addrlen;
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    // CamelOS doesn't have accept() yet
    return -1;
}

int bsd_send(int sockfd, const void* buf, uint32_t len, int flags) {
    (void)flags;
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.send(entry->kernel_handle, buf, len, flags);
}

int bsd_recv(int sockfd, void* buf, uint32_t len, int flags) {
    (void)flags;
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.recv(entry->kernel_handle, buf, len, flags);
}

int bsd_sendto(int sockfd, const void* buf, uint32_t len, int flags,
               const void* addr, uint32_t addrlen) {
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.sendto(entry->kernel_handle, buf, len, flags, addr, addrlen);
}

int bsd_recvfrom(int sockfd, void* buf, uint32_t len, int flags,
                 void* addr, uint32_t* addrlen) {
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.recvfrom(entry->kernel_handle, buf, len, flags, addr, addrlen);
}

int bsd_shutdown(int sockfd, int how) {
    (void)how;
    bsd_fd_entry_t* entry = bsd_get_fd(sockfd);
    if (!entry || entry->type != FD_TYPE_SOCKET) return -1;
    return g_kernel_api.close(entry->kernel_handle);
}

// --- Time ---

int bsd_gettimeofday(void* tv, void* tz) {
    (void)tz;
    if (!tv) return -1;
    // Fill in timeval struct
    uint32_t* tv_ptr = (uint32_t*)tv;
    int h, m, s;
    sys_get_time(&h, &m, &s);
    // Approximate seconds since epoch (very rough)
    tv_ptr[0] = h * 3600 + m * 60 + s;  // tv_sec
    tv_ptr[1] = 0;                        // tv_usec
    return 0;
}

int bsd_nanosleep(const void* req, void* rem) {
    (void)rem;
    if (!req) return -1;
    const uint32_t* ts = (const uint32_t*)req;
    uint32_t ms = ts[0] * 1000 + ts[1] / 1000000;
    if (ms > 0) sys_delay(ms);
    return 0;
}

// --- FD Management ---

int bsd_dup(int fd) {
    bsd_fd_entry_t* entry = bsd_get_fd(fd);
    if (!entry) return -1;
    return bsd_alloc_fd(entry->type, entry->kernel_handle, entry->path, entry->flags);
}

int bsd_dup2(int oldfd, int newfd) {
    bsd_fd_entry_t* old = bsd_get_fd(oldfd);
    if (!old) return -1;
    if (newfd < 0 || newfd >= BSD_MAX_FDS) return -1;

    // Close newfd if it's in use
    if (g_fd_table[newfd].in_use) {
        bsd_close(newfd);
    }

    // Copy the entry
    g_fd_table[newfd] = *old;
    return newfd;
}

int bsd_pipe(int pipefd[2]) {
    // CamelOS doesn't have real pipes yet
    // Create two FDs that point to a shared buffer
    pipefd[0] = bsd_alloc_fd(FD_TYPE_PIPE, -1, 0, BSD_O_RDONLY);
    pipefd[1] = bsd_alloc_fd(FD_TYPE_PIPE, -1, 0, BSD_O_WRONLY);
    if (pipefd[0] < 0 || pipefd[1] < 0) return -1;
    return 0;
}

int bsd_ioctl(int fd, uint32_t request, void* arg) {
    (void)fd; (void)request; (void)arg;
    // Stub - most ioctls are not critical
    return 0;
}

int bsd_fcntl(int fd, int cmd, int arg) {
    (void)fd; (void)cmd; (void)arg;
    // Stub
    return 0;
}

int bsd_select(int nfds, void* readfds, void* writefds, void* exceptfds, const void* timeout) {
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    // Stub - return 0 (no FDs ready)
    return 0;
}
