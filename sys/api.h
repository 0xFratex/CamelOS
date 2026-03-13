#ifndef SYS_API_H
#define SYS_API_H
#include "../fs/pfs32.h"

// Forward declarations
extern void* kmalloc(unsigned long size);
extern void kfree(void* ptr);

// --- System ---
void sys_init();
void sys_shutdown();
void sys_reboot();
void sys_delay(int amount);
void sys_get_time(int* h, int* m, int* s);

// --- Notification ---
uint32_t sys_get_fs_generation();
void sys_notify_fs_change();

// --- Helpers ---
int sys_fs_delete_recursive(const char* path);
void sys_fs_generate_unique_name(const char* parent, const char* base_name, int is_dir, char* out_name);
void sys_fs_copy_recursive(const char* src, const char* dest);

// --- Input/Output ---
void sys_print(const char* str);
void sys_clear();
int sys_get_key();
int sys_wait_key();
void sys_flush_input();
int  sys_mouse_read(int* x, int* y, int* left_click);
void sys_kbd_state(int* ctrl, int* shift, int* alt); // Updated to include Alt

// --- Storage API ---
int sys_fs_mount();
int sys_fs_ls(const char* path);
int sys_fs_list_dir(const char* path, void* buffer, int max_len);
int sys_fs_write(const char* full_path, char* data, int size);
int sys_fs_read(const char* full_path, char* buffer, int max_len);
int sys_fs_create(const char* full_path, int is_dir);
int sys_fs_delete(const char* full_path);
int sys_fs_exists(const char* full_path);
int sys_fs_is_dir(const char* full_path);
int sys_fs_rename(const char* old_path, const char* new_path);
void sys_fs_copy(const char* src, const char* dest);

// --- User/Clipboard ---
int sys_get_uid();
void sys_set_uid(int uid);
void sys_clipboard_set(const char* text);
int sys_clipboard_get(char* buf, int max_len);

// --- Graphics ---
void sys_gfx_init();
void sys_gfx_text_mode();
void sys_gfx_rect(int x, int y, int w, int h, int color);
void sys_gfx_pixel(int x, int y, int color);
void sys_gfx_char(int x, int y, char c, int color);
void sys_gfx_string(int x, int y, const char* str, int color);
void sys_gfx_string_scaled(int x, int y, const char* str, int color, int scale);
void sys_gfx_set_target(uint32_t* buffer);
void sys_vsync();
void sys_gfx_draw_image(int x, int y, int w, int h, const uint32_t* data);
void sys_gfx_draw_image_scaled(int x, int y, int w, int h, const uint32_t* data, int src_w, int src_h);

// --- Dynamic Library API ---
void sys_cdl_init_system();
int sys_load_library(const char* path);
void* sys_get_proc_address(int lib_handle, const char* symbol_name);
void sys_unload_library(int lib_handle);
void sys_cdl_list_libraries();

// --- Networking ---
int sys_net_ping(const char* ip_str, char* out_buf, int max_len);

#endif
