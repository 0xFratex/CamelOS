// sys/syscalls.h — CamelOS canonical system-call ABI
//
// This is the FROZEN, authoritative list of syscall numbers and the register
// convention used by both Ring 0 and Ring 3 callers.
//
//   Entry  : int 0x80 (legacy) and the sysenter/sysexit fast path
//   EAX    : syscall number
//   EBX,ECX,EDX,ESI,EDI : arguments 1..5 (left to right)
//   EAX    : return value (0 = success, negative = error)
//
// RULES:
//   * DO NOT renumber an existing syscall — that breaks the ABI.
//   * Append new syscalls in a free block; keep numbers stable forever.
//   * Bump CAMELOS_SYSCALL_ABI_VERSION only for a breaking change.
#ifndef CAMELOS_SYSCALLS_H
#define CAMELOS_SYSCALLS_H

#define CAMELOS_SYSCALL_ABI_VERSION 1

/* --- Process & task control (1-9) --- */
#define SYS_EXIT          1   // void exit(int code)
#define SYS_EXEC          2   // int exec(const char* path)
#define SYS_EXEC_ARGS     3   // int exec_with_args(const char* path, const char* args)
#define SYS_GET_ARGS      4   // void get_launch_args(char* buf, int max_len)

/* --- Console & memory (10-19) --- */
#define SYS_PRINT         10  // void print(const char* str)
#define SYS_MALLOC        11  // void* malloc(unsigned long size)
#define SYS_REALLOC       12  // void* realloc(void* ptr, unsigned long size)
#define SYS_FREE          13  // void free(void* ptr)

/* --- Filesystem (20-29) --- */
#define SYS_FS_READ       20  // int fs_read(const char* path, char* buf, int size)
#define SYS_FS_WRITE      21  // int fs_write(const char* path, char* buf, int size)
#define SYS_FS_LIST       22  // int fs_list(const char* path, void* buf, int count)
#define SYS_FS_CREATE     23  // int fs_create(const char* path, int size)
#define SYS_FS_DELETE     24  // int fs_delete(const char* path)
#define SYS_FS_RENAME     25  // int fs_rename(const char* old, const char* new)
#define SYS_FS_EXISTS     26  // int fs_exists(const char* path)

/* --- Window / graphics (30-39) --- */
#define SYS_CREATE_WIN    30  // win_handle_t create_window(title,w,h,paint,input,mouse)
#define SYS_DRAW_RECT     31  // void draw_rect(int x,int y,int w,int h,int color)
#define SYS_DRAW_TEXT     32  // void draw_text(int x,int y,const char* str,int color)
#define SYS_DRAW_RRECT    33  // void draw_rect_rounded(x,y,w,h,color,radius)
#define SYS_DRAW_IMG      34  // void draw_image(x,y,name)
#define SYS_DRAW_PIX      35  // void draw_pixels(x,y,w,h,const uint32_t* data)
#define SYS_SET_MENU      36  // void set_window_menu(win, menus, count, cb)

/* --- System info (40-49) --- */
#define SYS_GET_TICKS     40  // uint32_t get_ticks()
#define SYS_MEM_USED      41  // uint32_t mem_used()
#define SYS_MEM_TOTAL     42  // uint32_t mem_total()
#define SYS_KBD_STATE     43  // void get_kbd_state(int* ctrl,int* shift,int* alt)

/* --- Networking (50-69) --- */
#define SYS_SOCKET        50  // int socket(int domain,int type,int protocol)
#define SYS_BIND          51  // int bind(int fd,const void* addr,int len)
#define SYS_CONNECT       52  // int connect(int fd,const void* addr,int len)
#define SYS_SEND          53  // int send(int fd,const void* buf,ulong len,int flags)
#define SYS_RECV          54  // int recv(int fd,void* buf,ulong len,int flags)
#define SYS_SENDTO        55  // int sendto(fd,buf,len,flags,addr,alen)
#define SYS_RECVFROM      56  // int recvfrom(fd,buf,len,flags,addr,alen)
#define SYS_CLOSE         57  // int close(int fd)
#define SYS_DNS           58  // int dns_resolve(host,ip_out,max_len)
#define SYS_HTTP_GET      59  // int http_get(url,resp,size,hdrs,hcount)
#define SYS_NET_INFO      60  // int net_get_interface_info(name,ip,mac)
#define SYS_PING          61  // int ping(ip,buf,len)
#define SYS_LISTEN        62  // int listen(fd,backlog)
#define SYS_ACCEPT        63  // int accept(fd,addr,addrlen)

