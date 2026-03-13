// core/cdl_loader.c
#include "../sys/cdl_defs.h"
#include "../sys/api.h"
#include "../core/memory.h"
#include "../core/string.h"
#include "../hal/drivers/serial.h"
#include "../core/window_server.h"
#include "../kernel/assets.h"
#include "elf.h"
#include "../hal/cpu/timer.h"
#include "socket.h"
#include "dns.h"
#include "http.h"

// Built-in VarArgs
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)
typedef __builtin_va_list va_list;

// Relocation Types
#ifndef R_386_32
#define R_386_32 1
#endif
#ifndef R_386_PC32
#define R_386_PC32 2
#endif
#ifndef R_386_RELATIVE
#define R_386_RELATIVE 8
#endif

// --- Missing Wrappers ---
extern window_t* active_win;
void k_print_wrapper(const char* s) { s_printf(s); }
void* k_malloc_wrapper(unsigned long s) { return kmalloc(s); }
void* k_realloc_wrapper(void* ptr, unsigned long s) { return krealloc(ptr, s); }
void k_free_wrapper(void* p) { kfree(p); }
void wrap_memset(void* p, int v, unsigned long n) {
    // Force simple byte-by-byte - no SIMD
    unsigned char* ptr = (unsigned char*)p;
    unsigned char val = (unsigned char)v;
    for(unsigned long i = 0; i < n; i++) {
        ptr[i] = val;
    }
}

void wrap_memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* dst = (unsigned char*)d;
    const unsigned char* src = (const unsigned char*)s;
    for(unsigned long i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void wrap_memmove(void* d, const void* s, unsigned long n) {
    unsigned char* dst = (unsigned char*)d;
    const unsigned char* src = (const unsigned char*)s;
    
    if (dst < src) {
        for(unsigned long i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for(unsigned long i = n; i > 0; i--) dst[i-1] = src[i-1];
    }
}
void wrap_strcpy(char* d, const char* s) { strcpy(d, s); }
void wrap_strncpy(char* d, const char* s, unsigned long n) { strncpy(d, s, n); }
int wrap_strcmp(const char* s1, const char* s2) { return strcmp(s1, s2); }
int wrap_strncmp(const char* s1, const char* s2, unsigned long n) { return strncmp(s1, s2, n); }
char* wrap_strchr(const char* s, int c) { return strchr(s, c); }
char* wrap_strstr(const char* h, const char* n) { return strstr(h, n); }

unsigned long wrap_strlen(const char* s) { return strlen(s); }
uint32_t wrap_get_fs_generation() { return sys_get_fs_generation(); }

#ifndef DT_JMPREL
#define DT_JMPREL 23
#endif
#ifndef DT_PLTRELSZ
#define DT_PLTRELSZ 2
#endif

// --- Wrappers ---

// Robust sprintf for Apps
int wrap_sprintf(char* buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char* orig = buf;
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char* s = va_arg(args, const char*);
                if(!s) s = "(null)";
                while (*s) *buf++ = *s++;
            } else if (*fmt == 'd') {
                int val = va_arg(args, int);
                char tmp[32];
                extern void int_to_str(int, char*);
                int_to_str(val, tmp);
                char* p = tmp;
                while (*p) *buf++ = *p++;
            } else if (*fmt == 'c') {
                char c = (char)va_arg(args, int);
                *buf++ = c;
            } else if (*fmt == '0' && *(fmt+1) == '2' && *(fmt+2) == 'X') {
                fmt+=2;
                int val = va_arg(args, int);
                const char* hex = "0123456789ABCDEF";
                *buf++ = hex[(val >> 4) & 0xF];
                *buf++ = hex[val & 0xF];
            } else {
                *buf++ = '%'; 
                if(*fmt) *buf++ = *fmt; // Print literal if unknown
            }
            fmt++;
        } else {
            *buf++ = *fmt++;
        }
    }
    *buf = 0;
    va_end(args);
    return buf - orig;
}

