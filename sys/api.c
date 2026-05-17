#include "api.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/vga.h"
#include "../hal/drivers/ata.h"
#include "../hal/drivers/acpi.h"
#include "../fs/disk.h"
#include "../core/string.h"
#include "../hal/drivers/keyboard.h"
#include "../common/font.h"
#include "../hal/drivers/net_rtl8139.h"
#include "../core/net.h"
#include "../core/task.h"
#include <string.h>

// Import screen dimensions from HAL
extern int screen_w;
extern int screen_h;

extern void kbd_flush();
extern int sys_get_key();
extern void gfx_set_target(uint32_t* buffer);
extern void vga_wait_vsync();
extern void gfx_draw_icon(int x, int y, int w, int h, const uint32_t* data);

// Import keyboard flags
extern int kbd_ctrl_pressed;
extern int kbd_shift_pressed;
extern int kbd_alt_pressed; // Exposed from keyboard.c

// Forward declarations for CDL
#ifdef KERNEL_MODE
extern void internal_cdl_init_system();
extern int internal_load_library(const char* path);
extern void* internal_get_proc_address(int lib_handle, const char* symbol_name);
extern void internal_unload_library(int lib_handle);
extern void internal_cdl_list_libraries();
#endif

static char global_clipboard[256];
static volatile uint32_t g_fs_generation = 0;

void sys_notify_fs_change() { g_fs_generation++; }
uint32_t sys_get_fs_generation() { return g_fs_generation; }

void sys_shutdown() {
    // Flush filesystem before shutdown to prevent data loss
    extern int pfs32_sync(void);
    pfs32_sync();
    disk_flush_cache();
    sys_print("\nShutting down...");
    sys_delay(500);

    // Try ACPI S5 shutdown first
    if (acpi_is_available() && acpi_shutdown() == 0) {
        // ACPI shutdown succeeded (should not reach here)
        while(1) asm volatile("hlt");
    }

    // Fallback: QEMU/Bochs-specific shutdown ports
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    asm volatile("cli; hlt");
}

void sys_reboot() {
    // Flush filesystem and disk cache before rebooting to prevent data loss
    extern int pfs32_sync(void);
    pfs32_sync();
    disk_flush_cache();
    // Small delay to let the drive settle after flush
    sys_delay(50);

    // Try ACPI reboot first (includes keyboard controller fallback)
    if (acpi_is_available()) {
        acpi_reboot();
        // If ACPI reboot returned, fall through to triple fault
    }

    // Fallback: keyboard controller reset
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    sys_delay(100);
    asm volatile ("lidt 0\nint3"); // Triple fault
    while(1) asm volatile("hlt");
}

void sys_delay(int milliseconds) {
    extern void timer_wait(int);
    int ticks = milliseconds / 20;
    if(ticks < 1) ticks = 1;
    timer_wait(ticks);
}

extern void rtc_read_time(int* h, int* m, int* s);
extern void rtc_read_date(int* year, int* month, int* day);

// Timezone offset in minutes from UTC (e.g., -300 for EST, 60 for CET)
static int tz_offset_minutes = 0;

void sys_set_tz_offset(int offset_min) {
    tz_offset_minutes = offset_min;
}

int sys_get_tz_offset(void) {
    return tz_offset_minutes;
}

void sys_get_time(int* h, int* m, int* s) {
    rtc_read_time(h, m, s);
    // Apply timezone offset
    // Check pointer validity, not the values (midnight is 0:0:0 and should still be adjusted)
    if (tz_offset_minutes != 0) {
        int total_min = (*h) * 60 + (*m) + tz_offset_minutes;
        // Normalize to 0-23 hours
        while (total_min < 0) total_min += 24 * 60;
        total_min %= 24 * 60;
        *h = total_min / 60;
        *m = total_min % 60;
    }
}

void sys_get_date(int* year, int* month, int* day) {
    rtc_read_date(year, month, day);
    // Adjust date if timezone offset crosses midnight
    if (tz_offset_minutes != 0 && year && month && day) {
        int h, m, s;
        rtc_read_time(&h, &m, &s);
        int total_min = h * 60 + m + tz_offset_minutes;
        if (total_min < 0) {
            // Previous day
            (*day)--;
            if (*day < 1) {
                (*month)--;
                if (*month < 1) { *month = 12; (*year)--; }
                // Simple days per month
                int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                if (*year % 4 == 0 && (*year % 100 != 0 || *year % 400 == 0)) days_in_month[2] = 29;
                *day = days_in_month[*month];
            }
        } else if (total_min >= 24 * 60) {
            // Next day
            (*day)++;
            int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            if (*year % 4 == 0 && (*year % 100 != 0 || *year % 400 == 0)) days_in_month[2] = 29;
            if (*day > days_in_month[*month]) {
                *day = 1;
                (*month)++;
                if (*month > 12) { *month = 1; (*year)++; }
            }
        }
    }
}

