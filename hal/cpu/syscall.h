// hal/cpu/syscall.h - Syscall interface for CamelOS
// Provides a proper int 0x80 syscall mechanism for the monolithic kernel
// This gives apps a real system call interface like Linux/Windows

#ifndef SYSCALL_H
#define SYSCALL_H

#include "../../include/types.h"

// ============================================================================
// Syscall Numbers
// ============================================================================
// These numbers are used with: int 0x80, EAX = syscall number
// Arguments: EBX=arg1, ECX=arg2, EDX=arg3, ESI=arg4, EDI=arg5
// Return: EAX = result (0 = success, negative = error)

#define SYS_EXIT        1    // void exit(int code)
#define SYS_EXEC        2    // int exec(const char* path)
#define SYS_EXEC_ARGS   3    // int exec_with_args(const char* path, const char* args)
#define SYS_GET_ARGS    4    // void get_launch_args(char* buf, int max_len)

#define SYS_PRINT       10   // void print(const char* str)
#define SYS_MALLOC      11   // void* malloc(unsigned long size)
#define SYS_REALLOC     12   // void* realloc(void* ptr, unsigned long size)
#define SYS_FREE        13   // void free(void* ptr)

#define SYS_FS_READ     20   // int fs_read(const char* path, char* buf, int size)
#define SYS_FS_WRITE    21   // int fs_write(const char* path, char* buf, int size)
#define SYS_FS_LIST     22   // int fs_list(const char* path, void* buf, int count)
#define SYS_FS_CREATE   23   // int fs_create(const char* path, int size)
#define SYS_FS_DELETE   24   // int fs_delete(const char* path)
#define SYS_FS_RENAME   25   // int fs_rename(const char* old, const char* new)
#define SYS_FS_EXISTS   26   // int fs_exists(const char* path)

#define SYS_CREATE_WIN  30   // win_handle_t create_window(...)
#define SYS_DRAW_RECT   31   // void draw_rect(int x, int y, int w, int h, int color)
#define SYS_DRAW_TEXT   32   // void draw_text(int x, int y, const char* str, int color)
#define SYS_DRAW_RRECT  33   // void draw_rect_rounded(...)
#define SYS_DRAW_IMG    34   // void draw_image(...)
#define SYS_DRAW_PIX    35   // void draw_pixels(...)
#define SYS_SET_MENU    36   // void set_window_menu(...)

#define SYS_GET_TICKS   40   // uint32_t get_ticks()
#define SYS_MEM_USED    41   // uint32_t mem_used()
#define SYS_MEM_TOTAL   42   // uint32_t mem_total()
#define SYS_KBD_STATE   43   // void get_kbd_state(int* ctrl, int* shift, int* alt)

#define SYS_SOCKET      50   // int socket(int domain, int type, int protocol)
#define SYS_BIND        51   // int bind(int fd, const void* addr, int len)
#define SYS_CONNECT     52   // int connect(int fd, const void* addr, int len)
#define SYS_SEND        53   // int send(int fd, const void* buf, unsigned long len, int flags)
#define SYS_RECV        54   // int recv(int fd, void* buf, unsigned long len, int flags)
#define SYS_SENDTO      55   // int sendto(int fd, const void* buf, unsigned long len, int flags, const void* addr, int alen)
#define SYS_RECVFROM    56   // int recvfrom(int fd, void* buf, unsigned long len, int flags, void* addr, int* alen)
#define SYS_CLOSE       57   // int close(int fd)
#define SYS_DNS         58   // int dns_resolve(const char* host, char* ip_out, int max_len)
#define SYS_HTTP_GET    59   // int http_get(const char* url, char* resp, int size, const char** hdrs, int hcount)
#define SYS_NET_INFO    60   // int net_get_interface_info(char* name, char* ip, char* mac)
#define SYS_PING        61   // int ping(const char* ip, char* buf, int len)

#define SYS_PROCESS_EVENTS 70 // void process_events()

// --- Process Management (80-89) ---
#define SYS_GETPID      80   // int getpid()
#define SYS_FORK        81   // int fork()
#define SYS_WAITPID     82   // int waitpid(int pid, int* status, int options)
#define SYS_KILL        83   // int kill(int pid, int sig)
#define SYS_SIGNAL      84   // void* signal(int sig, void* handler)
#define SYS_SIGACTION   85   // int sigaction(int sig, const sigaction_t* act, sigaction_t* old)
#define SYS_SIGPROCMASK 86   // int sigprocmask(int how, uint32_t* set, uint32_t* old)

// --- Virtual Memory (90-99) ---
#define SYS_MMAP        90   // void* mmap(void* addr, unsigned long len, int prot, int flags, int fd, unsigned long off)
#define SYS_MUNMAP      91   // int munmap(void* addr, unsigned long len)
#define SYS_BRK         92   // int brk(void* addr)
#define SYS_MPROTECT    93   // int mprotect(void* addr, unsigned long len, int prot)

// --- Pipe IPC (100-109) ---
#define SYS_PIPE        100  // int pipe(int fd[2])
#define SYS_MKFIFO      101  // int mkfifo(const char* path, int mode)
#define SYS_READ_PIPE   102  // int read_pipe(int fd, void* buf, unsigned long count)
#define SYS_WRITE_PIPE  103  // int write_pipe(int fd, const void* buf, unsigned long count)
#define SYS_IOCTL_PIPE  104  // int ioctl_pipe(int fd, int cmd, void* arg)

// --- Notification (110-119) ---
#define SYS_NOTIFY_POST 110  // int notify_post(const char* title, const char* body, const char* source, int priority, int category)
#define SYS_NOTIFY_DISMISS 111 // int notify_dismiss(int id)
#define SYS_NOTIFY_CLICK 112  // int notify_click(int x, int y)
#define SYS_NOTIFY_DND  113  // int notify_set_dnd(int enabled)

// ============================================================================
// Syscall Register State (pushed by assembly stub)
// ============================================================================
typedef struct {
    // Pushed by pusha (in order): edi, esi, ebp, esp, ebx, edx, ecx, eax
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;    // Kernel ESP at time of pusha
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;    // Syscall number on entry, return value on exit
    
    // Pushed by our stub
    uint32_t gs, fs, es, ds;
} syscall_regs_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize the syscall system (installs IDT entry for int 0x80)
void init_syscall(void);

// C handler called from assembly
// Takes pointer to saved register state, dispatches to correct syscall
void syscall_handler(syscall_regs_t* regs);

// Inline assembly helper for making syscalls from C code
static inline int syscall0(int num) {
    int result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "a"(num)
        : "memory"
    );
    return result;
}

static inline int syscall1(int num, int arg1) {
    int result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "a"(num), "b"(arg1)
        : "memory"
    );
    return result;
}

static inline int syscall2(int num, int arg1, int arg2) {
    int result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "a"(num), "b"(arg1), "c"(arg2)
        : "memory"
    );
    return result;
}

static inline int syscall3(int num, int arg1, int arg2, int arg3) {
    int result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return result;
}

static inline int syscall5(int num, int arg1, int arg2, int arg3, int arg4, int arg5) {
    int result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4), "D"(arg5)
        : "memory"
    );
    return result;
}

#endif // SYSCALL_H