// Socket Wrappers
int wrap_socket(int d, int t, int p) { return k_socket(d, t, p); }
int wrap_bind(int s, const void* a, int al) { return k_bind(s, (const sockaddr_in_t*)a); }
int wrap_connect(int s, const void* a, int al) { return k_connect(s, (const sockaddr_in_t*)a); }
int wrap_sendto(int s, const void* b, unsigned long l, int f, const void* d, int al) { return k_sendto(s, b, l, f, (const sockaddr_in_t*)d); }
int wrap_recvfrom(int s, void* b, unsigned long l, int f, void* sa, int* al) { return k_recvfrom(s, b, l, f, (sockaddr_in_t*)sa); }
int wrap_send(int s, const void* b, unsigned long l, int f) { return k_sendto(s, b, l, f, 0); }
int wrap_recv(int s, void* b, unsigned long l, int f) { return k_recvfrom(s, b, l, f, 0); }
int wrap_close(int fd) { return k_close(fd); }

// DNS Wrapper
int wrap_dns_resolve(const char* hostname, char* ip_out, int max_len) {
    if (!hostname || !ip_out || max_len < 16) return -1;
    return dns_resolve(hostname, ip_out, max_len);
}

int wrap_net_get_if_info(char* name, char* out_ip, char* out_mac) {
    net_if_t* iface = net_get_by_name(name);
    if(!iface) return -1;
    wrap_sprintf(out_ip, "%d.%d.%d.%d", ((uint8_t*)&iface->ip_addr)[0], ((uint8_t*)&iface->ip_addr)[1], ((uint8_t*)&iface->ip_addr)[2], ((uint8_t*)&iface->ip_addr)[3]);
    wrap_sprintf(out_mac, "%02X:%02X:%02X:%02X:%02X:%02X", iface->mac[0], iface->mac[1], iface->mac[2], iface->mac[3], iface->mac[4], iface->mac[5]);
    return 0;
}

// ... (Other standard wrappers for graphics, fs, etc.) ...
void wrap_exit() { if (active_win) { active_win->anim_state = 2; active_win->anim_t = 0.0f; } }
int wrap_exec(const char* path) {
    char actual_path[128]; strncpy(actual_path, path, 128);
    int len = strlen(path);
    if(len > 4 && strcmp(path + len - 4, ".app") == 0) {
        actual_path[len - 4] = '\0'; strcat(actual_path, ".cdl");
    }
    return internal_load_library(actual_path);
}
extern void int_to_str(int, char*);
extern uint32_t k_get_free_mem();
extern uint32_t k_get_total_mem();
uint32_t wrap_mem_used() { return k_get_total_mem() - k_get_free_mem(); }
uint32_t wrap_mem_total() { return k_get_total_mem(); }
int wrap_ping(const char* ip, char* buf, int len) { return sys_net_ping(ip, buf, len); }
int wrap_fs_list(const char* p, void* b, int c) { return sys_fs_list_dir(p, b, c); }
static char g_launch_args[256] = {0};
void sys_set_launch_args(const char* args) { if(args) strncpy(g_launch_args, args, 255); else g_launch_args[0]=0; }
int wrap_exec_with_args(const char* p, const char* a) { sys_set_launch_args(a); return wrap_exec(p); }
void wrap_get_args(char* b, int m) { if(b) strncpy(b, g_launch_args, m); }
void* wrap_create_win(const char* t, int w, int h, void* p, void* i, void* m) { return ws_create_window(t, w, h, p, i, m); }
void wrap_draw_text_clip(int x, int y, const char* s, int c, int m) { sys_gfx_string(x, y, s, c); }
void wrap_draw_img(int x, int y, const char* n) {
    uint32_t c=0; const embedded_image_t* a=get_embedded_images(&c);
    for(uint32_t i=0;i<c;i++) if(strcmp(a[i].name,n)==0) gfx_draw_asset_scaled(0,x,y,a[i].data,a[i].width,a[i].height,a[i].width,a[i].height);
}
void wrap_draw_img_s(int x, int y, int w, int h, const char* n) {
    uint32_t c=0; const embedded_image_t* a=get_embedded_images(&c);
    for(uint32_t i=0;i<c;i++) if(strcmp(a[i].name,n)==0) gfx_draw_asset_scaled(0,x,y,a[i].data,a[i].width,a[i].height,w,h);
}
void wrap_draw_rrect(int x, int y, int w, int h, int c, int r) { gfx_fill_rounded_rect(x, y, w, h, c, r); }
void wrap_set_menu(win_handle_t w, menu_def_t* m, int c, menu_cb_t cb) {
    window_t* win=(window_t*)w; if(!win) return;
    win->menu_count=(c>MAX_MENUS)?MAX_MENUS:c; win->on_menu_action=(void*)cb;
    for(int i=0; i<win->menu_count; i++) {
        strncpy(win->menus[i].name, m[i].name, 11);
        win->menus[i].item_count=m[i].item_count;
        for(int k=0; k<m[i].item_count; k++) strncpy(win->menus[i].items[k].label, m[i].items[k].label, 15);
    }
}