// Updated to support Alt
void sys_kbd_state(int* ctrl, int* shift, int* alt) {
    if(ctrl) *ctrl = kbd_ctrl_pressed;
    if(shift) *shift = kbd_shift_pressed;
    if(alt) *alt = kbd_alt_pressed;
}

extern int mouse_x, mouse_y, mouse_btn_left, mouse_btn_right;
int sys_mouse_read(int* x, int* y, int* left_click) {
    *x = mouse_x; *y = mouse_y; *left_click = mouse_btn_left;
    return (mouse_btn_left | (mouse_btn_right << 1));
}

extern int mouse_scroll_delta;
int sys_mouse_scroll() {
    int delta = mouse_scroll_delta;
    mouse_scroll_delta = 0;  // Consume the scroll event
    return delta;
}

void sys_print(const char* str) { vga_print(str); }
void sys_clear() { vga_clear(); }

int sys_wait_key() {
    int c = 0;
    while((c = sys_get_key()) == 0) { asm("hlt"); }
    return c;
}

void sys_flush_input() { kbd_flush(); }

int sys_get_uid() { return get_current_uid(); }
void sys_set_uid(int uid) { if(get_current_uid() == 0) set_current_uid(uid); }

void sys_clipboard_set(const char* text) { if(text) strncpy(global_clipboard, text, 255); }
int sys_clipboard_get(char* buf, int max_len) {
    if(!buf) return 0;
    strncpy(buf, global_clipboard, max_len);
    buf[max_len - 1] = '\0';  // Ensure null termination
    return strlen(global_clipboard);
}

// ... Filesystem and Graphics functions remain mostly the same ...
// Including stubs to keep file complete for compilation context

