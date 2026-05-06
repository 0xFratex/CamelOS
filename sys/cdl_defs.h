// sys/cdl_defs.h
#ifndef CDL_DEFS_H
#define CDL_DEFS_H

#include "types.h"
typedef void* win_handle_t;

// Callbacks
typedef void (*paint_cb_t)(int x, int y, int w, int h);
typedef void (*input_cb_t)(int key);
typedef void (*mouse_cb_t)(int x, int y, int btn);
typedef void (*menu_cb_t)(int menu_idx, int item_idx);

#define MAX_MENU_ITEMS 5
#define MAX_MENUS 4

typedef struct {
    char name[12];
    struct { char label[16]; char action_id[32]; } items[MAX_MENU_ITEMS];
    int item_count;
} menu_def_t;

// --- STABLE KERNEL API TABLE ---
// Do not change the order of fields without recompiling ALL apps!
typedef struct {
    // 1. System & Memory
    void (*print)(const char*);
    void* (*malloc)(unsigned long);
    void* (*realloc)(void*, unsigned long);
    void (*free)(void*);
    void (*exit)(void);
    int  (*exec)(const char* path);
    int  (*exec_with_args)(const char* path, const char* args);
    void (*get_launch_args)(char* buf, int max_len);

    // 2. Filesystem
    int (*fs_read)(const char*, char*, int);
    int (*fs_write)(const char*, char*, int);
    int (*fs_list)(const char*, void*, int);
    int (*fs_create)(const char*, int);
    int (*fs_delete)(const char*);
    int (*fs_rename)(const char*, const char*);
    int (*fs_exists)(const char*);
    
    // 3. GUI & Graphics
    win_handle_t (*create_window)(const char* title, int w, int h, paint_cb_t p, input_cb_t i, mouse_cb_t m);
    void (*draw_rect)(int x, int y, int w, int h, int color);
    void (*draw_text)(int x, int y, const char* str, int color);
    void (*draw_text_clipped)(int x, int y, const char* str, int color, int max_w);
    void (*draw_image)(int x, int y, const char* name);
    void (*draw_image_scaled)(int x, int y, int w, int h, const char* name);
    void (*draw_rect_rounded)(int x, int y, int w, int h, int color, int radius);
    void (*set_window_menu)(win_handle_t win, menu_def_t* menus, int count, menu_cb_t cb);

    // 4. String & Utils
    void (*memset)(void*, int, unsigned long);
    void (*memcpy)(void*, const void*, unsigned long);
    void (*strcpy)(char*, const char*);
    void (*strncpy)(char*, const char*, unsigned long);
    int  (*strcmp)(const char*, const char*);
    int  (*strncmp)(const char*, const char*, unsigned long);
    char* (*strchr)(const char*, int);
    char* (*strstr)(const char*, const char*);
    void (*memmove)(void*, const void*, unsigned long);
    int  (*sprintf)(char*, const char*, ...);
    unsigned long (*strlen)(const char*);
    void (*itoa)(int, char*);

    // 5. Hardware & Stats
    uint32_t (*get_ticks)(void);
    uint32_t (*mem_used)(void);
    uint32_t (*mem_total)(void);
    void (*get_kbd_state)(int* ctrl, int* shift, int* alt);
    uint32_t (*get_fs_generation)(void);

    // 6. Network (Socket API)
    int (*ping)(const char* ip_str, char* out_buf, int max_len);
    int (*socket)(int domain, int type, int protocol);
    int (*bind)(int sockfd, const void* addr, int addrlen);
    int (*connect)(int sockfd, const void* addr, int addrlen);
    int (*sendto)(int sockfd, const void* buf, unsigned long len, int flags, const void* dest_addr, int addrlen);
    int (*send)(int sockfd, const void* buf, unsigned long len, int flags);
    int (*recvfrom)(int sockfd, void* buf, unsigned long len, int flags, void* src_addr, int* addrlen);
    int (*recv)(int sockfd, void* buf, unsigned long len, int flags);
    int (*close)(int fd);
    int (*listen)(int sockfd, int backlog);
    int (*accept)(int sockfd, void* addr, int* addrlen);
    int (*net_get_interface_info)(char* name, char* out_ip, char* out_mac);
    int (*dns_resolve)(const char* hostname, char* ip_out, int max_len);
    int (*http_get)(const char* url, char* response, int response_size, const char** headers, int header_count);
    
    // 7. Event Processing (for async operations)
    void (*process_events)(void);  // Process window events during long operations
    
    // 8. Custom Additions
    void (*draw_pixels)(int x, int y, int w, int h, const uint32_t* data);

    // 9. CDL Module Loading
    void* (*cdl_load)(const char* path);
    void* (*cdl_sym)(void* module, const char* name);

} kernel_api_t;

typedef struct { char name[32]; void* func_ptr; } cdl_symbol_t;
typedef struct { char lib_name[32]; int version; int symbol_count; cdl_symbol_t* symbols; } cdl_exports_t;
typedef cdl_exports_t* (*cdl_entry_func)(kernel_api_t* api);

// ============================================================================
// SYSCALL-BASED API
// Apps can use these instead of the function pointer table for a more
// "monolithic kernel" feel. These use int 0x80 to invoke kernel services.
// This is the preferred way for new apps - it's more like how Linux/Windows work.
// ============================================================================
#include "../hal/cpu/syscall.h"

// Convenience macros that wrap syscalls for CDL apps
// These can be used alongside or instead of the kernel_api_t function pointers

#define cam_print(str)          syscall1(SYS_PRINT, (int)(str))
#define cam_malloc(size)        syscall1(SYS_MALLOC, (int)(size))
#define cam_free(ptr)           syscall1(SYS_FREE, (int)(ptr))
#define cam_exit()              syscall0(SYS_EXIT)

#define cam_fs_read(p,b,s)     syscall3(SYS_FS_READ, (int)(p), (int)(b), (int)(s))
#define cam_fs_write(p,b,s)    syscall3(SYS_FS_WRITE, (int)(p), (int)(b), (int)(s))
#define cam_fs_exists(p)       syscall1(SYS_FS_EXISTS, (int)(p))
#define cam_fs_delete(p)       syscall1(SYS_FS_DELETE, (int)(p))
#define cam_fs_create(p,s)     syscall2(SYS_FS_CREATE, (int)(p), (int)(s))

#define cam_draw_rect(x,y,w,h,c) syscall5(SYS_DRAW_RECT, (int)(x), (int)(y), (int)(w), (int)(h), (int)(c))
#define cam_draw_text(x,y,s,c)   syscall3(SYS_DRAW_TEXT, (int)(x), (int)(y), (int)(s)) // Note: 4 args needs special handling

#define cam_get_ticks()        syscall0(SYS_GET_TICKS)
#define cam_mem_used()         syscall0(SYS_MEM_USED)
#define cam_mem_total()        syscall0(SYS_MEM_TOTAL)

#define cam_http_get(u,r,s,h,hc) syscall5(SYS_HTTP_GET, (int)(u), (int)(r), (int)(s), (int)(h), (int)(hc))
#define cam_dns_resolve(h,o,m)    syscall3(SYS_DNS, (int)(h), (int)(o), (int)(m))
#define cam_socket(d,t,p)         syscall3(SYS_SOCKET, (int)(d), (int)(t), (int)(p))
#define cam_connect(f,a,l)        syscall3(SYS_CONNECT, (int)(f), (int)(a), (int)(l))
#define cam_close(fd)             syscall1(SYS_CLOSE, (int)(fd))

#define cam_process_events()      syscall0(SYS_PROCESS_EVENTS)

#endif