// Process events during long operations (like HTTP requests)
// This keeps the UI responsive by polling network and updating display
void wrap_process_events() {
    extern void rtl8139_poll();
    rtl8139_poll();  // Poll network card
    
    // Redraw the active window completely
    if (active_win && active_win->paint_callback) {
        // Draw window frame first
        extern void compositor_draw_window(window_t* w);
        compositor_draw_window(active_win);
        
        // Then draw content
        typedef void (*pcb)(int,int,int,int);
        ((pcb)active_win->paint_callback)(active_win->x, active_win->y + 30, active_win->width, active_win->height - 30);
        
        // Swap buffers to show the update
        extern void gfx_swap_buffers();
        gfx_swap_buffers();
    }
    
    // Small delay to prevent CPU spinning
    for(volatile int i = 0; i < 1000; i++) asm volatile("pause");
}

// === THE FIXED API TABLE ===
kernel_api_t g_kernel_api = {
    .print = k_print_wrapper, .malloc = k_malloc_wrapper, .realloc = k_realloc_wrapper, .free = k_free_wrapper,
    .exit = wrap_exit, .exec = wrap_exec, .exec_with_args = wrap_exec_with_args, .get_launch_args = wrap_get_args,
    .fs_read = sys_fs_read, .fs_write = sys_fs_write, .fs_list = wrap_fs_list, .fs_create = sys_fs_create,
    .fs_delete = sys_fs_delete, .fs_rename = sys_fs_rename, .fs_exists = sys_fs_exists,
    .create_window = wrap_create_win, .draw_rect = sys_gfx_rect, .draw_text = sys_gfx_string,
    .draw_text_clipped = wrap_draw_text_clip, .draw_image = wrap_draw_img, .draw_image_scaled = wrap_draw_img_s,
    .draw_rect_rounded = wrap_draw_rrect, .set_window_menu = wrap_set_menu,
    .memset = wrap_memset, .memcpy = wrap_memcpy, .strcpy = wrap_strcpy, .strncpy = wrap_strncpy,
    .strcmp = wrap_strcmp, .strncmp = wrap_strncmp, .strchr = wrap_strchr, .strstr = wrap_strstr,
    .memmove = wrap_memmove, .sprintf = wrap_sprintf, .strlen = wrap_strlen, .itoa = int_to_str,
    .get_ticks = get_tick_count, .mem_used = wrap_mem_used, .mem_total = wrap_mem_total,
    .get_kbd_state = sys_kbd_state, .get_fs_generation = wrap_get_fs_generation,
    .ping = wrap_ping,
    .socket = wrap_socket, .bind = wrap_bind, .connect = wrap_connect, .sendto = wrap_sendto,
    .send = wrap_send, .recvfrom = wrap_recvfrom, .recv = wrap_recv, .close = wrap_close,
    .net_get_interface_info = wrap_net_get_if_info, .dns_resolve = wrap_dns_resolve,
    .http_get = http_get_simple,
    .process_events = wrap_process_events
};