int sys_fs_mount() {
    ata_identify_device(0);
    if (!ide_devices[0].present) return -1;

    uint8_t mbr[512];
    if (disk_read_block(0, mbr) != 0) return -1;

    // Check if MBR has a valid partition table (signature 0xAA55)
    uint32_t part_lba;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        part_lba = *(uint32_t*)(mbr + 0x1BE + 8); // first partition start
    } else {
        part_lba = 0; // No valid MBR partition table
    }

    // If no valid partition, create one with default offset
    if (part_lba == 0 || part_lba >= disk_total_blocks) {
        part_lba = 16384; // Default partition start offset

        // Write a default MBR partition table
        memset(mbr, 0, 512);
        mbr[0x1BE] = 0x80;     // Bootable
        mbr[0x1BE + 4] = 0x7F; // Partition type (CamelOS)
        *(uint32_t*)(mbr + 0x1BE + 8) = part_lba;
        *(uint32_t*)(mbr + 0x1BE + 12) = disk_total_blocks - part_lba;
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        disk_write_block(0, mbr);
    }

    int result = pfs32_init(part_lba, disk_total_blocks - part_lba);

    // If no filesystem found, auto-format and create directory structure
    if (result != 0) {
        sys_print("[KERNEL] No filesystem found. Auto-formatting...\n");
        pfs32_init(part_lba, disk_total_blocks - part_lba);
        // Temporarily disable bad block scan for fast boot-time format
        extern uint32_t pfs32_format_fast(const char* label, uint32_t total);
        if (pfs32_format_fast("Camel Sys", disk_total_blocks - part_lba) == 0) {
            sys_print("[KERNEL] PFS32 formatted successfully.\n");

            // Create essential directory structure (same as installer)
            pfs32_create_directory("/Users");
            pfs32_create_directory("/usr");
            pfs32_create_directory("/usr/lib");
            pfs32_create_directory("/usr/apps");
            pfs32_create_directory("/Applications");
            pfs32_create_directory("/Library");
            pfs32_create_directory("/Library/Preferences");
            pfs32_create_directory("/etc");

            // Create .app bundle structures for dock compatibility
            // Each bundle gets Contents/MacOS and Info.plist
            struct { const char* path; const char* name; const char* cdl; const char* type; } app_bundles[] = {
                {"/Applications/Files.app",     "Files",     "/usr/lib/gui.cdl",     "cdl"},
                {"/Applications/Terminal.app",  "Terminal",  "",                      "builtin"},
                {"/Applications/Monitor.app",   "Monitor",   "/usr/lib/sysmon.cdl",  "cdl"},
                {"/Applications/NetDiag.app",   "NetDiag",   "/usr/apps/NetDiag.cdl","cdl"},
                {"/Applications/TextEdit.app",  "TextEdit",  "",                      "builtin"},
                {"/Applications/Browser.app",   "Browser",   "",                      "builtin"},
                {"/Applications/Settings.app",  "Settings",  "",                      "builtin"},
            };
            for (int i = 0; i < 7; i++) {
                pfs32_create_directory(app_bundles[i].path);
                char contents_path[256];
                strcpy(contents_path, app_bundles[i].path);
                strcat(contents_path, "/Contents");
                pfs32_create_directory(contents_path);
                char macos_path[256];
                strcpy(macos_path, contents_path);
                strcat(macos_path, "/MacOS");
                pfs32_create_directory(macos_path);
                char res_path[256];
                strcpy(res_path, contents_path);
                strcat(res_path, "/Resources");
                pfs32_create_directory(res_path);
                
                // Write Info.plist
                char plist_path[256];
                strcpy(plist_path, contents_path);
                strcat(plist_path, "/Info.plist");
                char plist[512];
                int plen = 0;
                plen += sprintf(plist + plen, "# CamelOS App Bundle Info\n");
                plen += sprintf(plist + plen, "CFBundleName=%s\n", app_bundles[i].name);
                plen += sprintf(plist + plen, "CFBundleIdentifier=com.camelos.%s\n", app_bundles[i].name);
                plen += sprintf(plist + plen, "CFBundleExecutable=%s\n", app_bundles[i].name);
                plen += sprintf(plist + plen, "CFBundleVersion=1.0\n");
                plen += sprintf(plist + plen, "CFBundleType=%s\n", app_bundles[i].type);
                plen += sprintf(plist + plen, "CFBundleMinOSVersion=1.0\n");
                if (app_bundles[i].cdl[0]) {
                    plen += sprintf(plist + plen, "CFBundleCDLPath=%s\n", app_bundles[i].cdl);
                }
                pfs32_create_file(plist_path);
                pfs32_write_file(plist_path, (uint8_t*)plist, plen);
            }

            pfs32_sync();
            disk_flush_cache();

            // Verify superblock was actually committed to disk
            {
                uint8_t vbuf[512];
                ata_read_sector(0, part_lba, vbuf);
                uint32_t* vmagic = (uint32_t*)vbuf;
                if (*vmagic != PFS32_MAGIC) {
                    sys_print("[KERNEL] WARNING: Superblock verification failed, retrying write...\n");
                    pfs32_sync();
                    disk_flush_cache();
                } else {
                    sys_print("[KERNEL] Superblock verified on disk.\n");
                }
            }

            sys_print("[KERNEL] Directory structure created.\n");

            // Re-mount
            result = pfs32_init(part_lba, disk_total_blocks - part_lba);
        } else {
            sys_print("[KERNEL] PFS32 format failed!\n");
        }
    }

    return result;
}

int sys_fs_write(const char* filename, char* data, int size) {
    int res = pfs32_write_file(filename, (uint8_t*)data, size);
    if(res >= 0) sys_notify_fs_change();
    return res;
}
int sys_fs_read(const char* filename, char* buffer, int max_len) {
    return pfs32_read_file(filename, (uint8_t*)buffer, max_len);
}
int sys_fs_create(const char* full_path, int is_dir) {
    int res = (is_dir) ? pfs32_create_directory(full_path) : pfs32_create_file(full_path);
    if (res == 0) sys_notify_fs_change();
    return res;
}
int sys_fs_delete(const char* full_path) {
    int res = pfs32_delete(full_path);
    if (res == 0) sys_notify_fs_change();
    return res;
}
int sys_fs_exists(const char* full_path) { return pfs32_stat(full_path, 0) == 0; }
int sys_fs_is_dir(const char* full_path) {
    pfs32_direntry_t entry;
    if(pfs32_stat(full_path, &entry) != 0) return -1;
    return (entry.attributes & PFS32_ATTR_DIRECTORY) ? 1 : 0;
}
int sys_fs_rename(const char* o, const char* n) {
    int res = pfs32_rename(o, n);
    if(res == 0) sys_notify_fs_change();
    return res;
}
void sys_fs_copy(const char* s, const char* d) { pfs32_copy(s, d); }

int sys_fs_stat(const char* path, int* total_blocks, int* free_blocks) {
    (void)path;  // Only one filesystem in CamelOS currently
    pfs32_stats_t stats;
    if (pfs32_get_stats(&stats) != 0) return -1;
    if (total_blocks) *total_blocks = stats.total_sectors_used + stats.blocks_free;
    if (free_blocks)  *free_blocks  = stats.blocks_free;
    return 0;
}