/* --- Event processing (70-79) --- */
#define SYS_PROCESS_EVENTS 70 // void process_events()

/* --- Process management (80-89) --- */
#define SYS_GETPID        80  // int getpid()
#define SYS_FORK          81  // int fork()
#define SYS_WAITPID       82  // int waitpid(pid,status,options)
#define SYS_KILL          83  // int kill(pid,sig)
#define SYS_SIGNAL        84  // void* signal(sig,handler)
#define SYS_SIGACTION     85  // int sigaction(sig,act,old)
#define SYS_SIGPROCMASK   86  // int sigprocmask(how,set,old)

/* --- Virtual memory (90-99) --- */
#define SYS_MMAP          90  // void* mmap(addr,len,prot,flags,fd,off)
#define SYS_MUNMAP        91  // int munmap(addr,len)
#define SYS_BRK           92  // int brk(addr)
#define SYS_MPROTECT      93  // int mprotect(addr,len,prot)

/* --- Pipe IPC (100-109) --- */
#define SYS_PIPE          100 // int pipe(int fd[2])
#define SYS_MKFIFO        101 // int mkfifo(path,mode)
#define SYS_READ_PIPE     102 // int read_pipe(fd,buf,count)
#define SYS_WRITE_PIPE    103 // int write_pipe(fd,buf,count)
#define SYS_IOCTL_PIPE    104 // int ioctl_pipe(fd,cmd,arg)

/* --- Notification (110-119) --- */
#define SYS_NOTIFY_POST    110 // int notify_post(title,body,source,priority,category)
#define SYS_NOTIFY_DISMISS 111 // int notify_dismiss(id)
#define SYS_NOTIFY_CLICK   112 // int notify_click(x,y)
#define SYS_NOTIFY_DND     113 // int notify_set_dnd(enabled)

/* --- Canonical user-mode (Ring 3) POSIX subset (120-129) --- */
#define SYS_USER_EXIT     120 // void exit(int code)
#define SYS_USER_READ     121 // int read(fd,buf,count)
#define SYS_USER_WRITE    122 // int write(fd,buf,count)
#define SYS_USER_OPEN     123 // int open(path,flags,mode)
#define SYS_USER_CLOSE    124 // int close(fd)
#define SYS_USER_FORK     125 // int fork()
#define SYS_USER_EXEC     126 // int exec(path,argv)
#define SYS_USER_YIELD    127 // void yield()

/* --- Ring 3 window (128-129) --- */
#define SYS_USER_WIN_CREATE 128 // int win_create(title,w,h,&x,&y) -> win id

/* --- Extended file/process/scheduler (130-149) --- */
#define SYS_stat          130
#define SYS_chmod         131
#define SYS_chown         132
#define SYS_mount         133
#define SYS_umount        134
#define SYS_ioctl         135
#define SYS_gettimeofday  136
#define SYS_setuid        137
#define SYS_getuid        138
#define SYS_chdir         139
#define SYS_sync          140
#define SYS_access        141
#define SYS_dup           142
#define SYS_dup2          143
#define SYS_pipe2         144
#define SYS_fcntl         145
#define SYS_sched_set_policy 146
#define SYS_sched_get_policy 147
#define SYS_sched_set_nice   148
#define SYS_sched_get_nice   149

/* ============================================================================
 * Inline syscall helpers (work from Ring 0 and Ring 3).
 * ==========================================================================*/

static inline int syscall0(int num) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(num) : "memory");
    return result;
}

static inline int syscall1(int num, int arg1) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(num), "b"(arg1) : "memory");
    return result;
}

static inline int syscall2(int num, int arg1, int arg2) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(num), "b"(arg1), "c"(arg2) : "memory");
    return result;
}

static inline int syscall3(int num, int arg1, int arg2, int arg3) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result)
                     : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3) : "memory");
    return result;
}

static inline int syscall4(int num, int arg1, int arg2, int arg3, int arg4) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result)
                     : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4) : "memory");
    return result;
}

static inline int syscall5(int num, int arg1, int arg2, int arg3, int arg4, int arg5) {
    int result;
    __asm__ volatile("int $0x80" : "=a"(result)
                     : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4), "D"(arg5) : "memory");
    return result;
}

#endif /* CAMELOS_SYSCALLS_H */