// ... (ELF Loader implementation remains the same) ...
#define MAX_LOADED_LIBS 16
typedef struct { char name[32]; void* base_addr; uint32_t size; cdl_exports_t* exports; int active; } loaded_cdl_t;
loaded_cdl_t loaded_libraries[MAX_LOADED_LIBS];

void internal_cdl_init_system() { memset(loaded_libraries, 0, sizeof(loaded_libraries)); }

void extract_unique_name(const char* path, char* out_buf) {
    char* app_ptr = strstr(path, ".app");
    if (app_ptr) {
        char* start = app_ptr;
        while (start > path && *start != '/') start--;
        if (*start == '/') start++;
        int len = (app_ptr - start) + 4; if (len > 31) len = 31;
        strncpy(out_buf, start, len); out_buf[len] = 0;
    } else {
        char* fname = strrchr(path, '/'); if (fname) fname++; else fname = (char*)path;
        strncpy(out_buf, fname, 31); out_buf[31] = 0;
    }
}

int find_loaded_library(const char* name) {
    for(int i=0; i<MAX_LOADED_LIBS; i++) if (loaded_libraries[i].active && strcmp(loaded_libraries[i].name, name) == 0) return i;
    return -1;
}

void process_relocations(Elf32_Rel* rel, int count, uint32_t load_base, uint32_t min_vaddr) {
    // Delta between where code expects to be vs where it actually is
    int32_t delta = (int32_t)(load_base - min_vaddr);
    
    char buf[32];
    serial_write_string("Processing ");
    int_to_str(count, buf);
    serial_write_string(buf);
    serial_write_string(" relocations, delta=");
    int_to_str(delta, buf);
    serial_write_string(buf);
    serial_write_string("\n");
    
    for(int i=0; i<count; i++) {
        uint32_t rel_type = ELF32_R_TYPE(rel[i].r_info);
        uint32_t sym_index = ELF32_R_SYM(rel[i].r_info);
        
        // r_offset is a VIRTUAL address - convert to actual loaded address
        uint32_t* target = (uint32_t*)(load_base + (rel[i].r_offset - min_vaddr));
        
        // Bounds check
        if ((uint32_t)target < load_base || (uint32_t)target >= load_base + 0x10000) {
            serial_write_string("WARNING: Relocation target out of bounds: ");
            int_to_str((uint32_t)target, buf);
            serial_write_string(buf);
            serial_write_string("\n");
            continue;
        }
        
        switch(rel_type) {
            case R_386_32: {
                // Direct 32-bit address: S + A
                // Add delta to make it point to correct loaded location
                *target += delta;
                break;
            }
            case R_386_RELATIVE: {
                // Base + Addend (addend is stored at target)
                *target += delta;
                break;
            }
            case R_386_PC32: {
                // PC-relative: S + A - P
                // For PC-relative, we need to adjust differently
                // The value should be: target_address - current_instruction_address
                // Since both moved by delta, this usually doesn't need adjustment
                // UNLESS the linker didn't account for PIE properly
                break;
            }
            default: {
                serial_write_string("Unknown relocation type: ");
                int_to_str(rel_type, buf);
                serial_write_string(buf);
                serial_write_string(" at offset ");
                int_to_str(rel[i].r_offset, buf);
                serial_write_string(buf);
                serial_write_string("\n");
                break;
            }
        }
    }
    serial_write_string("Relocations done\n");
}