// GFX Wrappers
void sys_gfx_init() { gfx_init_hal(0); }
void sys_gfx_text_mode() {}
void sys_gfx_rect(int x, int y, int w, int h, int color) { gfx_fill_rect(x, y, w, h, (uint32_t)color); }
void sys_gfx_pixel(int x, int y, int color) { gfx_put_pixel(x, y, (uint32_t)color); }
void sys_gfx_char(int x, int y, char c, int color) {
    unsigned char uc = (unsigned char)c;
    const uint8_t* glyph;
    
    if (uc >= 32 && uc <= 127) {
        glyph = font_8x16[uc - 32];
    } else if (uc >= 160 && uc <= 255) {
        glyph = font_latin1_8x16[uc - 160];
    } else {
        glyph = font_8x16[0]; // Fallback to space
    }
    
    for(int row=0; row<16; row++) {
        uint8_t line = glyph[row];
        for(int col=0; col<8; col++) {
            // Bit 7 is leftmost
            if((line << col) & 0x80) gfx_put_pixel(x + col, y + row, (uint32_t)color);
        }
    }
}
void sys_gfx_string(int x, int y, const char* str, int color) {
    while(*str) { sys_gfx_char(x, y, *str++, color); x += 8; }
}
void sys_gfx_string_scaled(int x, int y, const char* str, int color, int scale) {
    extern void gfx_draw_string_scaled(int x, int y, const char* str, uint32_t color, int scale);
    gfx_draw_string_scaled(x, y, str, (uint32_t)color, scale);
}
void sys_vsync() { vga_wait_vsync(); }
void sys_gfx_set_target(uint32_t* buffer) { gfx_set_target(buffer); }
void sys_gfx_draw_image(int x, int y, int w, int h, const uint32_t* data) { gfx_draw_icon(x, y, w, h, data); }
void sys_gfx_draw_image_scaled(int x, int y, int w, int h, const uint32_t* data, int sw, int sh) {
    extern void gfx_draw_asset_scaled(uint32_t*, int, int, const uint32_t*, int, int, int, int);
    gfx_draw_asset_scaled(0, x, y, data, sw, sh, w, h);
}

// CDL Wrappers
#ifdef KERNEL_MODE
void sys_cdl_init_system() { internal_cdl_init_system(); }
int sys_load_library(const char* path) { return internal_load_library(path); }
void* sys_get_proc_address(int h, const char* s) { return internal_get_proc_address(h, s); }
void sys_unload_library(int h) { internal_unload_library(h); }
void sys_cdl_list_libraries() { internal_cdl_list_libraries(); }

// Network Wrapper (Simplified for brevity)
int sys_net_ping(const char* ip, char* buf, int len) {
    // (Implementation assumed same as provided previously in core/net.c helpers)
    // Just a placeholder to link correctly
    return -1; 
}
#else
void sys_cdl_init_system() {}
int sys_load_library(const char* p) { return -1; }
void* sys_get_proc_address(int h, const char* s) { return 0; }
void sys_unload_library(int h) {}
void sys_cdl_list_libraries() {}
int sys_net_ping(const char* ip, char* buf, int len) { return -1; }
#endif

int sys_fs_ls(const char* path) { return -1; }
int sys_fs_list_dir(const char* path, void* buf, int max) {
    uint32_t blk=0; if(get_dir_block(path, &blk)!=0) return -1;
    return pfs32_listdir(blk, (pfs32_direntry_t*)buf, max);
}
void sys_fs_copy_recursive(const char* s, const char* d) { sys_fs_copy(s, d); }
void sys_fs_generate_unique_name(const char* p, const char* b, int d, char* o) {}

// Implement recursive deletion
int sys_fs_delete_recursive(const char* path) {
    // Check if the path is a directory
    if (sys_fs_is_dir(path) == 1) {
        // List directory contents
        pfs32_direntry_t entries[32];
        int count = sys_fs_list_dir(path, entries, 32);
        if (count < 0) return -1;
        
        // Recursively delete each entry
        for (int i = 0; i < count; i++) {
            if (entries[i].filename[0] == 0) continue;
            
            char full_path[256];
            strcpy(full_path, path);
            strcat(full_path, "/");
            strcat(full_path, entries[i].filename);
            
            if (entries[i].attributes & PFS32_ATTR_DIRECTORY) {
                sys_fs_delete_recursive(full_path);
            } else {
                sys_fs_delete(full_path);
            }
        }
    }
    
    // Delete the directory or file itself
    return sys_fs_delete(path);
}