int internal_load_library(const char* path) {
    char unique_name[32]; 
    extract_unique_name(path, unique_name);
    
    // Check if already loaded
    int existing = find_loaded_library(unique_name);
    if (existing != -1) {
        // Unload previous instance
        if (loaded_libraries[existing].base_addr) 
            kfree(loaded_libraries[existing].base_addr);
        loaded_libraries[existing].active = 0; 
        memset(&loaded_libraries[existing], 0, sizeof(loaded_cdl_t));
    }
    
    // Find free slot
    int slot = -1; 
    for(int i=0; i<MAX_LOADED_LIBS; i++) {
        if(!loaded_libraries[i].active) { 
            slot = i; 
            break; 
        }
    }
    if(slot == -1) {
        serial_write_string("CDL: No free slot\n");
        return -1;
    }
    
    // Check file exists
    if (!sys_fs_exists(path)) {
        serial_write_string("CDL: File not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return -1;
    }

    // First, read just the ELF header to determine file structure
    char header_buf[1024];
    memset(header_buf, 0, sizeof(header_buf));
    int header_size = sys_fs_read(path, header_buf, sizeof(header_buf));
    
    serial_write_string("CDL: Header size read: ");
    char num[16]; int_to_str(header_size, num);
    serial_write_string(num);
    serial_write_string("\n");
    
    if(header_size < sizeof(Elf32_Ehdr)) {
        serial_write_string("CDL: Header too small\n");
        return -1;
    }
    
    Elf32_Ehdr* ehdr = (Elf32_Ehdr*)header_buf;
    
    // Verify ELF magic
    if (ehdr->e_ident[0] != 0x7F || 
        ehdr->e_ident[1] != 'E' || 
        ehdr->e_ident[2] != 'L' || 
        ehdr->e_ident[3] != 'F') {
        serial_write_string("CDL: Invalid ELF magic\n");
        return -1;
    }
    
    // Calculate total file size needed from program headers
    uint32_t min_vaddr = 0xFFFFFFFF;
    uint32_t max_vaddr = 0;
    uint32_t max_file_offset = 0;
    
    Elf32_Phdr* phdr = (Elf32_Phdr*)(header_buf + ehdr->e_phoff);
    serial_write_string("CDL: Program headers: ");
    int_to_str(ehdr->e_phnum, num);
    serial_write_string(num);
    serial_write_string("\n");
    
    for(int i=0; i<ehdr->e_phnum; i++) {
        if(phdr[i].p_type == PT_LOAD) {
            if(phdr[i].p_vaddr < min_vaddr) 
                min_vaddr = phdr[i].p_vaddr;
            uint32_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
            if(seg_end > max_vaddr) 
                max_vaddr = seg_end;
            // Track maximum file offset needed
            uint32_t file_end = phdr[i].p_offset + phdr[i].p_filesz;
            if(file_end > max_file_offset)
                max_file_offset = file_end;
        }
    }
    
    serial_write_string("CDL: max_file_offset: ");
    int_to_str(max_file_offset, num);
    serial_write_string(num);
    serial_write_string("\n");
    
    // Allocate buffer for entire file (dynamically sized)
    uint32_t file_buf_size = (max_file_offset + 4095) & ~4095; // Page align
    serial_write_string("CDL: file_buf_size: ");
    int_to_str(file_buf_size, num);
    serial_write_string(num);
    serial_write_string("\n");
    
    char* raw_file_buffer = (char*)kmalloc(file_buf_size);
    if(!raw_file_buffer) {
        serial_write_string("CDL: Failed to allocate file buffer\n");
        return -1;
    }
    memset(raw_file_buffer, 0, file_buf_size);
    
    // Read entire file
    int fsize = sys_fs_read(path, raw_file_buffer, file_buf_size);
    serial_write_string("CDL: File size read: ");
    int_to_str(fsize, num);
    serial_write_string(num);
    serial_write_string("\n");
    
    if(fsize < sizeof(Elf32_Ehdr)) {
        serial_write_string("CDL: File too small\n");
        kfree(raw_file_buffer);
        return -1;
    }
    
    // Re-point ehdr and phdr to the full buffer
    ehdr = (Elf32_Ehdr*)raw_file_buffer;
    phdr = (Elf32_Phdr*)(raw_file_buffer + ehdr->e_phoff);
    
    uint32_t total_size = max_vaddr - min_vaddr;
    
    // IMPORTANT: Align to page boundary
    total_size = (total_size + 4095) & ~4095;
    
    // Allocate memory for code/data
    char* load_base = (char*)kmalloc(total_size);
    if(!load_base) {
        kfree(raw_file_buffer);
        return -1;
    }
    
    // Clear allocated memory
    memset(load_base, 0, total_size);
    
    // Load segments
    for(int i=0; i<ehdr->e_phnum; i++) {
        if(phdr[i].p_type == PT_LOAD) {
            uint32_t dest_addr = (uint32_t)load_base + (phdr[i].p_vaddr - min_vaddr);
            
            // Copy the segment data
            if(phdr[i].p_filesz > 0) {
                memcpy((void*)dest_addr, 
                       raw_file_buffer + phdr[i].p_offset, 
                       phdr[i].p_filesz);
            }
            
            // Zero out the rest (BSS)
            if(phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((void*)(dest_addr + phdr[i].p_filesz), 
                       0, 
                       phdr[i].p_memsz - phdr[i].p_filesz);
            }
        }
    }
    
    // Process dynamic relocations
    Elf32_Dyn* dyn = 0;
    for(int i=0; i<ehdr->e_phnum; i++) {
        if(phdr[i].p_type == PT_DYNAMIC) { 
            dyn = (Elf32_Dyn*)(load_base + (phdr[i].p_vaddr - min_vaddr)); 
            break; 
        }
    }
    
    if(dyn) {
        Elf32_Rel* rel = 0;
        uint32_t relsz = 0;
        Elf32_Rel* pltrel = 0;
        uint32_t pltrelsz = 0;
        uint32_t relent = sizeof(Elf32_Rel);
        
        while(dyn->d_tag != DT_NULL) {
            if (dyn->d_tag == DT_REL) 
                rel = (Elf32_Rel*)(load_base + (dyn->d_un.d_ptr - min_vaddr));
            else if (dyn->d_tag == DT_RELSZ) 
                relsz = dyn->d_un.d_val;
            else if (dyn->d_tag == DT_RELENT) 
                relent = dyn->d_un.d_val;
            else if (dyn->d_tag == DT_JMPREL) 
                pltrel = (Elf32_Rel*)(load_base + (dyn->d_un.d_ptr - min_vaddr));
            else if (dyn->d_tag == DT_PLTRELSZ) 
                pltrelsz = dyn->d_un.d_val;
            dyn++;
        }
        
        if(relent == 0) relent = sizeof(Elf32_Rel);
        
        if(rel && relsz > 0) 
            process_relocations(rel, relsz / relent, (uint32_t)load_base, min_vaddr);
        
        if(pltrel && pltrelsz > 0) 
            process_relocations(pltrel, pltrelsz / relent, (uint32_t)load_base, min_vaddr);
    }
    
    // Free the file buffer - we don't need it anymore
    kfree(raw_file_buffer);
    raw_file_buffer = 0;
    
    // Call entry point
    uint32_t entry_addr = (uint32_t)load_base + (ehdr->e_entry - min_vaddr);
    cdl_entry_func entry_func = (cdl_entry_func)entry_addr;
    
    // DEBUG: Log entry point
    char buf[32];
    int_to_str(entry_addr, buf);
    serial_write_string("CDL: Calling entry at ");
    serial_write_string(buf);
    serial_write_string("\n");
    
    cdl_exports_t* exports = entry_func(&g_kernel_api);
    
    // Store library info
    strcpy(loaded_libraries[slot].name, unique_name);
    loaded_libraries[slot].base_addr = load_base;
    loaded_libraries[slot].size = total_size;
    loaded_libraries[slot].exports = exports;
    loaded_libraries[slot].active = 1;
    
    return slot;
}

void* internal_get_proc_address(int lib_handle, const char* symbol_name) {
    if(lib_handle < 0 || !loaded_libraries[lib_handle].active) return 0;
    cdl_exports_t* ex = loaded_libraries[lib_handle].exports; if(!ex) return 0;
    for(int i=0; i<ex->symbol_count; i++) if(strcmp(ex->symbols[i].name, symbol_name) == 0) return ex->symbols[i].func_ptr;
    return 0;
}
void internal_unload_library(int lib_handle) { if(lib_handle >= 0 && loaded_libraries[lib_handle].active) loaded_libraries[lib_handle].active = 0; }
void internal_cdl_list_libraries() {}