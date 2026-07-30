/* installer/installer_main.c - Camel OS Installer (macOS X "Aqua" Edition)
 *
 * macOS X skin integrated:
 *   - aqua_desktop()      : grey recovery-style desktop behind every screen
 *   - aqua_window()       : floating window w/ title bar + etched bottom bar
 *   - aqua_traffic_lights : red/yellow/green controls (glyphs on hover)
 *   - aqua_button()       : glossy Aqua gradient pill
 *   - aqua_ghosted_logo() : giant faded brand glyph (Image 1 watermark)
 *   - aqua_install_badge(): glossy circular installer icon (Image 0)
 *   - aqua_util_row()     : Utilities list row (Image 2)
 *   - render_welcome()    : "Install Camel OS" intro window   (Image 1)
 *   - render_utilities()  : "Camel OS Utilities" list window  (Image 2)
 *   - render_pie_chart()  : donut chart w/ % labels + hover
 *   - extra buttons       : Erase Disk, Disk Utility toolbar, traffic lights
 */
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/ata.h"
#include "../hal/drivers/serial.h"
#include "../common/ports.h"
#include "../include/string.h"
#include "../fs/pfs32.h"
#include "../fs/disk.h"
#include "../core/memory.h"
#include "../core/theme.h"
#include "../hal/cpu/idt.h"
#include "../hal/drivers/mouse.h"
#include "../hal/drivers/vga.h"
#include "../kernel/assets.h"
#include "../usr/lib/ui_widgets.h"
#include "disk_tools.h"
#include "disk_health.h"
#include "sys_requirements.h"

/* Multiboot info (memory detection) */
typedef struct {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr, syms[4], mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr, config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info, vbe_mode;
    uint16_t vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint32_t framebuffer_addr, framebuffer_pitch, framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp, framebuffer_type, color_info[6];
} multiboot_info_t;

uint32_t total_memory_kb;

/* --- Payload Externs --- */
extern uint8_t system_bin_start[], system_bin_end[];
extern uint8_t mbr_bin_start[];
extern uint32_t _bss_end;
extern uint8_t app_math_start[], app_math_end[];
extern uint8_t app_usr32_start[], app_usr32_end[];
extern uint8_t app_syskernel_start[], app_syskernel_end[];
extern uint8_t app_proc_start[], app_proc_end[];
extern uint8_t app_timer_start[], app_timer_end[];
extern uint8_t app_gui_start[], app_gui_end[];
extern uint8_t app_sysmon_start[], app_sysmon_end[];
extern uint8_t app_jsengine_start[], app_jsengine_end[];
extern uint8_t app_netdiag_start[], app_netdiag_end[];

/* --- Design Configuration --- */
#define WIN_W (gfx_get_width())
#define WIN_H (gfx_get_height())
#define CX (WIN_W / 2)
#define CY (WIN_H / 2)

#define C_BG            (theme_get_current()->page_bg)
#define C_SIDEBAR       (theme_get_current()->sidebar_bg)
#define C_WHITE         (theme_get_current()->card_bg)
#define C_TEXT_DARK     (theme_get_current()->text_primary)
#define C_TEXT_MUTED    (theme_get_current()->text_secondary)
#define C_ACCENT        (theme_get_current()->accent_color)
#define C_ACCENT_HOVER  (theme_get_current()->accent_hover)
#define C_DANGER        (theme_get_current()->danger_color)
#define C_BORDER        (theme_get_current()->card_border)
#define C_MODAL_DIM     (theme_get_current()->modal_dim)
#define C_SHADOW        (theme_get_current()->shadow_strong)
#define C_SUCCESS       (theme_get_current()->success_color)
#define C_WARNING       (theme_get_current()->warning_color)

#define C_PART_FREE     0xFFE5E5EA
#define C_PART_CAMEL    0xFF007AFF
#define C_PART_OTHER    0xFF5856D6
#define C_PART_NTFS     0xFF5856D6
#define C_PART_FAT32    0xFF34C759
#define C_PART_EXT4     0xFFFF9500
#define C_PART_BOOT     0xFFFF9500
#define C_PART_SYS      0xFF34C759

/* --- MBR Structures --- */
typedef struct {
    uint8_t  status;
    uint8_t  chs_start[3];
    uint8_t  type;
    uint8_t  chs_end[3];
    uint32_t lba_start;
    uint32_t lba_length;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    uint8_t     bootstrap[446];
    mbr_entry_t partitions[4];
    uint16_t    signature;
} __attribute__((packed)) mbr_sector_t;

/* --- Installer State --- */
typedef enum {
    STATE_WELCOME,
    STATE_UTILITIES,   /* NEW: Mac OS X Utilities list (Image 2) */
    STATE_SYS_CHECK,
    STATE_DISK_UTIL,
    STATE_SELECT_DISK,
    STATE_INSTALLING,
    STATE_SUCCESS,
    STATE_FAILURE
} InstallerState;

InstallerState current_state = STATE_WELCOME;

int selected_drive_idx = -1;
int util_drive_idx = 0;
int util_part_idx = -1;

extern uint32_t get_tick_count(void);
uint32_t install_start_tick = 0;

int modal_active = 0;
int modal_just_opened = 0;
char modal_title[32];
char modal_msg[64];
char modal_action_label[16];
void (*modal_callback)(void) = 0;

static int open_menu_id = -2;

mbr_sector_t disk_mbr[2];
int disk_has_mbr[2];

int install_step = 0;
int install_sub_step = 0;
int install_file_idx = 0;
int install_pct = 0;
int install_target_pct = 0;
char install_status[64] = "";
uint32_t kernel_write_offset = 0;
int install_error = 0;
char install_error_msg[128] = "";
uint32_t last_animation_tick = 0;
int install_step_tick = 0;
int install_idle_ticks = 0;
int install_step_start_tick = 0;
int install_total_write_failures = 0;

extern int mouse_x, mouse_y, mouse_btn_left;
int mx = 512, my = 384;
int mb_left = 0, mb_prev = 0;
int mb_clicked = 0;

int logs_window_open = 0;
char install_log[2048] = "";
int log_line_count = 0;
int log_window_dragging = 0;
int log_window_drag_x = 0, log_window_drag_y = 0;
int log_window_x = 0;
int log_window_y = 0;

int disk_tools_window_open = 0;
int disk_tool_selected = 0;
int disk_tool_tab = 0;
DiskBenchmark g_benchmark;
BadSectorScan g_bad_scan;
DiskWipe g_wipe;
DiskClone g_clone;
SurfaceScan g_surface_scan;
FilesystemCheck g_fs_check;
DiskInfo g_disk_info;

int sys_check_done = 0;
int util_menu_selected = 0;   /* selected row in the Utilities window */

void install_tick(void);

int health_scanned[4] = {0, 0, 0, 0};

/* --- Cursor Bitmap --- */
static const uint8_t cursor_bmp[] = {
    1,0,0,0,0,0,0,0,0,0,0,0,
    1,1,0,0,0,0,0,0,0,0,0,0,
    1,2,1,0,0,0,0,0,0,0,0,0,
    1,2,2,1,0,0,0,0,0,0,0,0,
    1,2,2,2,1,0,0,0,0,0,0,0,
    1,2,2,2,2,1,0,0,0,0,0,0,
    1,2,2,2,2,2,1,0,0,0,0,0,
    1,2,2,2,2,2,2,1,0,0,0,0,
    1,2,2,2,2,2,2,2,1,0,0,0,
    1,2,2,2,2,2,2,2,2,1,0,0,
    1,2,2,2,2,2,1,1,1,1,1,0,
    1,2,2,2,2,2,1,0,0,0,0,0,
    1,2,1,1,2,2,1,0,0,0,0,0,
    1,1,0,0,1,2,2,1,0,0,0,0,
    1,0,0,0,1,2,2,1,0,0,0,0,
    0,0,0,0,0,1,2,2,1,0,0,0,
    0,0,0,0,0,1,2,2,1,0,0,0,
    0,0,0,0,0,0,1,1,0,0,0,0
};

/* =============================================================================
 * LOGGING
 * ============================================================================= */
void add_log(const char* msg) {
    if (!msg || !*msg) return;
    int cur  = (int)strlen(install_log);
    int need = (int)strlen(msg);
    int cap  = (int)sizeof(install_log);
    if (cur + need + 2 >= cap) {
        int keep = cap - need - 2;
        if (keep < 0)   keep = 0;
        if (keep > cur) keep = cur;
        int start = cur - keep;
        while (start < cur && install_log[start] != '\n') start++;
        if (start < cur) start++;
        int movelen = cur - start;
        for (int i = 0; i < movelen; i++) install_log[i] = install_log[start + i];
        install_log[movelen] = 0;
        cur = movelen;
    }
    if (cur + need + 1 < cap) {
        memcpy(install_log + cur, msg, need);
        install_log[cur + need]     = '\n';
        install_log[cur + need + 1] = 0;
    }
    log_line_count++;
}

/* =============================================================================
 * INPUT (PS/2 mouse, polling)
 * ============================================================================= */
#define PS2_MOUSE_PORT   0x60
#define PS2_STATUS_PORT  0x64

static uint8_t ps2_mouse_id = 0x00;

static void ps2_mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { while (timeout--) { if ((inb(0x64) & 1) == 1) return; } }
    else           { while (timeout--) { if ((inb(0x64) & 2) == 0) return; } }
}
static void ps2_mouse_write(uint8_t val) {
    ps2_mouse_wait(1); outb(0x64, 0xD4);
    ps2_mouse_wait(1); outb(0x60, val);
}
static uint8_t ps2_mouse_read(void) { ps2_mouse_wait(0); return inb(0x60); }

static void init_ps2_mouse(void) {
    ps2_mouse_id = 0x00;
    ps2_mouse_wait(1); outb(0x64, 0xA8);
    ps2_mouse_wait(1); outb(0x64, 0x20);
    ps2_mouse_wait(0); uint8_t status = inb(0x60);
    status |= 2; status &= ~0x20;
    ps2_mouse_wait(1); outb(0x64, 0x60);
    ps2_mouse_wait(1); outb(0x60, status);
    ps2_mouse_write(0xFF); ps2_mouse_read();
    for (volatile int d = 0; d < 10000; d++) {}
    for (volatile int d = 0; d < 5000; d++) {}
    while ((inb(0x64) & 0x21) == 0x21) inb(0x60);
    /* Intellimouse scroll-wheel negotiation */
    ps2_mouse_write(0xF3); ps2_mouse_read();
    ps2_mouse_write(200);  ps2_mouse_read();
    ps2_mouse_write(0xF3); ps2_mouse_read();
    ps2_mouse_write(100);  ps2_mouse_read();
    ps2_mouse_write(0xF3); ps2_mouse_read();
    ps2_mouse_write(80);   ps2_mouse_read();
    ps2_mouse_write(0xF2); ps2_mouse_read();
    for (volatile int d = 0; d < 5000; d++) {}
    uint8_t dev_id = ps2_mouse_read();
    if (dev_id == 0x03) ps2_mouse_id = 0x03;
    ps2_mouse_write(0xF3); ps2_mouse_read();
    ps2_mouse_write(100);  ps2_mouse_read();
    ps2_mouse_write(0xF4); ps2_mouse_read();
    for (volatile int d = 0; d < 10000; d++) {}
    while ((inb(0x64) & 0x21) == 0x21) inb(0x60);
}

void poll_input(void) {
    static uint8_t packet[4];
    static int cycle = 0;
    mb_prev = mb_left;
    mb_clicked = 0;
    while ((inb(PS2_STATUS_PORT) & 0x21) == 0x21) {
        uint8_t b = inb(PS2_MOUSE_PORT);
        if (cycle == 0 && !(b & 0x08)) continue;
        packet[cycle++] = b;
        uint8_t packet_len = (ps2_mouse_id == 0x03) ? 4 : 3;
        if (cycle >= packet_len) {
            cycle = 0;
            if (packet[0] & 0xC0) continue;
            int rel_x = packet[1]; if (packet[0] & 0x10) rel_x -= 256;
            int rel_y = packet[2]; if (packet[0] & 0x20) rel_y -= 256;
            mx += rel_x; my -= rel_y;
            int new_left = packet[0] & 1;
            if (new_left && !mb_left) mb_clicked = 1;
            mb_left = new_left;
            if (mx < 0) mx = 0; if (mx >= WIN_W) mx = WIN_W - 1;
            if (my < 0) my = 0; if (my >= WIN_H) my = WIN_H - 1;
        }
    }
}

void draw_cursor(void) {
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 12; x++) {
            uint8_t p = cursor_bmp[y * 12 + x];
            if (p == 1) gfx_put_pixel(mx + x, my + y, 0xFF000000);
            else if (p == 2) gfx_put_pixel(mx + x, my + y, 0xFFFFFFFF);
        }
}

/* =============================================================================
 * HELPERS
 * ============================================================================= */
static uint32_t lerp_color(uint32_t a, uint32_t b, int t) {
    if (t < 0) t = 0; if (t > 256) t = 256;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = (ar * (256 - t) + br * t) >> 8;
    int g = (ag * (256 - t) + bg * t) >> 8;
    int bl= (ab * (256 - t) + bb * t) >> 8;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}
static uint32_t lighten(uint32_t c, int a) {
    int r = ((c >> 16) & 0xFF) + a; if (r > 255) r = 255;
    int g = ((c >> 8)  & 0xFF) + a; if (g > 255) g = 255;
    int b = ( c        & 0xFF) + a; if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static uint32_t darken(uint32_t c, int a) {
    int r = ((c >> 16) & 0xFF) - a; if (r < 0) r = 0;
    int g = ((c >> 8)  & 0xFF) - a; if (g < 0) g = 0;
    int b = ( c        & 0xFF) - a; if (b < 0) b = 0;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static int is_light(uint32_t c) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 > 150;
}
static uint32_t blend_over(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 0)   return dst;
    if (a >= 255) return src | 0xFF000000u;
    int sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    int r = (sr * (int)a + dr * (int)(255 - a)) >> 8;
    int g = (sg * (int)a + dg * (int)(255 - a)) >> 8;
    int b = (sb * (int)a + db * (int)(255 - a)) >> 8;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static uint32_t isqrt_u(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

void format_disk_size(uint64_t sectors, char* out) {
    if (sectors == 0) { strcpy(out, "0 MB"); return; }
    uint64_t mb = sectors / 2048;
    if (mb >= 1024) {
        if (mb >= 1024ULL * 1024) {
            uint64_t tb = mb / 1024 / 1024;
            int dec = (int)((mb % (1024ULL * 1024)) * 10 / (1024ULL * 1024));
            char buf[16];
            int_to_str((int)tb, out); strcat(out, ".");
            int_to_str(dec, buf); strcat(out, buf); strcat(out, " TB");
        } else {
            int gb = (int)(mb / 1024);
            int dec = (int)((mb % 1024) * 10 / 1024);
            char buf[16];
            int_to_str(gb, out); strcat(out, ".");
            int_to_str(dec, buf); strcat(out, buf); strcat(out, " GB");
        }
    } else {
        int_to_str((int)mb, out); strcat(out, " MB");
    }
}

void get_part_type_name(uint8_t type, char* out) {
    switch (type) {
        case 0x00: strcpy(out, "Free"); break;
        case 0x07: strcpy(out, "NTFS"); break;
        case 0x0B: strcpy(out, "FAT32"); break;
        case 0x83: strcpy(out, "EXT4"); break;
        case 0x7F: strcpy(out, "PFS32"); break;
        case 0xFF: strcpy(out, "RAW"); break;
        default:   strcpy(out, "Unknown"); break;
    }
}
uint32_t get_part_color(uint8_t type) {
    switch (type) {
        case 0x7F: return C_PART_CAMEL;
        case 0x07: return C_PART_NTFS;
        case 0x0B: return C_PART_FAT32;
        case 0x83: return C_PART_EXT4;
        default:   return C_PART_OTHER;
    }
}

void render_breadcrumb(int y, const char** steps, int count, int current) {
    int step_w = 130, total_w = count * step_w, start_x = (WIN_W - total_w) / 2;
    for (int i = 0; i < count; i++) {
        int x = start_x + i * step_w;
        int done = (i < current), active = (i == current);
        uint32_t dot_bg  = done ? C_SUCCESS : (active ? C_ACCENT : C_BORDER);
        uint32_t label_c = done ? C_SUCCESS : (active ? C_ACCENT : C_TEXT_MUTED);
        int dcx = x + step_w / 2;
        gfx_fill_rounded_rect(dcx - 11, y, 22, 22, dot_bg, 11);
        if (done) gfx_draw_string(dcx - 4, y + 5, "v", 0xFFFFFFFF);
        else { char num[4]; int_to_str(i + 1, num); gfx_draw_string(dcx - 3, y + 5, num, 0xFFFFFFFF); }
        int lw = strlen(steps[i]) * 8;
        gfx_draw_string(dcx - lw / 2, y + 27, steps[i], label_c);
    }
    for (int i = 0; i < count - 1; i++) {
        int x1 = start_x + i * step_w + step_w / 2;
        int x2 = start_x + (i + 1) * step_w + step_w / 2;
        uint32_t line_c = (i + 1 <= current) ? C_ACCENT : C_BORDER;
        gfx_fill_rect(x1 + 11, y + 10, x2 - x1 - 22, 2, line_c);
    }
}

void render_health_badge(int x, int y, int score) {
    uint32_t color; const char* label;
    if (score < 0) {
        gfx_fill_rounded_rect(x, y, 64, 20, C_BORDER, 4);
        gfx_draw_string(x + 10, y + 4, "Unknown", C_TEXT_MUTED); return;
    }
    if (score >= 80)      { color = C_SUCCESS; label = "Good"; }
    else if (score >= 50) { color = C_WARNING; label = "Fair"; }
    else                  { color = C_DANGER;  label = "Poor"; }
    gfx_fill_rounded_rect(x, y, 64, 20, color, 4);
    char pill[16]; strcpy(pill, label); strcat(pill, " ");
    int_to_str(score, pill + strlen(pill)); strcat(pill, "%");
    gfx_draw_string(x + 5, y + 4, pill, 0xFFFFFFFF);
}

void render_progress_bar(int x, int y, int w, int h, int pct, uint32_t color) {
    gfx_fill_rounded_rect(x + 2, y + 2, w, h, C_SHADOW, h / 2);
    gfx_fill_rounded_rect(x, y, w, h, C_WHITE, h / 2);
    gfx_draw_rect(x, y, w, h, C_BORDER);
    if (pct > 0) {
        int fill = (w * pct) / 100;
        if (fill > 2) gfx_fill_rounded_rect(x + 2, y + 2, fill - 2, h - 4, color, h / 2);
    }
}

void render_disk_mini_map(int x, int y, int w, int h, int drive_idx) {
    gfx_fill_rounded_rect(x, y, w, h, C_PART_FREE, 4);
    gfx_draw_rect(x, y, w, h, C_BORDER);
    if (!disk_has_mbr[drive_idx] || !ide_devices[drive_idx].present) {
        gfx_draw_string(x + w/2 - 24, y + h/2 - 5, "Empty", C_TEXT_MUTED); return;
    }
    uint64_t total = ide_devices[drive_idx].sectors;
    int max_px = x + w - 2, px = x + 2;
    for (int k = 0; k < 4; k++) {
        mbr_entry_t* p = &disk_mbr[drive_idx].partitions[k];
        if (p->type == 0) continue;
        int pw = (int)((uint64_t)p->lba_length * (w - 4) / total);
        if (pw < 4) pw = 4;
        if (px + pw > max_px) pw = max_px - px;
        if (pw <= 0) break;
        gfx_fill_rounded_rect(px, y + 2, pw, h - 4, get_part_color(p->type), 3);
        px += pw;
    }
}

void render_section_label(int x, int y, int w, const char* label) {
    gfx_draw_string(x, y, label, C_TEXT_MUTED);
    gfx_draw_rect(x, y + 14, w, 1, C_BORDER);
}

/* --- Integer trig (milliturns: 0..1000 = 0..360deg, 0=right, clockwise) --- */
static const int16_t sin_table[64] = {
    0,  25,  50,  75, 100, 125, 150, 174, 198, 222, 245, 268, 290, 312, 334, 355,
    376, 396, 415, 434, 452, 469, 486, 502, 517, 531, 544, 557, 569, 580, 590, 599,
    608, 616, 623, 629, 634, 639, 642, 645, 647, 649, 650, 650, 649, 648, 646, 643,
    639, 634, 628, 622, 615, 608, 600, 591, 582, 572, 561, 550, 538, 526, 513, 500
};
static int isin(int mt) {
    mt = mt % 1000; if (mt < 0) mt += 1000;
    int idx = (mt * 256 + 500) / 1000;
    int quadrant = (idx >> 6) & 3, i = idx & 63, val;
    switch (quadrant) {
        case 0: val =  sin_table[i]; break;
        case 1: val =  sin_table[63 - i]; break;
        case 2: val = -sin_table[i]; break;
        case 3: val = -sin_table[63 - i]; break;
        default: val = 0; break;
    }
    return val;
}
static int icos(int mt) { return isin(mt + 250); }

static int i_atan2(int dy, int dx) {
    if (dx == 0 && dy == 0) return 0;
    int abs_dx = dx < 0 ? -dx : dx, abs_dy = dy < 0 ? -dy : dy, angle;
    if (abs_dx >= abs_dy) {
        if (abs_dx == 0) angle = 0;
        else { int ratio = (abs_dy * 1000) / abs_dx;
               angle = (ratio * 1000) / (1000 + 280 * ratio / 1000); }
    } else {
        int ratio = (abs_dx * 1000) / abs_dy;
        angle = 250 - (ratio * 1000) / (1000 + 280 * ratio / 1000);
    }
    if (dx >= 0 && dy <= 0)      { /* Q0 */ }
    else if (dx < 0 && dy <= 0)  angle = 500 - angle;
    else if (dx < 0 && dy > 0)   angle = 500 + angle;
    else                         angle = 1000 - angle;
    if (angle < 0) angle += 1000;
    if (angle >= 1000) angle -= 1000;
    return angle;
}

/* --- Donut/Pie Chart (UPGRADED: % labels, center total, hover highlight) --- */
typedef struct {
    int fraction_permil;   /* 0..1000 */
    uint32_t color;
    const char* label;
} pie_slice_t;

void render_pie_chart(int cx, int cy, int radius, pie_slice_t* slices,
                      int slice_count, const char* center_label) {
    if (!slices || slice_count <= 0 || radius <= 0) return;
    int inner_r = radius * 55 / 100;      /* donut hole */
    int R2 = radius * radius, I2 = inner_r * inner_r;

    /* shadow */
    gfx_fill_rounded_rect(cx - radius + 2, cy - radius + 3, radius * 2, radius * 2,
                          0x30000000, radius);

    /* hover detection */
    int hover = -1;
    int ddx = mx - cx, ddy = my - cy;
    int dist2 = ddx * ddx + ddy * ddy;
    if (!modal_active && dist2 <= R2 && dist2 >= I2) {
        int pa = i_atan2(-ddy, ddx);
        int ang = 0;
        for (int s = 0; s < slice_count; s++) {
            if (slices[s].fraction_permil <= 0) continue;
            int end = ang + slices[s].fraction_permil;
            int in = (end <= 1000) ? (pa >= ang && pa < end)
                                   : (pa >= ang || pa < (end - 1000));
            if (in) { hover = s; break; }
            ang = end;
        }
    }

    /* slices */
    int angle = 0;
    for (int s = 0; s < slice_count; s++) {
        if (slices[s].fraction_permil <= 0) continue;
        int end_angle = angle + slices[s].fraction_permil;
        uint32_t col = slices[s].color;
        if (s == hover) col = lighten(col, 28);
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 > R2 || d2 < I2) continue;
                int px_angle = (dx == 0 && dy == 0) ? 0 : i_atan2(-dy, dx);
                int in_slice = (end_angle <= 1000)
                    ? (px_angle >= angle && px_angle < end_angle)
                    : (px_angle >= angle || px_angle < (end_angle - 1000));
                if (in_slice) {
                    int px = cx + dx, py = cy + dy;
                    if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                        gfx_put_pixel(px, py, col);
                }
            }
        }
        /* separator */
        int sx = cx + (icos(angle) * radius + 500) / 1000;
        int sy = cy - (isin(angle) * radius + 500) / 1000;
        gfx_draw_line(cx, cy, sx, sy, 0xFFFFFFFF);
        angle = end_angle;
    }

    /* outer ring */
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++) {
            int d2 = dx * dx + dy * dy;
            int inner = (radius - 2) * (radius - 2);
            if (d2 >= inner && d2 <= R2) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                    gfx_put_pixel(px, py, 0xFFC6C6C8);
            }
        }

    /* % labels on slices (only if big enough) */
    angle = 0;
    for (int s = 0; s < slice_count; s++) {
        if (slices[s].fraction_permil <= 0) continue;
        int end_angle = angle + slices[s].fraction_permil;
        if (slices[s].fraction_permil >= 70) {
            int mid = angle + slices[s].fraction_permil / 2;
            int lr = (radius + inner_r) / 2;
            int lx = cx + (icos(mid) * lr + 500) / 1000;
            int ly = cy - (isin(mid) * lr + 500) / 1000;
            char pc[8]; int_to_str(slices[s].fraction_permil / 10, pc); strcat(pc, "%");
            int tw = strlen(pc) * 8;
            gfx_draw_string(lx - tw / 2, ly - 6, pc, 0xFFFFFFFF);
        }
        angle = end_angle;
    }

    /* donut hole + center label */
    for (int dy = -inner_r; dy <= inner_r; dy++)
        for (int dx = -inner_r; dx <= inner_r; dx++)
            if (dx*dx + dy*dy <= inner_r*inner_r) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                    gfx_put_pixel(px, py, 0xFFFFFFFF);
            }
    gfx_fill_rounded_rect(cx - inner_r, cy - inner_r, inner_r*2, inner_r*2, 0x00000000, inner_r);
    if (center_label && center_label[0]) {
        int tw = strlen(center_label) * 8;
        gfx_draw_string(cx - tw / 2, cy - 6, center_label, C_TEXT_DARK);
    }

    /* legend with percentages (2 columns) */
    int cols = (slice_count > 3) ? 2 : 1, col_w = 170;
    int legend_x = cx - (cols * col_w) / 2;
    int legend_y = cy + radius + 12;
    for (int s = 0; s < slice_count; s++) {
        if (!slices[s].label) continue;
        int c = s % cols, r = s / cols;
        int lx = legend_x + c * col_w, ly = legend_y + r * 20;
        gfx_fill_rounded_rect(lx, ly, 12, 12, slices[s].color, 3);
        gfx_draw_rect(lx, ly, 12, 12, 0xFFC6C6C8);
        char leg[64]; strcpy(leg, slices[s].label);
        strcat(leg, " "); int_to_str(slices[s].fraction_permil / 10, leg + strlen(leg));
        strcat(leg, "%");
        gfx_draw_string(lx + 16, ly, leg, (s == hover) ? C_ACCENT : C_TEXT_DARK);
    }

    /* hover tooltip */
    if (hover >= 0 && slices[hover].label) {
        char tip[64]; strcpy(tip, slices[hover].label);
        int tw = strlen(tip) * 8 + 16;
        int tx = mx + 14, ty = my + 14;
        if (tx + tw > WIN_W) tx = WIN_W - tw;
        gfx_fill_rounded_rect(tx, ty, tw, 20, 0xEE1C1C1E, 5);
        gfx_draw_string(tx + 8, ty + 4, tip, 0xFFFFFFFF);
    }
}

/* =============================================================================
 * AQUA SKIN (macOS X look) — no "mac_" prefix by request
 * ============================================================================= */
#define TL_RED   0xFFFF5F57u
#define TL_RED_B 0xFFE0443Eu
#define TL_YEL   0xFFFEBC2Eu
#define TL_YEL_B 0xFFDEA123u
#define TL_GRN   0xFF28C840u
#define TL_GRN_B 0xFF1AAB29u

/* Grey recovery-style desktop (Image 2 background) */
void aqua_desktop(void) {
    int top = 28; /* HEADER_HEIGHT */
    for (int y = top; y < WIN_H; y++) {
        int t = (y - top) * 256 / (WIN_H - top);
        gfx_fill_rect(0, y, WIN_W, 1, lerp_color(0xFF74747Au, 0xFF46464Bu, t));
    }
    uint32_t vig = 0x2A000000u;
    gfx_fill_rect(0, top, WIN_W/3, WIN_H-top, vig);
    gfx_fill_rect(WIN_W-WIN_W/3, top, WIN_W/3, WIN_H-top, vig);
    gfx_fill_rect(0, top, WIN_W, 40, vig);
    gfx_fill_rect(0, WIN_H-40, WIN_W, 40, vig);
}

/* Traffic lights; returns 1 if the RED light was clicked */
static int aqua_traffic_lights(int x, int y) {
    int cy = y + 14;
    int hov = (mx >= x && mx < x + 70 && my >= y && my < y + 28);
    struct { uint32_t c, b; int cx; } L[3] = {
        { TL_RED, TL_RED_B, x + 20 },
        { TL_YEL, TL_YEL_B, x + 40 },
        { TL_GRN, TL_GRN_B, x + 60 } };
    for (int i = 0; i < 3; i++) {
        gfx_fill_rounded_rect(L[i].cx - 6, cy - 6, 12, 12, L[i].b, 6);
        gfx_fill_rounded_rect(L[i].cx - 5, cy - 5, 10, 10, L[i].c, 5);
        gfx_fill_rounded_rect(L[i].cx - 4, cy - 5,  8,  4, 0x55FFFFFFu, 3);
        if (hov) {
            uint32_t g = darken(L[i].c, 90);
            if (i == 0) { gfx_draw_line(L[i].cx-3,cy-3,L[i].cx+3,cy+3,g);
                          gfx_draw_line(L[i].cx-3,cy+3,L[i].cx+3,cy-3,g); }
            if (i == 1)   gfx_fill_rect(L[i].cx-3, cy, 7, 1, g);
            if (i == 2) { gfx_fill_rect(L[i].cx-3, cy, 7, 1, g);
                          gfx_fill_rect(L[i].cx, cy-3, 1, 7, g); }
        }
    }
    return (mb_clicked && mx >= L[0].cx-6 && mx <= L[0].cx+6 && my >= cy-6 && my <= cy+6);
}

/* Floating window with title bar + optional etched bottom bar.
 * Returns content-area top Y. */
int aqua_window(int x, int y, int w, int h, const char* title,
                uint32_t body_col, int has_bar) {
    int bar_h = has_bar ? 52 : 0;
    gfx_fill_rounded_rect(x + 4, y + 6, w, h, 0x55000000u, 12);
    gfx_fill_rounded_rect(x, y, w, h, has_bar ? 0xFFDADADAu : body_col, 12);
    gfx_fill_rounded_rect(x, y, w, h - bar_h, body_col, 12);
    gfx_fill_rect(x, y + (h - bar_h) - 12, w, 12, body_col);
    for (int i = 0; i < 28; i++)
        gfx_fill_rect(x, y + i, w, 1, lerp_color(0xFFF7F7F7u, 0xFFDCDCDCu, i*256/27));
    gfx_fill_rect(x + 1, y + 14, w - 2, 14, 0xFFDCDCDCu);
    gfx_fill_rect(x, y + 28, w, 1, 0xFFB0B0B0u);
    gfx_fill_rect(x, y + 29, w, 1, 0xFFFFFFFFu);
    if (has_bar) {
        gfx_fill_rect(x, y + h - bar_h,     w, 1, 0xFFB0B0B0u);
        gfx_fill_rect(x, y + h - bar_h + 1, w, 1, 0xFFFFFFFFu);
    }
    aqua_traffic_lights(x, y);
    int tw = (int)strlen(title) * 8;
    gfx_draw_string(x + (w - tw)/2, y + 8, title, 0xFF4D4D4Du);
    gfx_stroke_rounded_rect(x, y, w, h, 0xFF9A9A9Cu, 12, 1);
    return y + 29;
}

/* Glossy Aqua gradient pill button */
int aqua_button(int x, int y, int w, int h, const char* label,
                int enabled, int is_default) {
    int hover = enabled && !modal_active && mx >= x && mx < x+w && my >= y && my < y+h;
    int press = hover && mb_left;
    int r = h/2;
    uint32_t top, bot, border, txt;
    if (!enabled)       { top = 0xFFF4F4F4u; bot = 0xFFE6E6E6u; border = 0xFFB6B6BAu; txt = 0xFF9A9A9Eu; }
    else if (is_default){ top = 0xFF7FB4E8u; bot = 0xFF2E6FE0u; border = 0xFF1C4FA8u; txt = 0xFFFFFFFFu; }
    else                { top = 0xFFFFFFFFu; bot = 0xFFE2E2E6u; border = 0xFF9A9A9Cu; txt = 0xFF1C1C1Eu; }
    if (hover && !press) { top = darken(top, 8); bot = darken(bot, 8); }
    int yy = y + (press ? 1 : 0);
    gfx_fill_rounded_rect(x, yy, w, h, border, r);
    gfx_fill_rounded_rect(x+1, yy+1, w-2, h-2, press ? top : bot, r-1);
    int gh = (h-2)/2;
    gfx_fill_rounded_rect(x+2, yy+2, w-4, gh, press ? 0x22FFFFFFu : 0x88FFFFFFu, gh-1);
    int lw = (int)strlen(label) * 8;
    gfx_draw_string(x + (w-lw)/2, yy + (h-12)/2, label, txt);
    return hover && mb_clicked;
}

/* Giant faded brand glyph behind the title (Image 1 watermark) — a smooth "C" */
void aqua_ghosted_logo(int cx, int cy, int size, uint32_t col) {
    int stroke = size / 5, rr = stroke / 2;
    for (int t = 125; t <= 875; t += 3) {
        int px = cx + (icos(t) * (size/2) + 500) / 1000;
        int py = cy - (isin(t) * (size/2) + 500) / 1000;
        gfx_fill_rounded_rect(px - rr, py - rr, rr*2, rr*2, col, rr);
    }
    int ex0 = cx + (icos(125)*(size/2)+500)/1000, ey0 = cy - (isin(125)*(size/2)+500)/1000;
    int ex1 = cx + (icos(875)*(size/2)+500)/1000, ey1 = cy - (isin(875)*(size/2)+500)/1000;
    gfx_fill_rounded_rect(ex0 - stroke, ey0 - rr, stroke + rr, rr*2, col, rr);
    gfx_fill_rounded_rect(ex1 - stroke, ey1 - rr, stroke + rr, rr*2, col, rr);
}

/* Glossy circular installer badge (Image 0 icon) */
void aqua_install_badge(int cx, int cy, int r) {
    gfx_fill_rounded_rect(cx-r+2, cy-r+3, r*2, r*2, 0x30000000u, r);
    gfx_fill_rounded_rect(cx-r,   cy-r,   r*2, r*2, 0xFFFFFFFFu, r);
    gfx_fill_rounded_rect(cx-r+3, cy-r+3, r*2-6, r-2, 0x66FFFFFFu, (r-2)/2);
    gfx_stroke_rounded_rect(cx-r, cy-r, r*2, r*2, 0xFFC6C6C8u, r, 1);
    uint32_t ar = 0xFF8E8E93u;
    gfx_fill_rect(cx, cy-r+7, 1, r-4, ar);
    gfx_draw_line(cx-5, cy-5, cx, cy, ar);
    gfx_draw_line(cx+5, cy-5, cx, cy, ar);
    gfx_draw_string_scaled(cx-8, cy+3, "C", C_ACCENT, 2);
}

/* One row of the Utilities list (Image 2) */
static void aqua_util_row(int x, int y, int w, int h, int sel, int hov,
                          uint32_t badge, const char* glyph,
                          const char* title, const char* desc) {
    if (sel)      gfx_fill_rounded_rect(x, y, w, h, 0xFF2E6FE0u, 6);
    else if (hov) gfx_fill_rounded_rect(x, y, w, h, 0xFFE8E8EDu, 6);
    uint32_t tc = sel ? 0xFFFFFFFFu : C_TEXT_DARK;
    uint32_t dc = sel ? 0xCCFFFFFFu : C_TEXT_MUTED;
    gfx_fill_rounded_rect(x+8, y+6, 32, 32, darken(badge, 30), 7);
    gfx_fill_rounded_rect(x+9, y+7, 30, 30, badge, 6);
    gfx_fill_rounded_rect(x+10,y+8, 28, 13, 0x55FFFFFFu, 5);
    int gw = (int)strlen(glyph) * 8;
    gfx_draw_string_scaled(x + 24 - gw/2, y + 13, glyph, 0xFFFFFFFFu, 2);
    gfx_draw_string(x+48, y+7,  title, tc);
    gfx_draw_string(x+48, y+25, desc,  dc);
}

/* =============================================================================
 * DISK OPERATIONS
 * ============================================================================= */
#define FAT32_BOOT_SIG  0xAA55
#define EXT4_MAGIC      0xEF53

int detect_filesystem(int drive, uint32_t lba_start, uint8_t* type_out) {
    uint8_t buf[512];
    ata_read_sector(drive, lba_start, buf);
    uint16_t* m16 = (uint16_t*)buf;
    uint32_t* m32 = (uint32_t*)buf;
    if (memcmp(buf + 3, "NTFS", 4) == 0) { *type_out = 0x07; return 1; }
    if (m16[255] == FAT32_BOOT_SIG) {
        if (*(uint32_t*)(buf + 0x16) != 0) { *type_out = 0x0B; return 1; }
    }
    if (lba_start + 2 < ide_devices[drive].sectors) {
        ata_read_sector(drive, lba_start + 2, buf);
        if (m16[0x19] == EXT4_MAGIC) { *type_out = 0x83; return 1; }
    }
    if (lba_start <= 16384 && lba_start + 16384 < ide_devices[drive].sectors) {
        ata_read_sector(drive, lba_start + 16384, buf);
        if (m32[0] == PFS32_MAGIC) { *type_out = 0x7F; return 1; }
    }
    *type_out = 0xFF;
    return 0;
}

void read_drive_mbr(int drive) {
    if (!ide_devices[drive].present) return;
    disk_set_drive(drive);
    ata_read_sector(drive, 0, (uint8_t*)&disk_mbr[drive]);
    disk_has_mbr[drive] = (disk_mbr[drive].signature == 0xAA55);
    if (!disk_has_mbr[drive]) memset(&disk_mbr[drive], 0, sizeof(mbr_sector_t));
}
void scan_hardware(void) {
    ata_identify_device(0); read_drive_mbr(0);
    ata_identify_device(1); read_drive_mbr(1);
}

void action_erase_disk(void) {
    int drv = util_drive_idx;
    uint8_t z[512]; memset(z, 0, 512);
    ata_write_sector(drv, 0, z);
    ata_write_sector(drv, 16384, z);
    scan_hardware();
    modal_active = 0;
}
void action_format_partition(uint8_t fs_type) {
    int drv = util_drive_idx;
    if (util_part_idx < 0) return;
    mbr_entry_t* part = &disk_mbr[drv].partitions[util_part_idx];
    if (part->type == 0) return;
    switch (fs_type) {
        case 0x7F:
            pfs32_init(part->lba_start, part->lba_length);
            { extern uint32_t pfs32_format_fast(const char*, uint32_t);
              pfs32_format_fast("Camel Partition", part->lba_length); }
            add_log("Partition formatted as PFS32");
            break;
        case 0x0B: {
            uint8_t b[512]; memset(b, 0, 512);
            b[0]=0xEB; b[1]=0x58; b[2]=0x90;
            memcpy(b+3, "FAT32   ", 8);
            *(uint16_t*)(b+11)=512; b[13]=8; *(uint16_t*)(b+14)=32; b[16]=2;
            *(uint32_t*)(b+32) = part->lba_length;
            *(uint32_t*)(b+36) = (part->lba_length / 1024);
            *(uint32_t*)(b+44)=2; *(uint16_t*)(b+510)=0xAA55;
            ata_write_sector(drv, part->lba_start, b);
            add_log("Partition formatted as FAT32");
            break;
        }
        case 0x07: {
            uint8_t b[512]; memset(b, 0, 512);
            b[0]=0xEB; b[1]=0x52; b[2]=0x90;
            memcpy(b+3, "NTFS    ", 8);
            *(uint16_t*)(b+11)=512; b[13]=8;
            *(uint64_t*)(b+40)=part->lba_length;
            *(uint64_t*)(b+48)=4;
            *(uint16_t*)(b+510)=0xAA55;
            ata_write_sector(drv, part->lba_start, b);
            add_log("Partition formatted as NTFS");
            break;
        }
        case 0x83: {
            uint8_t b[512]; memset(b, 0, 512);
            b[0x18]=0xEF; b[0x19]=0x53;
            ata_write_sector(drv, part->lba_start + 2, b);
            add_log("Partition formatted as EXT4");
            break;
        }
        default: {
            uint8_t z[512]; memset(z, 0, 512);
            for (uint32_t i=0; i<100; i++) ata_write_sector(drv, part->lba_start+i, z);
            add_log("Partition wiped (RAW)");
            break;
        }
    }
    part->type = fs_type;
    ata_write_sector(drv, 0, (uint8_t*)&disk_mbr[drv]);
    scan_hardware();
    modal_active = 0;
}
void action_create_schema(void) {
    int drv = util_drive_idx;
    disk_set_drive(drv);
    mbr_sector_t new_mbr; memset(&new_mbr, 0, sizeof(new_mbr));
    uint64_t total64 = ide_devices[drv].sectors;
    uint32_t total = (total64 > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)total64;
    uint32_t start = 16384, size = total - start;
    new_mbr.partitions[0].status = 0x80;
    new_mbr.partitions[0].type   = 0x7F;
    new_mbr.partitions[0].lba_start  = start;
    new_mbr.partitions[0].lba_length = size;
    new_mbr.signature = 0xAA55;
    ata_write_sector(drv, 0, (uint8_t*)&new_mbr);
    action_format_partition(0x7F);
    add_log("Disk initialized with MBR and PFS32");
    scan_hardware();
    modal_active = 0;
}
void action_delete_partition(void) {
    if (util_part_idx < 0) return;
    memset(&disk_mbr[util_drive_idx].partitions[util_part_idx], 0, sizeof(mbr_entry_t));
    ata_write_sector(util_drive_idx, 0, (uint8_t*)&disk_mbr[util_drive_idx]);
    scan_hardware();
    modal_active = 0;
}
void show_modal(const char* title, const char* msg, const char* btn, void (*cb)(void)) {
    strcpy(modal_title, title);
    strcpy(modal_msg, msg);
    strcpy(modal_action_label, btn);
    modal_callback = cb;
    modal_active = 1;
    modal_just_opened = 1;
}

/* =============================================================================
 * UI PRIMITIVES
 * ============================================================================= */
#define HEADER_HEIGHT 28

int measure_text_width(const char* s) { return strlen(s) * 8; }
void draw_centered_text(int y, const char* str, int scale, uint32_t color) {
    int w = strlen(str) * 8 * scale;
    gfx_draw_string_scaled((WIN_W - w) / 2, y, str, color, scale);
}
static void draw_pill(int x, int y, int w, int h, const char* label, uint32_t col, int enabled) {
    int r = h/2, br = r-1; if (br < 1) br = 1;
    uint32_t bot    = enabled ? darken(col, 14)  : 0xFFE2E2E6u;
    uint32_t border = enabled ? darken(col, 60)  : 0xFFB6B6BAu;
    uint32_t txt    = enabled ? (is_light(col) ? C_TEXT_DARK : 0xFFFFFFFFu) : 0xFF9A9A9Eu;
    gfx_fill_rounded_rect(x + 2, y + 3, w, h, C_SHADOW, r);
    gfx_fill_rounded_rect(x, y, w, h, border, r);
    gfx_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, bot, br);
    int gh = (h - 2) / 2, gr = br - 1; if (gr < 1) gr = 1;
    if (gh > 2) gfx_fill_rounded_rect(x + 2, y + 2, w - 4, gh, 0x55FFFFFFu, gr);
    int tlen = (int)strlen(label) * 8;
    gfx_draw_string(x + (w - tlen) / 2, y + (h - 16) / 2, label, txt);
}
int ui_button(int x, int y, int w, int h, const char* label, uint32_t color) {
    if (modal_active) return 0;
    int hover = (mx >= x && mx <= x + w && my >= y && my <= y + h);
    int pressed = (hover && mb_left);
    int r = h/2, br = r-1; if (br < 1) br = 1;
    uint32_t col = color;
    if (hover && !pressed) col = darken(color, 12);
    int yy = y + (pressed ? 1 : 0);
    uint32_t border = darken(color, 60);
    uint32_t bot    = darken(col, pressed ? 8 : 14);
    gfx_fill_rounded_rect(x + 2, yy + 3, w, h, C_SHADOW, r);
    gfx_fill_rounded_rect(x, yy, w, h, border, r);
    gfx_fill_rounded_rect(x + 1, yy + 1, w - 2, h - 2, bot, br);
    int gh = (h - 2) / 2, gr = br - 1; if (gr < 1) gr = 1;
    if (gh > 2) gfx_fill_rounded_rect(x + 2, yy + 2, w - 4, gh, 0x55FFFFFFu, gr);
    int tlen = (int)strlen(label) * 8;
    uint32_t tcol = is_light(color) ? C_TEXT_DARK : 0xFFFFFFFFu;
    gfx_draw_string(x + (w - tlen) / 2, yy + (h - 16) / 2, label, tcol);
    return (hover && mb_clicked);
}
int ui_icon_button(int x, int y, int w, int h, const char* label, uint32_t bg, uint32_t fg) {
    if (modal_active) return 0;
    int hover = (mx >= x && mx <= x+w && my >= y && my <= y+h);
    uint32_t draw_bg = hover ? C_ACCENT : bg;
    uint32_t draw_fg = hover ? 0xFFFFFFFF : fg;
    gfx_fill_rounded_rect(x, y, w, h, draw_bg, 6);
    gfx_draw_rect(x, y, w, h, hover ? C_ACCENT_HOVER : C_BORDER);
    gfx_draw_string(x + 8, y + (h-14)/2, label, draw_fg);
    return (hover && mb_clicked);
}

/* =============================================================================
 * MENU BAR
 * ============================================================================= */
static void mrow(int rx, int ry, int rw, const char* label) {
    int hov = (mx >= rx && mx < rx + rw && my >= ry && my < ry + 20);
    if (hov) {
        gfx_fill_rounded_rect(rx + 5, ry + 2, rw - 10, 16, 0xFF2D6FE0u, 4);
        gfx_draw_string(rx + 15, ry + 6, label, 0xFFFFFFFFu);
    } else {
        gfx_draw_string(rx + 15, ry + 6, label, 0xFF333333u);
    }
}
static int mtitle(int cur_x, const char* name, int id) {
    int w = measure_text_width(name) + 20, ph = 20, py0 = (HEADER_HEIGHT - ph) / 2;
    int hovered = (mx >= cur_x && mx < cur_x + w && my < HEADER_HEIGHT);
    int open = (open_menu_id == id);
    if (open) {
        gfx_fill_rounded_rect(cur_x, py0, w, ph, 0xFF2D6FE0u, 5);
        gfx_fill_rect(cur_x + 2, py0 + ph - 5, w - 4, 9, 0xFF2D6FE0u);
        gfx_draw_string(cur_x + 10, py0 + 6, name, 0xFFFFFFFFu);
    } else if (hovered) {
        gfx_fill_rounded_rect(cur_x, py0, w, ph, 0x30000000u, 5);
        gfx_draw_string(cur_x + 10, py0 + 6, name, C_TEXT_DARK);
    } else {
        gfx_draw_string(cur_x + 10, py0 + 6, name, 0xFF333333u);
    }
    return w;
}
void render_menu_bar(void) {
    for (int i = 0; i < HEADER_HEIGHT; i++)
        gfx_fill_rect(0, i, WIN_W, 1, lerp_color(0xFFF7F7F7u, 0xFFD6D6D6u, i * 256 / HEADER_HEIGHT));
    gfx_fill_rect(0, HEADER_HEIGHT,     WIN_W, 1, 0xFF8E8E90u);
    gfx_fill_rect(0, HEADER_HEIGHT + 1, WIN_W, 1, 0x28000000u);
    gfx_fill_rect(0, 0,                 WIN_W, 1, 0xFFFFFFFFu);
    gfx_fill_rounded_rect(6, 6, 15, 16, C_ACCENT, 4);
    gfx_fill_rounded_rect(6, 6, 15, 8,  0x55FFFFFFu, 3);
    gfx_draw_string(9, 8, "C", 0xFFFFFFFFu);
    int cur_x = 26, my2 = HEADER_HEIGHT, w;

    w = mtitle(cur_x, "Camel", -1);
    if (open_menu_id == -1) {
        gfx_fill_rounded_rect(cur_x + 3, my2 + 3, 160, 86, C_SHADOW, 8);
        gfx_fill_rounded_rect(cur_x, my2, 160, 86, 0xFF9A9A9Cu, 8);
        gfx_fill_rounded_rect(cur_x + 1, my2 + 1, 158, 84, 0xFFF4F4F4u, 7);
        gfx_fill_rect(cur_x + 6, my2 + 30, 148, 1, 0xFFD8D8D8u);
        mrow(cur_x, my2 + 6,  160, "About Camel OS");
        mrow(cur_x, my2 + 40, 160, "Restart");
        mrow(cur_x, my2 + 60, 160, "Shutdown");
    }
    cur_x += w;

    w = mtitle(cur_x, "View", 0);
    if (open_menu_id == 0) {
        gfx_fill_rounded_rect(cur_x + 3, my2 + 3, 180, 46, C_SHADOW, 8);
        gfx_fill_rounded_rect(cur_x, my2, 180, 46, 0xFF9A9A9Cu, 8);
        gfx_fill_rounded_rect(cur_x + 1, my2 + 1, 178, 44, 0xFFF4F4F4u, 7);
        mrow(cur_x, my2 + 3, 180, "Installer Logs");
    }
    cur_x += w;

    w = mtitle(cur_x, "Utilities", 1);   /* renamed from "Tools" */
    if (open_menu_id == 1) {
        gfx_fill_rounded_rect(cur_x + 3, my2 + 3, 200, 66, C_SHADOW, 8);
        gfx_fill_rounded_rect(cur_x, my2, 200, 66, 0xFF9A9A9Cu, 8);
        gfx_fill_rounded_rect(cur_x + 1, my2 + 1, 198, 64, 0xFFF4F4F4u, 7);
        gfx_fill_rect(cur_x + 6, my2 + 22, 188, 1, 0xFFD8D8D8u);
        mrow(cur_x, my2 + 3,  200, "System Check");
        mrow(cur_x, my2 + 23, 200, "Camel OS Utilities");
    }
    cur_x += w;

    w = mtitle(cur_x, "Help", 2);
    if (open_menu_id == 2) {
        gfx_fill_rounded_rect(cur_x + 3, my2 + 3, 180, 46, C_SHADOW, 8);
        gfx_fill_rounded_rect(cur_x, my2, 180, 46, 0xFF9A9A9Cu, 8);
        gfx_fill_rounded_rect(cur_x + 1, my2 + 1, 178, 44, 0xFFF4F4F4u, 7);
        gfx_fill_rect(cur_x + 6, my2 + 28, 168, 1, 0xFFD8D8D8u);
        mrow(cur_x, my2 + 10, 180, "Installation Guide");
        mrow(cur_x, my2 + 30, 180, "System Requirements");
    }
}
int process_menu_bar(int px, int py, int click) {
    int cur_x = 26, target_menu = -3;
    int w = measure_text_width("Camel") + 20;
    if (px >= cur_x && px < cur_x + w && py < HEADER_HEIGHT && click) target_menu = -1;
    if (open_menu_id == -1) {
        int my2 = HEADER_HEIGHT;
        if (click && px >= cur_x && px < cur_x + 160 && py >= my2) {
            int ry = py - my2;
            if (ry >= 40 && ry < 60) {
                disk_flush_cache();
                for (volatile int _i = 0; _i < 100000; _i++) {}
                outb(0x64, 0xFE);
            } else if (ry >= 60 && ry < 80) {
                disk_flush_cache();
                for (volatile int _i = 0; _i < 100000; _i++) {}
                outw(0x604, 0x2000); asm volatile("cli; hlt");
            }
            open_menu_id = -2;
        }
    }
    cur_x += w;

    w = measure_text_width("View") + 20;
    if (px >= cur_x && px < cur_x + w && py < HEADER_HEIGHT && click) target_menu = 0;
    if (open_menu_id == 0) {
        int my2 = HEADER_HEIGHT;
        if (click && px >= cur_x && px < cur_x + 180 && py >= my2 + 3 && py < my2 + 23) {
            logs_window_open = !logs_window_open; open_menu_id = -2;
        }
    }
    cur_x += w;

    w = measure_text_width("Utilities") + 20;
    if (px >= cur_x && px < cur_x + w && py < HEADER_HEIGHT && click) target_menu = 1;
    if (open_menu_id == 1) {
        int my2 = HEADER_HEIGHT;
        for (int i = 0; i < 2; i++) {
            int iy = my2 + 3 + i * 20;
            if (click && px >= cur_x && px < cur_x + 200 && py >= iy && py < iy + 20) {
                if (i == 0)      { sys_check_done = 0; current_state = STATE_SYS_CHECK; }
                else if (i == 1) { scan_hardware(); current_state = STATE_UTILITIES; }
                open_menu_id = -2;
            }
        }
    }
    cur_x += w;

    w = measure_text_width("Help") + 20;
    if (px >= cur_x && px < cur_x + w && py < HEADER_HEIGHT && click) target_menu = 2;
    if (click && target_menu != -3) {
        open_menu_id = (open_menu_id == target_menu) ? -2 : target_menu;
        return 1;
    }
    if (click && open_menu_id != -2 && !(px < cur_x && py < HEADER_HEIGHT)) open_menu_id = -2;
    return 0;
}

/* =============================================================================
 * LOGS WINDOW (now with traffic lights)
 * ============================================================================= */
void render_logs_window(void) {
    if (!logs_window_open) return;
    int win_w = 600, win_h = 300;
    if (log_window_x == 0 && log_window_y == 0) {
        log_window_x = (WIN_W - win_w) / 2;
        log_window_y = (WIN_H - win_h) / 2;
    }
    if (log_window_dragging) {
        log_window_x += mx - log_window_drag_x;
        log_window_y += my - log_window_drag_y;
        log_window_drag_x = mx; log_window_drag_y = my;
        if (log_window_x < 0) log_window_x = 0;
        if (log_window_y < 0) log_window_y = 0;
        if (log_window_x + win_w > WIN_W) log_window_x = WIN_W - win_w;
        if (log_window_y + win_h > WIN_H) log_window_y = WIN_H - win_h;
    }
    int wx = log_window_x, wy = log_window_y;
    gfx_fill_rounded_rect(wx + 2, wy + 2, win_w, win_h, C_SHADOW, 8);
    gfx_fill_rounded_rect(wx, wy, win_w, win_h, 0xFFFFFFFF, 8);
    gfx_draw_rect(wx, wy, win_w, win_h, C_BORDER);
    gfx_fill_rect(wx, wy, win_w, 30, C_SIDEBAR);
    gfx_draw_rect(wx, wy, win_w, 30, C_BORDER);
    /* traffic lights: red closes */
    if (aqua_traffic_lights(wx, wy + 1)) logs_window_open = 0;
    gfx_draw_string(wx + 80, wy + 8, "Installer Logs", C_TEXT_DARK);
    if (mx >= wx && mx < wx + win_w && my >= wy && my < wy + 30 && mb_clicked) {
        log_window_dragging = 1; log_window_drag_x = mx; log_window_drag_y = my;
    }
    if (!mb_left) log_window_dragging = 0;

    int lx = wx + 10, ly = wy + 40, lw = win_w - 20, lh = win_h - 60;
    gfx_fill_rect(lx, ly, lw, lh, C_BG);
    gfx_draw_rect(lx, ly, lw, lh, C_BORDER);
    if (current_state == STATE_INSTALLING)
        gfx_draw_string(lx + lw - 56, ly + 2, "[ live ]", C_SUCCESS);
    if (!install_log[0]) {
        gfx_draw_string(lx + 5, ly + 5, "(no log output yet)", C_TEXT_MUTED);
        return;
    }
    int total_lines = 0;
    { const char* q = install_log; while (*q) { if (*q == '\n') total_lines++; q++; }
      if (q > install_log && *(q - 1) != '\n') total_lines++; }
    int max_vis = (lh - 12) / 16; if (max_vis < 1) max_vis = 1;
    const char* startp = install_log;
    if (total_lines > max_vis) {
        int skip = total_lines - max_vis;
        while (skip > 0 && *startp) { if (*startp == '\n') skip--; startp++; }
    }
    int line_y = ly + 5;
    const char* ptr = startp;
    while (*ptr && line_y < ly + lh - 4) {
        const char* end = strchr(ptr, '\n');
        int len = end ? (int)(end - ptr) : (int)strlen(ptr);
        if (len > 0) {
            char line[128]; if (len > 127) len = 127;
            memcpy(line, ptr, len); line[len] = 0;
            gfx_draw_string(lx + 5, line_y, line, C_TEXT_DARK);
        }
        line_y += 16;
        if (!end) break;
        ptr = end + 1;
    }
}

/* =============================================================================
 * MODAL
 * ============================================================================= */
void render_modal(void) {
    if (!modal_active) return;
    int is_format = strcmp(modal_title, "Format Partition") == 0;
    int box_w = 420, box_h = is_format ? 360 : 220;
    gfx_fill_rect(0, 0, WIN_W, WIN_H, C_MODAL_DIM);
    int bx = (WIN_W - box_w)/2, by = (WIN_H - box_h)/2;
    gfx_fill_rounded_rect(bx+3, by+4, box_w, box_h, C_SHADOW, 14);
    gfx_fill_rounded_rect(bx, by, box_w, box_h, C_WHITE, 14);
    gfx_draw_rect(bx, by, box_w, box_h, C_BORDER);
    gfx_fill_rounded_rect(bx, by, box_w, 50, C_BG, 14);
    gfx_draw_rect(bx, by + 50, box_w, 1, C_BORDER);
    gfx_draw_string_scaled(bx + 20, by + 15, modal_title, C_TEXT_DARK, 2);
    gfx_draw_string(bx + 20, by + 68, modal_msg, C_TEXT_MUTED);
    if (modal_just_opened) { modal_just_opened = 0; return; }
    if (is_format) {
        struct { uint8_t type; const char* label; const char* desc; } opts[] = {
            {0x7F, "PFS32 (Camel OS Native)", "Recommended for Camel OS partitions"},
            {0x07, "NTFS",                   "For Windows compatibility"},
            {0x0B, "FAT32",                  "Universal compatibility"},
            {0x83, "EXT4",                   "Linux native filesystem"},
            {0xFF, "RAW (Unformatted)",       "Clear filesystem structures"},
        };
        int oy = by + 92;
        for (int i = 0; i < 5; i++) {
            int hover = (mx >= bx+16 && mx <= bx+box_w-16 && my >= oy && my <= oy+46);
            uint32_t bg = hover ? C_ACCENT : C_BG;
            uint32_t fg = hover ? C_WHITE : C_TEXT_DARK;
            uint32_t sg = hover ? 0xCCFFFFFF : C_TEXT_MUTED;
            gfx_fill_rounded_rect(bx+16, oy, box_w-32, 46, bg, 8);
            gfx_draw_rect(bx+16, oy, box_w-32, 46, hover ? C_ACCENT_HOVER : C_BORDER);
            gfx_draw_string(bx+28, oy+8, opts[i].label, fg);
            gfx_draw_string(bx+28, oy+26, opts[i].desc, sg);
            if (hover && mb_clicked) action_format_partition(opts[i].type);
            oy += 52;
        }
    }
    int btn_y = by + box_h - 58;
    int cancel_hov = (mx >= bx+16 && mx <= bx+126 && my >= btn_y && my <= btn_y+42);
    gfx_fill_rounded_rect(bx+16, btn_y, 110, 42, cancel_hov ? C_TEXT_DARK : C_SIDEBAR, 8);
    gfx_draw_rect(bx+16, btn_y, 110, 42, C_BORDER);
    gfx_draw_string(bx+44, btn_y+13, "Cancel", cancel_hov ? C_WHITE : C_TEXT_DARK);
    if (cancel_hov && mb_clicked) modal_active = 0;
    if (!is_format) {
        int act_hov = (mx >= bx+box_w-146 && mx <= bx+box_w-16 && my >= btn_y && my <= btn_y+42);
        uint32_t act_bg = act_hov ? C_ACCENT_HOVER : C_ACCENT;
        if (strcmp(modal_title, "Confirm Delete") == 0 || strcmp(modal_title, "Erase Entire Disk") == 0)
            act_bg = act_hov ? 0xFFCC0020 : C_DANGER;
        gfx_fill_rounded_rect(bx+box_w-146, btn_y, 130, 42, act_bg, 8);
        gfx_draw_string(bx+box_w-146+10, btn_y+13, modal_action_label, C_WHITE);
        if (act_hov && mb_clicked && modal_callback) modal_callback();
    }
}

/* Legacy panel background (kept for reference; screens now use aqua_desktop) */
static void render_panel_bg(int y0, int h, int wmx, int wmy, int wmR, int alpha) {
    int r = wmR * 62 / 100, R2 = wmR * wmR, r2 = r * r;
    for (int py = y0; py < y0 + h && py < WIN_H; py++) {
        int t = (h > 1) ? ((py - y0) * 256 / (h - 1)) : 0; if (t > 256) t = 256;
        uint32_t base = lerp_color(0xFFF6F6FAu, 0xFFE6E6EEu, t);
        gfx_fill_rect(0, py, WIN_W, 1, base);
        if (wmR <= 0 || alpha <= 0) continue;
        int dy = py - wmy; if (dy < -wmR || dy > wmR) continue;
        int d2 = R2 - dy * dy; if (d2 < 0) continue;
        int xo = (int)isqrt_u((uint32_t)d2);
        int xi = (r2 - dy * dy > 0) ? (int)isqrt_u((uint32_t)(r2 - dy * dy)) : 0;
        int ady = dy < 0 ? -dy : dy;
        int gap_limit = (ady * 100) / 119;
        int grey = (dy < 0) ? (0xCC - (ady * 0x14) / wmR) : (0xAE - (ady * 0x16) / wmR);
        if (grey < 0) grey = 0; if (grey > 255) grey = 255;
        uint32_t src = ((uint32_t)alpha << 24) | ((uint32_t)grey << 16)
                     | ((uint32_t)grey << 8) | (uint32_t)grey;
        uint32_t bl = blend_over(base, src);
        int ls = wmx - xo, le = wmx - (xi > 0 ? xi : 0);
        if (le >= ls) { int x0 = ls < 0 ? 0 : ls, x1 = le > WIN_W-1 ? WIN_W-1 : le;
            if (x1 >= x0) gfx_fill_rect(x0, py, x1 - x0 + 1, 1, bl); }
        int re = gap_limit < xo ? gap_limit : xo, rs = wmx + xi, rr = wmx + re;
        if (rr >= rs) { int x0 = rs < 0 ? 0 : rs, x1 = rr > WIN_W-1 ? WIN_W-1 : rr;
            if (x1 >= x0) gfx_fill_rect(x0, py, x1 - x0 + 1, 1, bl); }
    }
}

/* =============================================================================
 * SCREENS
 * ============================================================================= */

/* --- Welcome (Image 1: ghosted-logo intro window) --- */
void render_welcome(void) {
    aqua_desktop();
    int w = 640, h = 440, x = (WIN_W-w)/2, y = (WIN_H-h)/2;
    int ct = aqua_window(x, y, w, h, "Install Camel OS", 0xFFFFFFFFu, 1);
    int cw = w - 32, cx0 = x + 16;
    aqua_ghosted_logo(x + w/2, y + h/2 + 8, 240, 0xFFE4E4E8u);
    aqua_install_badge(x + w/2, ct + 58, 34);
    int hw = (int)strlen("Install Camel OS") * 16;
    gfx_draw_string_scaled(x + (w-hw)/2, ct + 108, "Install Camel OS", C_TEXT_DARK, 3);
    const char* sub = "To set up the installation of Camel OS, click Continue.";
    gfx_draw_string(x + (w - (int)strlen(sub)*8)/2, ct + 150, sub, C_TEXT_MUTED);
    const char* sub2 = "Version 1.0  |  The Operating System for Everyone";
    gfx_draw_string(x + (w - (int)strlen(sub2)*8)/2, ct + 170, sub2, C_TEXT_MUTED);
    /* bottom bar: extra "Disk Utility" (left) + default "Continue" (right) */
    int by = y + h - 39;
    if (aqua_button(cx0, by, 120, 26, "Disk Utility", 1, 0)) {
        scan_hardware(); current_state = STATE_UTILITIES;
    }
    int cwid = (int)strlen("Continue") * 8 + 40;
    if (aqua_button(x + w - 16 - cwid, by, cwid, 26, "Continue", 1, 1)) {
        sys_check_done = 0; current_state = STATE_SYS_CHECK;
    }
    gfx_draw_string(12, WIN_H - 14, "Camel OS Installer v1.0  |  camel-os.dev", 0xFFB0B0B4u);
}

/* --- Utilities (Image 2: Mac OS X Utilities list window) --- */
void render_utilities(void) {
    aqua_desktop();
    int w = 560, h = 420, x = (WIN_W-w)/2, y = (WIN_H-h)/2;
    int ct = aqua_window(x, y, w, h, "Camel OS Utilities", 0xFFF2F2F4u, 1);
    const char* ttl = "Camel OS Utilities";
    gfx_draw_string_scaled(x + (w - (int)strlen(ttl)*16)/2, ct + 12, ttl, C_TEXT_DARK, 3);
    int lx = x + 20, ly = ct + 52, lw = w - 40, rows = 6, rh = 44;
    int lh = rows * rh + 8;
    gfx_fill_rounded_rect(lx, ly, lw, lh, 0xFFFFFFFFu, 8);
    gfx_stroke_rounded_rect(lx, ly, lw, lh, 0xFFC6C6C8u, 8, 1);
    struct { uint32_t badge; const char* g; const char* t; const char* d; } R[6] = {
        { 0xFF2E6FE0u, "C", "Install Camel OS",  "Set up a new installation of Camel OS." },
        { 0xFF8E8E93u, "D", "Disk Utility",      "Partition, format and repair your disks." },
        { 0xFF34C759u, "B", "Disk Benchmark",    "Measure sequential & random performance." },
        { 0xFFFF9500u, "S", "Bad Sector Scan",   "Scan the surface for unreadable sectors." },
        { 0xFFFF3B30u, "W", "Disk Wipe",         "Securely erase a disk (DoD / Gutmann)." },
        { 0xFF5856D6u, "K", "Clone Disk",        "Copy one disk bit-for-bit to another." } };
    for (int i = 0; i < rows; i++) {
        int ry = ly + 4 + i * rh;
        int hov = !modal_active && mx >= lx && mx < lx+lw && my >= ry && my < ry+rh;
        if (hov && mb_clicked) util_menu_selected = i;
        if (i) gfx_fill_rect(lx+8, ry, lw-16, 1, 0xFFECECF0u);
        aqua_util_row(lx+4, ry, lw-8, rh, i == util_menu_selected, hov,
                      R[i].badge, R[i].g, R[i].t, R[i].d);
    }
    int by = y + h - 39, cwid = (int)strlen("Continue")*8 + 40;
    if (aqua_button(x + w - 16 - cwid, by, cwid, 26, "Continue", 1, 1)) {
        switch (util_menu_selected) {
            case 0: sys_check_done = 0; current_state = STATE_SYS_CHECK; break;
            case 1: scan_hardware();    current_state = STATE_DISK_UTIL;  break;
            case 2: disk_tool_selected = 1; disk_benchmark_init(&g_benchmark);
                    disk_tools_window_open = 1; break;
            case 3: disk_tool_selected = 2; bad_sector_scan_init(&g_bad_scan);
                    disk_tools_window_open = 1; break;
            case 4: disk_tool_selected = 3; disk_wipe_init(&g_wipe);
                    disk_tools_window_open = 1; break;
            case 5: disk_tool_selected = 4; disk_clone_init(&g_clone);
                    disk_tools_window_open = 1; break;
        }
    }
    if (aqua_traffic_lights(x, y)) current_state = STATE_WELCOME;
}

/* --- System Check --- */
void render_sys_check(void) {
    aqua_desktop();
    if (!sys_check_done) { sys_requirements_check(); sys_check_done = 1; }
    RequirementsCheck* req = sys_requirements_get();
    const char* steps[] = {"Welcome", "Check", "Select Disk", "Install"};
    render_breadcrumb(HEADER_HEIGHT + 10, steps, 4, 1);
    int content_y = HEADER_HEIGHT + 65;
    draw_centered_text(content_y, "System Requirements", 2, 0xFFFFFFFFu);
    draw_centered_text(content_y + 30, "Checking hardware compatibility with Camel OS", 1, 0xFFD0D0D4u);
    int table_x = CX - 310, table_y = content_y + 62, table_w = 620, row_h = 60;
    int table_h = req->requirement_count * row_h + 16;
    gfx_fill_rounded_rect(table_x+2, table_y+2, table_w, table_h, C_SHADOW, 12);
    gfx_fill_rounded_rect(table_x, table_y, table_w, table_h, C_WHITE, 12);
    gfx_draw_rect(table_x, table_y, table_w, table_h, C_BORDER);
    for (int i = 0; i < req->requirement_count; i++) {
        SystemRequirement* r = &req->requirements[i];
        int ry = table_y + 8 + i * row_h;
        if (i > 0) gfx_draw_rect(table_x + 10, ry - 4, table_w - 20, 1, 0xFFEEEEF0);
        uint32_t sc = sys_requirements_status_color(r->status);
        gfx_fill_rounded_rect(table_x + 14, ry + 12, 16, 16, sc, 8);
        gfx_draw_string(table_x + 18, ry + 14,
                        r->status == REQ_STATUS_PASS ? "v" :
                        r->status == REQ_STATUS_FAIL ? "x" : "!", 0xFFFFFFFF);
        gfx_draw_string(table_x + 40, ry + 6,  r->name,        C_TEXT_DARK);
        gfx_draw_string(table_x + 40, ry + 24, r->description, C_TEXT_MUTED);
        if (r->detected > 0) {
            char det[32]; int_to_str(r->detected, det); strcat(det, " MB");
            gfx_draw_string(table_x + 390, ry + 6,  det, C_TEXT_DARK);
            char req_str[32]; strcpy(req_str, "Min "); int_to_str(r->minimum, req_str+4);
            strcat(req_str, " MB");
            gfx_draw_string(table_x + 390, ry + 24, req_str, C_TEXT_MUTED);
        } else {
            gfx_draw_string(table_x + 390, ry + 15, r->status_text, sc);
        }
        if (r->recommended > 0 && r->detected > 0) {
            int bar_w = 160;
            int pct = (r->detected >= r->recommended) ? 100 : (r->detected * 100 / r->recommended);
            if (pct > 100) pct = 100;
            render_progress_bar(table_x + 560 - bar_w, ry + 15, bar_w, 12, pct, sc);
        }
    }
    int banner_y = table_y + table_h + 12;
    if (req->can_install) {
        gfx_fill_rounded_rect(table_x, banner_y, table_w, 44, 0xFFE8F8EE, 8);
        gfx_draw_rect(table_x, banner_y, table_w, 44, C_SUCCESS);
        gfx_draw_string(table_x + 20, banner_y + 14,
                        "v  Your system meets all requirements for Camel OS", 0xFF1E7B35);
    } else {
        gfx_fill_rounded_rect(table_x, banner_y, table_w, 44, 0xFFFFECEE, 8);
        gfx_draw_rect(table_x, banner_y, table_w, 44, C_DANGER);
        char warn_msg[96]; strcpy(warn_msg, "x  ");
        int_to_str(req->errors_count, warn_msg + 3);
        strcat(warn_msg, " requirement(s) not met. Installation may fail.");
        gfx_draw_string(table_x + 20, banner_y + 14, warn_msg, C_DANGER);
    }
    int nav_y = banner_y + 56;
    if (ui_button(table_x, nav_y, 130, 46, "< Back", C_WHITE)) current_state = STATE_WELCOME;
    if (req->can_install) {
        if (ui_button(table_x + table_w - 190, nav_y, 190, 46, "Continue >", C_ACCENT)) {
            scan_hardware(); current_state = STATE_SELECT_DISK;
        }
    } else {
        if (ui_button(table_x + table_w - 230, nav_y, 230, 46, "Try Anyway >", C_DANGER)) {
            scan_hardware(); current_state = STATE_SELECT_DISK;
        }
    }
}

/* --- Select Disk (extra "Erase Disk" button) --- */
void render_select_disk(void) {
    aqua_desktop();
    const char* steps[] = {"Welcome", "Check", "Select Disk", "Install"};
    render_breadcrumb(HEADER_HEIGHT + 8, steps, 4, 2);
    int content_y = HEADER_HEIGHT + 58;
    draw_centered_text(content_y,      "Install Camel OS", 3, 0xFFFFFFFFu);
    draw_centered_text(content_y + 40, "Choose the disk where you want to install Camel OS.", 1, 0xFFD0D0D4u);
    int present[2], np = 0;
    for (int i = 0; i < 2; i++) if (ide_devices[i].present) present[np++] = i;
    int panel_w = 560, panel_h = 172;
    int px = (WIN_W - panel_w) / 2, py = content_y + 70;
    gfx_fill_rounded_rect(px + 2, py + 3, panel_w, panel_h, C_SHADOW, 14);
    gfx_fill_rounded_rect(px, py, panel_w, panel_h, 0xFFFFFFFFu, 14);
    gfx_draw_rect(px, py, panel_w, panel_h, C_BORDER);
    const embedded_image_t* imgs; uint32_t ic; imgs = get_embedded_images(&ic);
    const embedded_image_t* hdd = 0;
    for (uint32_t i = 0; i < ic; i++) if (strcmp(imgs[i].name, "hdd_icon") == 0) { hdd = &imgs[i]; break; }
    int slot_w = np ? panel_w / np : panel_w;
    for (int s = 0; s < np; s++) {
        int drv = present[s], sx = px + s * slot_w, scx = sx + slot_w / 2;
        int sel = (selected_drive_idx == drv);
        if (s > 0) gfx_fill_rect(px + s * slot_w, py + 16, 1, panel_h - 32, C_BORDER);
        if (sel) {
            gfx_fill_rounded_rect(scx - 46, py + 12, 92, 124, 0x35FF3B30u, 14);
            gfx_fill_rounded_rect(scx - 42, py + 16, 84, 116, 0xFFE0392Bu, 12);
            gfx_fill_rounded_rect(scx - 39, py + 19, 78, 110, 0xFFF7F7FAu, 10);
        }
        if (hdd) gfx_draw_asset_scaled(0, scx - 32, py + 26, hdd->data, hdd->width, hdd->height, 64, 64);
        else     gfx_draw_string_scaled(scx - 16, py + 44, "HDD", C_TEXT_MUTED, 2);
        char nm[24]; strcpy(nm, drv == 0 ? "Primary" : "Secondary");
        gfx_draw_string(scx - strlen(nm) * 4, py + 100, nm, C_TEXT_DARK);
        char sz[24]; format_disk_size(ide_devices[drv].sectors, sz);
        gfx_draw_string(scx - strlen(sz) * 4, py + 118, sz, C_TEXT_MUTED);
        if (!modal_active && mb_clicked && mx >= sx && mx < sx + slot_w && my >= py && my < py + panel_h)
            selected_drive_idx = drv;
    }
    if (np == 0)
        gfx_draw_string_centered(CX, py + panel_h / 2 - 7, "No installation target detected", C_TEXT_MUTED, 1);
    char line[64];
    if (selected_drive_idx >= 0 && ide_devices[selected_drive_idx].present) {
        strcpy(line, "Camel OS will be installed on \x22");
        strcat(line, ide_devices[selected_drive_idx].model); strcat(line, "\x22");
    } else strcpy(line, "Select a disk above to continue.");
    gfx_draw_string(CX - strlen(line) * 4, py + panel_h + 20, line, 0xFFD0D0D4u);
    int nav_y = WIN_H - 70;
    if (ui_button(px, nav_y, 130, 44, "Disk Utility", C_WHITE)) { scan_hardware(); current_state = STATE_UTILITIES; }
    /* EXTRA: quick Erase Disk */
    if (selected_drive_idx >= 0 && ide_devices[selected_drive_idx].present) {
        util_drive_idx = selected_drive_idx;
        if (ui_button(px + 140, nav_y, 130, 44, "Erase Disk", C_DANGER))
            show_modal("Erase Entire Disk", "All data and partitions will be lost.", "Erase", action_erase_disk);
    }
    if (ui_button(CX - 60, nav_y, 120, 44, "Back", C_WHITE)) current_state = STATE_SYS_CHECK;
    int can = (selected_drive_idx >= 0 && ide_devices[selected_drive_idx].present
               && ide_devices[selected_drive_idx].sectors >= 204800ULL);
    if (can) {
        if (ui_button(CX + 70, nav_y, 130, 44, "Install", C_ACCENT)) {
            install_step = 0; install_sub_step = 0; install_file_idx = 0;
            install_error = 0; install_error_msg[0] = 0; kernel_write_offset = 0;
            install_pct = 0; install_target_pct = 0; install_step_tick = 0;
            install_idle_ticks = 0; install_step_start_tick = 0; install_total_write_failures = 0;
            install_start_tick = get_tick_count();
            current_state = STATE_INSTALLING;
            add_log("Starting installation process");
        }
    } else {
        draw_pill(CX + 70, nav_y, 130, 44, "Install", C_ACCENT, 0);
        if (selected_drive_idx >= 0 && ide_devices[selected_drive_idx].present)
            gfx_draw_string(CX + 70, nav_y - 16, "Drive too small (< 100 MB)", 0xFFFF8080u);
    }
}

/* --- Disk Utility (extra toolbar) --- */
void render_disk_utility(void) {
    aqua_desktop();
    /* Sidebar (Image 0 style) */
    gfx_fill_rect(0, HEADER_HEIGHT, 250, WIN_H - HEADER_HEIGHT, C_SIDEBAR);
    gfx_draw_rect(0, HEADER_HEIGHT, 250, WIN_H - HEADER_HEIGHT, C_BORDER);
    gfx_draw_string(20, HEADER_HEIGHT + 14, "DRIVES", C_TEXT_MUTED);
    int sy = HEADER_HEIGHT + 40;
    for (int i = 0; i < 2; i++) {
        int active = (util_drive_idx == i);
        if (active) gfx_fill_rounded_rect(8, sy, 234, 58, C_ACCENT, 8);
        else { gfx_fill_rounded_rect(8, sy, 234, 58, C_WHITE, 8);
               gfx_draw_rect(8, sy, 234, 58, C_BORDER); }
        uint32_t fg = active ? C_WHITE : C_TEXT_DARK;
        uint32_t muted = active ? 0xCCFFFFFF : C_TEXT_MUTED;
        if (ide_devices[i].present) {
            char sz[32]; format_disk_size(ide_devices[i].sectors, sz);
            char label[32]; strcpy(label, i==0 ? "Disk 0" : "Disk 1");
            gfx_draw_string(24, sy + 10, label, fg);
            gfx_draw_string(24, sy + 30, sz, muted);
            if (!health_scanned[i]) { disk_health_scan(i); health_scanned[i] = 1; }
            int score = disk_health_get_score(i);
            uint32_t hc = (score >= 80) ? C_SUCCESS : (score >= 50) ? C_WARNING : C_DANGER;
            if (active) hc = 0xCCFFFFFF;
            gfx_fill_rounded_rect(204, sy + 19, 10, 10, hc, 5);
        } else {
            gfx_draw_string(24, sy + 20, "Empty Bay", muted);
        }
        if (!modal_active && mx >= 8 && mx < 242 && my >= sy && my < sy+58 && mb_clicked && ide_devices[i].present) {
            util_drive_idx = i; util_part_idx = -1;
        }
        sy += 68;
    }
    if (ui_button(12, WIN_H - 68, 226, 44, "< Back to Utilities", C_WHITE)) current_state = STATE_UTILITIES;

    int cx = 270, content_w = WIN_W - cx - 10;
    /* EXTRA toolbar (Image 0 style) */
    int tb_y = HEADER_HEIGHT + 8;
    gfx_fill_rounded_rect(cx, tb_y, content_w - 4, 34, 0xFFFFFFFFu, 8);
    gfx_draw_rect(cx, tb_y, content_w - 4, 34, C_BORDER);
    if (ui_icon_button(cx + 8, tb_y + 5, 70, 24, "Refresh", C_BG, C_TEXT_DARK)) {
        scan_hardware(); health_scanned[util_drive_idx] = 0;
    }
    if (ui_icon_button(cx + 84, tb_y + 5, 60, 24, "Info", C_BG, C_TEXT_DARK)) {
        disk_get_info(util_drive_idx, &g_disk_info);
        disk_tool_selected = 0; disk_tools_window_open = 1;
    }
    if (ide_devices[util_drive_idx].present &&
        ui_icon_button(cx + 150, tb_y + 5, 70, 24, "Erase", 0xFFFFECEE, C_DANGER))
        show_modal("Erase Entire Disk", "All data and partitions will be lost.", "Erase", action_erase_disk);

    if (!ide_devices[util_drive_idx].present) {
        gfx_draw_string(cx, HEADER_HEIGHT + 80, "No drive present in selected bay.", C_TEXT_MUTED);
        return;
    }
    ide_device_t* dev = &ide_devices[util_drive_idx];
    char sz[32]; format_disk_size(dev->sectors, sz);

    int info_y = HEADER_HEIGHT + 52;
    gfx_fill_rounded_rect(cx+2, info_y+2, content_w-4, 90, C_SHADOW, 10);
    gfx_fill_rounded_rect(cx, info_y, content_w-4, 90, C_WHITE, 10);
    gfx_draw_rect(cx, info_y, content_w-4, 90, C_BORDER);
    gfx_fill_rounded_rect(cx+14, info_y+14, 56, 56, C_BG, 8);
    gfx_draw_string(cx + 22, info_y + 32, "HDD", C_TEXT_MUTED);
    gfx_draw_string(cx + 80, info_y + 14, "Model:", C_TEXT_MUTED);
    gfx_draw_string(cx + 132, info_y + 14, dev->model, C_TEXT_DARK);
    gfx_draw_string(cx + 80, info_y + 34, "Capacity:", C_TEXT_MUTED);
    gfx_draw_string(cx + 148, info_y + 34, sz, C_TEXT_DARK);
    gfx_draw_string(cx + 80, info_y + 54, "Scheme:", C_TEXT_MUTED);
    gfx_draw_string(cx + 148, info_y + 54, disk_has_mbr[util_drive_idx] ? "MBR" : "Uninitialized", C_ACCENT);
    int hs = disk_health_get_score(util_drive_idx);
    render_health_badge(cx + content_w - 90, info_y + 14, hs);

    int bar_y = info_y + 106;
    render_section_label(cx, bar_y, content_w - 4, "PARTITION MAP");
    bar_y += 20;
    int bar_w = content_w - 4, bar_h = 54;
    gfx_fill_rounded_rect(cx+2, bar_y+2, bar_w, bar_h, C_SHADOW, 8);
    gfx_fill_rounded_rect(cx, bar_y, bar_w, bar_h, C_WHITE, 8);
    gfx_draw_rect(cx, bar_y, bar_w, bar_h, C_BORDER);
    if (disk_has_mbr[util_drive_idx]) {
        uint64_t total = dev->sectors;
        int max_px = cx + bar_w - 4, ppx = cx + 4;
        for (int k = 0; k < 4; k++) {
            mbr_entry_t* part = &disk_mbr[util_drive_idx].partitions[k];
            if (part->type == 0) continue;
            int pw = (int)((uint64_t)part->lba_length * (bar_w-8) / total);
            if (pw < 8) pw = 8;
            if (ppx + pw > max_px) pw = max_px - ppx;
            if (pw <= 0) break;
            uint32_t col = get_part_color(part->type);
            if (util_part_idx == k) col = C_ACCENT_HOVER;
            gfx_fill_rounded_rect(ppx, bar_y+4, pw, bar_h-8, col, 5);
            if (pw > 40) { char tn[16]; get_part_type_name(part->type, tn);
                gfx_draw_string(ppx + 4, bar_y + bar_h/2 - 7, tn, 0xFFFFFFFF); }
            if (!modal_active && mx >= ppx && mx < ppx+pw && my >= bar_y && my <= bar_y+bar_h && mb_clicked)
                util_part_idx = k;
            ppx += pw;
        }
        if (ppx < cx + bar_w - 4) {
            gfx_fill_rounded_rect(ppx, bar_y+4, (cx+bar_w-4)-ppx, bar_h-8, C_PART_FREE, 5);
            gfx_draw_string(ppx + 4, bar_y + bar_h/2 - 7, "Free", C_TEXT_MUTED);
        }
    } else {
        gfx_fill_rounded_rect(cx+4, bar_y+4, bar_w-8, bar_h-8, C_PART_FREE, 5);
        gfx_draw_string_centered(cx + bar_w/2, bar_y + bar_h/2 - 7, "Unallocated", C_TEXT_MUTED, 1);
    }

    int ctrl_y = bar_y + bar_h + 14;
    if (disk_has_mbr[util_drive_idx]) {
        if (util_part_idx >= 0) {
            mbr_entry_t* p = &disk_mbr[util_drive_idx].partitions[util_part_idx];
            gfx_fill_rounded_rect(cx, ctrl_y, 320, 70, C_WHITE, 8);
            gfx_draw_rect(cx, ctrl_y, 320, 70, C_BORDER);
            char lbl[48]; strcpy(lbl, "Partition "); char num[4];
            int_to_str(util_part_idx + 1, num); strcat(lbl, num);
            gfx_draw_string(cx + 12, ctrl_y + 10, lbl, C_TEXT_DARK);
            char tn[16]; get_part_type_name(p->type, tn);
            gfx_draw_string(cx + 12, ctrl_y + 30, "Type:", C_TEXT_MUTED);
            gfx_draw_string(cx + 60, ctrl_y + 30, tn, C_TEXT_DARK);
            char ps[32]; format_disk_size(p->lba_length, ps);
            gfx_draw_string(cx + 12, ctrl_y + 48, "Size:", C_TEXT_MUTED);
            gfx_draw_string(cx + 60, ctrl_y + 48, ps, C_TEXT_DARK);
            if (ui_button(cx + 330, ctrl_y + 8, 110, 38, "Format", C_ACCENT)) {
                strcpy(modal_title, "Format Partition"); strcpy(modal_msg, "Select filesystem type:");
                modal_active = 1; modal_just_opened = 1; modal_callback = 0;
            }
            if (ui_button(cx + 450, ctrl_y + 8, 110, 38, "Delete", C_DANGER))
                show_modal("Confirm Delete", "This will permanently erase the partition.", "Delete", action_delete_partition);
        } else {
            gfx_draw_string(cx, ctrl_y + 14, "Click a partition in the map to select it", C_TEXT_MUTED);
        }
        if (ui_button(cx + content_w - 148, ctrl_y + 8, 140, 38, "Wipe Disk", C_DANGER))
            show_modal("Erase Entire Disk", "All data and partitions will be lost.", "Erase", action_erase_disk);
    } else {
        gfx_draw_string(cx, ctrl_y + 8, "This disk has no partition table.", C_TEXT_MUTED);
        if (ui_button(cx, ctrl_y + 30, 200, 42, "Initialize Disk (MBR)", C_ACCENT)) {
            action_create_schema();
            add_log("Disk initialized with MBR + PFS32");
        }
    }

    if (disk_has_mbr[util_drive_idx]) {
        uint64_t used = 0;
        for (int k=0; k<4; k++) if (disk_mbr[util_drive_idx].partitions[k].type!=0)
            used += disk_mbr[util_drive_idx].partitions[k].lba_length;
        char ustr[32], fstr[32];
        format_disk_size(used, ustr); format_disk_size(dev->sectors - used, fstr);
        int st_y = ctrl_y + 88;
        gfx_fill_rounded_rect(cx, st_y, content_w-4, 28, C_BG, 6);
        gfx_draw_rect(cx, st_y, content_w-4, 28, C_BORDER);
        gfx_draw_string(cx + 12, st_y + 8, "Used:", C_TEXT_MUTED);
        gfx_draw_string(cx + 56, st_y + 8, ustr, C_TEXT_DARK);
        gfx_draw_string(cx + 140, st_y + 8, "Free:", C_TEXT_MUTED);
        gfx_draw_string(cx + 180, st_y + 8, fstr, C_TEXT_DARK);

        pie_slice_t pie_slices[6]; int pie_count = 0;
        uint64_t total_sectors = dev->sectors;
        for (int k = 0; k < 4; k++) {
            mbr_entry_t* part = &disk_mbr[util_drive_idx].partitions[k];
            if (part->type == 0) continue;
            int frac = (total_sectors > 0) ? (int)((uint64_t)part->lba_length * 1000 / total_sectors) : 0;
            char tn[16]; get_part_type_name(part->type, tn);
            static char part_labels[4][48];
            char ps[32]; format_disk_size(part->lba_length, ps);
            strcpy(part_labels[k], tn); strcat(part_labels[k], " "); strcat(part_labels[k], ps);
            pie_slices[pie_count].fraction_permil = frac;
            pie_slices[pie_count].color = get_part_color(part->type);
            pie_slices[pie_count].label = part_labels[k];
            pie_count++;
        }
        if (used < dev->sectors) {
            int free_frac = (total_sectors > 0) ? (int)((uint64_t)(dev->sectors - used) * 1000 / total_sectors) : 0;
            pie_slices[pie_count].fraction_permil = free_frac;
            pie_slices[pie_count].color = C_PART_FREE;
            pie_slices[pie_count].label = "Free Space";
            pie_count++;
        }
        if (pie_count > 0) {
            int pie_r = 50, pie_cx = cx + 90;
            int pie_cy = st_y + 36 + 22 + 8 + pie_r;
            render_section_label(cx, st_y + 36, content_w - 4, "DISK USAGE");
            char total_lbl[32]; format_disk_size(dev->sectors, total_lbl);
            render_pie_chart(pie_cx, pie_cy, pie_r, pie_slices, pie_count, total_lbl);
        }
    }

    int tools_y = (disk_has_mbr[util_drive_idx]) ? ctrl_y + 330 : ctrl_y + 88;
    render_section_label(cx, tools_y, content_w - 4, "ADVANCED TOOLS");
    tools_y += 22;
    struct { const char* label; int id; uint32_t color; } tools[] = {
        {"Benchmark", 1, C_ACCENT}, {"Bad Sectors", 2, C_ACCENT}, {"Surface Scan", 5, C_ACCENT},
        {"Wipe Tool", 3, C_DANGER}, {"Clone Disk", 4, C_ACCENT},  {"FS Check", 6, C_ACCENT},
    };
    int btn_x = cx;
    for (int i = 0; i < 6; i++) {
        if (i == 3) { btn_x = cx; tools_y += 46; }
        if (ui_button(btn_x, tools_y, 118, 38, tools[i].label, tools[i].color)) {
            disk_tool_selected = tools[i].id;
            disk_tools_window_open = 1;
            if (tools[i].id == 1) disk_benchmark_init(&g_benchmark);
            else if (tools[i].id == 2) bad_sector_scan_init(&g_bad_scan);
            else if (tools[i].id == 3) disk_wipe_init(&g_wipe);
            else if (tools[i].id == 4) disk_clone_init(&g_clone);
            else if (tools[i].id == 5) surface_scan_init(&g_surface_scan);
            else if (tools[i].id == 6) fs_check_init(&g_fs_check);
            add_log("Opened disk tool window");
        }
        btn_x += 126;
    }
}

/* --- Disk Tools Window (traffic lights) --- */
void render_disk_tools_window(void) {
    if (!disk_tools_window_open) return;
    int win_w = 680, win_h = 460;
    int win_x = (WIN_W - win_w) / 2, win_y = (WIN_H - win_h) / 2;
    gfx_fill_rounded_rect(win_x+3, win_y+4, win_w, win_h, C_SHADOW, 14);
    gfx_fill_rounded_rect(win_x, win_y, win_w, win_h, C_WHITE, 14);
    gfx_draw_rect(win_x, win_y, win_w, win_h, C_BORDER);
    gfx_fill_rect(win_x, win_y, win_w, 38, C_SIDEBAR);
    gfx_draw_rect(win_x, win_y + 38, win_w, 1, C_BORDER);
    const char* titles[] = {"Disk Information", "Disk Benchmark", "Bad Sector Scan", "Disk Wipe",
                            "Disk Clone", "Surface Scan", "Filesystem Check"};
    if (disk_tool_selected >= 0 && disk_tool_selected <= 6)
        gfx_draw_string(win_x + 80, win_y + 11, titles[disk_tool_selected], C_TEXT_DARK);
    if (aqua_traffic_lights(win_x, win_y + 5)) { disk_tools_window_open = 0; disk_tool_selected = 0; }

    int cx = win_x + 16, cy = win_y + 52;
    switch (disk_tool_selected) {
        case 0:
            disk_get_info(util_drive_idx, &g_disk_info);
            disk_info_render(cx, cy, &g_disk_info);
            break;
        case 1:
            disk_benchmark_render(cx, cy, &g_benchmark);
            if (!g_benchmark.test_complete) {
                if (ui_button(win_x + (win_w-140)/2, win_y + win_h - 56, 140, 38,
                              g_benchmark.test_progress > 0 ? "Stop" : "Start Benchmark", C_ACCENT))
                    disk_benchmark_run_async(util_drive_idx, &g_benchmark, BENCHMARK_FULL);
            }
            break;
        case 2:
            bad_sector_scan_render(cx, cy, &g_bad_scan);
            if (!g_bad_scan.scan_complete) {
                const char* slbl = g_bad_scan.scan_active ? "Pause" : (g_bad_scan.scan_paused ? "Resume" : "Start Scan");
                if (ui_button(win_x + (win_w-140)/2, win_y + win_h - 56, 140, 38, slbl, C_ACCENT)) {
                    if (!g_bad_scan.scan_active && !g_bad_scan.scan_paused)
                        bad_sector_scan_start(util_drive_idx, &g_bad_scan, SCAN_MODE_STANDARD);
                    else if (g_bad_scan.scan_active) bad_sector_scan_pause(&g_bad_scan);
                    else bad_sector_scan_resume(&g_bad_scan);
                }
            }
            break;
        case 3:
            disk_wipe_render(cx, cy, &g_wipe);
            if (!g_wipe.wipe_complete) {
                if (ui_button(win_x + (win_w-180)/2, win_y + win_h - 56, 180, 38,
                              g_wipe.wipe_active ? "Stop Wipe" : "Start Wipe (Zeros)", C_DANGER))
                    disk_wipe_start(util_drive_idx, &g_wipe, WIPE_MODE_ZEROS, 0,
                                    ide_devices[util_drive_idx].present ? ide_devices[util_drive_idx].sectors : 0);
            }
            break;
        case 4:
            disk_clone_render(cx, cy, &g_clone);
            if (!g_clone.clone_complete) {
                if (ui_button(win_x + (win_w-140)/2, win_y + win_h - 56, 140, 38,
                              g_clone.clone_active ? "Stop Clone" : "Start Clone", C_ACCENT))
                    disk_clone_start(0, 1, &g_clone, 0);
            }
            break;
        case 5:
            surface_scan_render(cx, cy, &g_surface_scan);
            if (!g_surface_scan.scan_complete) {
                if (ui_button(win_x + (win_w-140)/2, win_y + win_h - 56, 140, 38,
                              g_surface_scan.scan_active ? "Stop Scan" : "Start Surface Scan", C_ACCENT))
                    surface_scan_start(util_drive_idx, &g_surface_scan, 0,
                                       ide_devices[util_drive_idx].present ? ide_devices[util_drive_idx].sectors : 0);
            }
            break;
        case 6:
            fs_check_render(cx, cy, &g_fs_check);
            if (!g_fs_check.check_complete) {
                if (ui_button(win_x + (win_w-140)/2, win_y + win_h - 56, 140, 38,
                              g_fs_check.check_active ? "Stop Check" : "Run FS Check", C_ACCENT))
                    fs_check_start(util_drive_idx, (util_part_idx >= 0) ? util_part_idx : 0, &g_fs_check);
            }
            break;
    }
    if (ui_button(win_x + win_w - 106, win_y + win_h - 56, 90, 38, "Close", C_SIDEBAR)) {
        disk_tools_window_open = 0; disk_tool_selected = 0;
    }
}

/* --- Installing --- */
void render_installing(void) {
    static uint32_t anim = 0; anim++;
    aqua_desktop();
    draw_centered_text(HEADER_HEIGHT + 64, "Installing", 3, 0xFFFFFFFFu);
    int bar_y = HEADER_HEIGHT + 470, bar_w = 520, bar_x = (WIN_W - bar_w) / 2;
    char vol[80];
    strcpy(vol, "Installing Camel OS on the volume \x22");
    strcat(vol, (selected_drive_idx >= 0 && ide_devices[selected_drive_idx].present)
                ? ide_devices[selected_drive_idx].model : "Camel OS");
    strcat(vol, "\x22");
    gfx_draw_string(CX - (int)strlen(vol) * 4, bar_y - 22, vol, 0xFFE8E8ECu);
    gfx_fill_rounded_rect(bar_x + 2, bar_y + 3, bar_w, 16, C_SHADOW, 8);
    gfx_fill_rounded_rect(bar_x,     bar_y,     bar_w, 16, 0xFF9A9A9Cu, 8);
    gfx_fill_rounded_rect(bar_x + 1, bar_y + 1, bar_w - 2, 14, 0xFFFFFFFFu, 7);
    if (install_pct > 0) {
        int fill = ((bar_w - 4) * install_pct) / 100;
        if (fill > 2) {
            gfx_fill_rounded_rect(bar_x + 2, bar_y + 2, fill, 12, 0xFF1E6FE0u, 6);
            gfx_fill_rounded_rect(bar_x + 2, bar_y + 2, fill, 5,  0x70FFFFFFu, 3);
        }
    }
    for (int i = 0; i < 5; i++) {
        int dx = CX - 48 + i * 24;
        uint32_t dc = (i < install_step) ? C_SUCCESS : (i == install_step ? C_ACCENT : C_BORDER);
        gfx_fill_rounded_rect(dx, bar_y + 24, 10, 10, dc, 5);
        if (i < 4) gfx_fill_rect(dx + 10, bar_y + 28, 14, 2, (i < install_step) ? C_SUCCESS : C_BORDER);
    }
    static uint32_t f0 = 0, cal_f = 0, cal_t = 0, ms_per_frame_x10 = 0;
    static int eta_armed = 0;
    if (install_pct == 0 && install_step == 0) eta_armed = 0;
    if (!eta_armed) { f0 = anim; cal_f = anim; cal_t = get_tick_count(); ms_per_frame_x10 = 0; eta_armed = 1; }
    if (ms_per_frame_x10 == 0 && (anim - cal_f) >= 40) {
        uint32_t dt = get_tick_count() - cal_t, df = anim - cal_f;
        if (dt > 0 && df > 0) ms_per_frame_x10 = (dt * 10u) / df;
        if (ms_per_frame_x10 == 0) ms_per_frame_x10 = 200;
    }
    char eta[48];
    if (ms_per_frame_x10 == 0 || install_pct < 3) strcpy(eta, "Calculating...");
    else {
        uint32_t elapsed_ms = (ms_per_frame_x10 * (anim - f0)) / 10u;
        uint32_t remain_ms  = (elapsed_ms / (uint32_t)install_pct) * (uint32_t)(100 - install_pct);
        uint32_t remain_s   = remain_ms / 1000u;
        if (remain_s < 1) strcpy(eta, "a few seconds remaining");
        else if (remain_s < 60) { int_to_str((int)remain_s, eta);
            strcat(eta, (remain_s == 1) ? " second remaining" : " seconds remaining"); }
        else if (remain_s < 3600) { uint32_t m = remain_s / 60; int_to_str((int)m, eta);
            strcat(eta, (m == 1) ? " minute remaining" : " minutes remaining"); }
        else { uint32_t h = remain_s / 3600; int_to_str((int)h, eta);
            strcat(eta, (h == 1) ? " hour remaining" : " hours remaining"); }
    }
    char tr[96]; strcpy(tr, "Time Remaining:  "); strcat(tr, eta);
    gfx_draw_string(bar_x, bar_y + 44, tr, 0xFFD0D0D4u);
    gfx_draw_string(bar_x + bar_w - (int)strlen(install_status) * 8, bar_y + 44, install_status, 0xFFD0D0D4u);
    int py = WIN_H - 64;
    draw_pill(CX - 130, py, 120, 40, "Go Back", C_WHITE, 0);
    draw_pill(CX +  10, py, 120, 40, "Install", C_WHITE, 0);
    install_tick();
}

/* --- Success --- */
void render_success(void) {
    aqua_desktop();
    int cy0 = CY - 120;
    gfx_fill_rounded_rect(CX - 46 + 2, cy0 + 3, 92, 92, C_SHADOW, 46);
    gfx_fill_rounded_rect(CX - 46, cy0, 92, 92, C_SUCCESS, 46);
    gfx_fill_rounded_rect(CX - 44, cy0 + 2, 88, 40, 0x55FFFFFFu, 20);
    for (int t = -2; t <= 2; t++) {
        gfx_draw_line(CX - 16, cy0 + 48 + t, CX - 4,  cy0 + 60 + t, 0xFFFFFFFFu);
        gfx_draw_line(CX - 4,  cy0 + 60 + t, CX + 18, cy0 + 34 + t, 0xFFFFFFFFu);
    }
    draw_centered_text(cy0 + 112, "Installation Complete", 3, 0xFFFFFFFFu);
    draw_centered_text(cy0 + 152, "Camel OS is ready. Remove the install media and restart.", 1, 0xFFD0D0D4u);
    int cw = 440, cx0 = (WIN_W - cw) / 2, cyy = cy0 + 184;
    gfx_fill_rounded_rect(cx0 + 2, cyy + 2, cw, 84, C_SHADOW, 12);
    gfx_fill_rounded_rect(cx0,     cyy,     cw, 84, C_BORDER, 12);
    gfx_fill_rounded_rect(cx0 + 1, cyy + 1, cw - 2, 82, 0xFFFFFFFFu, 11);
    gfx_draw_string(cx0 + 20,  cyy + 14, "Destination",  C_TEXT_MUTED);
    gfx_draw_string(cx0 + 130, cyy + 14, selected_drive_idx == 0 ? "ATA 0 (Primary)" : "ATA 1 (Secondary)", C_TEXT_DARK);
    gfx_draw_string(cx0 + 20,  cyy + 36, "Filesystem",   C_TEXT_MUTED);
    gfx_draw_string(cx0 + 130, cyy + 36, "PFS32 (Camel OS Native)", C_TEXT_DARK);
    gfx_draw_string(cx0 + 20,  cyy + 58, "Installed",    C_TEXT_MUTED);
    gfx_draw_string(cx0 + 130, cyy + 58, "9 system components", C_TEXT_DARK);
    if (ui_button(CX - 110, cyy + 104, 220, 46, "Restart", C_ACCENT)) {
        pfs32_sync(); disk_flush_cache();
        for (volatile int _i = 0; _i < 100000; _i++) {}
        outb(0x64, 0xFE);
    }
}

/* --- Failure --- */
void render_failure(void) {
    aqua_desktop();
    int cy0 = CY - 120;
    gfx_fill_rounded_rect(CX - 46 + 2, cy0 + 3, 92, 92, C_SHADOW, 46);
    gfx_fill_rounded_rect(CX - 46, cy0, 92, 92, C_DANGER, 46);
    gfx_fill_rounded_rect(CX - 44, cy0 + 2, 88, 40, 0x55FFFFFFu, 20);
    gfx_draw_string_scaled(CX - 11, cy0 + 22, "!", 0xFFFFFFFFu, 3);
    draw_centered_text(cy0 + 112, "Installation Failed", 3, 0xFFFFFFFFu);
    if (install_error_msg[0]) {
        int mw = strlen(install_error_msg) * 8; if (mw > 520) mw = 520;
        gfx_fill_rounded_rect(CX - mw / 2 - 16, cy0 + 150, mw + 32, 34, 0xFFFFECECu, 8);
        gfx_draw_rect(CX - mw / 2 - 16, cy0 + 150, mw + 32, 34, C_DANGER);
        gfx_draw_string(CX - mw / 2, cy0 + 161, install_error_msg, C_DANGER);
    }
    draw_centered_text(cy0 + 200, "Open  View > Installer Logs  for the full trace.", 1, 0xFFD0D0D4u);
    if (ui_button(CX - 110, cy0 + 232, 220, 46, "Restart", C_WHITE)) {
        disk_flush_cache();
        for (volatile int _i = 0; _i < 100000; _i++) {}
        outb(0x64, 0xFE);
    }
}

/* =============================================================================
 * INSTALL LOGIC (unchanged)
 * ============================================================================= */
int install_file(const char* path, uint8_t* start, uint8_t* end) {
    uint32_t size = (uint32_t)(end - start);
    int create_res = pfs32_create_file(path);
    if (create_res != 0 && create_res != -5) return -1;
    int write_res = pfs32_write_file(path, start, size);
    if (write_res < 0) return -2;
    return 0;
}
typedef struct { const char* path; uint8_t* start; uint8_t* end; } install_file_entry_t;
static install_file_entry_t install_files[] = {
    { "/usr/lib/math.cdl",      0,0},
    { "/usr/lib/usr32.cdl",     0,0},
    { "/usr/lib/syskernel.cdl", 0,0},
    { "/usr/lib/proc.cdl",      0,0},
    { "/usr/lib/timer.cdl",     0,0},
    { "/usr/lib/gui.cdl",       0,0},
    { "/usr/lib/sysmon.cdl",    0,0},
    { "/usr/lib/jsengine.cdl",  0,0},
    { "/usr/apps/NetDiag.cdl",  0,0},
    {0,0,0}
};
void init_install_files(void) {
    install_files[0].start=app_math_start;       install_files[0].end=app_math_end;
    install_files[1].start=app_usr32_start;      install_files[1].end=app_usr32_end;
    install_files[2].start=app_syskernel_start;  install_files[2].end=app_syskernel_end;
    install_files[3].start=app_proc_start;       install_files[3].end=app_proc_end;
    install_files[4].start=app_timer_start;      install_files[4].end=app_timer_end;
    install_files[5].start=app_gui_start;        install_files[5].end=app_gui_end;
    install_files[6].start=app_sysmon_start;     install_files[6].end=app_sysmon_end;
    install_files[7].start=app_jsengine_start;   install_files[7].end=app_jsengine_end;
    install_files[8].start=app_netdiag_start;    install_files[8].end=app_netdiag_end;
}
void install_tick(void) {
    disk_set_drive(selected_drive_idx);
    if (install_error) { current_state = STATE_FAILURE; return; }
    install_step_start_tick++;
    if (install_pct < install_target_pct) {
        int diff = install_target_pct - install_pct;
        install_pct += (diff > 10) ? 3 : 2;
        if (install_pct > install_target_pct) install_pct = install_target_pct;
    }
    if (install_step_start_tick > 3000 && install_step < 4) {
        extern int pfs32_sync(void); extern void disk_flush_cache(void);
        pfs32_sync(); disk_flush_cache();
        add_log("WARN: Step watchdog expired (60s), force-advancing to next step");
        install_step++; install_step_tick = 0; install_sub_step = 0;
        install_step_start_tick = 0; install_idle_ticks = 0;
        return;
    }
    if (install_step == 0) {
        if (install_step_tick == 0) {
            strcpy(install_status, "Writing Bootloader & Partition Table...");
            add_log("Step 0: Writing bootloader & partition table");
            uint8_t z[512]; memset(z, 0, 512);
            if (ata_write_sector(selected_drive_idx, 0, z) < 0) {
                add_log("ERROR: Failed to wipe MBR sector");
                if (ata_write_sector(selected_drive_idx, 0, z) < 0) {
                    add_log("FATAL: MBR write failed after retry"); install_error = 1; return;
                }
            }
            mbr_sector_t mbr; memcpy(&mbr, mbr_bin_start, 512);
            uint64_t total64 = ide_devices[selected_drive_idx].sectors;
            uint32_t total = (total64 > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)total64;
            uint32_t part_start = 16384;
            mbr.partitions[0].status=0x80; mbr.partitions[0].type=0x7F;
            mbr.partitions[0].lba_start=part_start; mbr.partitions[0].lba_length=total-part_start;
            mbr.signature=0xAA55;
            if (ata_write_sector(selected_drive_idx, 0, (uint8_t*)&mbr) < 0) {
                add_log("ERROR: Failed to write MBR — retrying");
                if (ata_write_sector(selected_drive_idx, 0, (uint8_t*)&mbr) < 0) {
                    add_log("FATAL: MBR write failed after retry"); install_error = 1; return;
                }
            } else add_log("MBR written successfully");
            install_step_tick = 1; install_target_pct = 10;
            install_step++; install_step_tick = 0; install_step_start_tick = 0;
            add_log("Bootloader step complete");
            return;
        }
        return;
    }
    if (install_step == 1) {
        strcpy(install_status, "Copying Kernel Image...");
        uint32_t k_size = system_bin_end - system_bin_start;
        if (k_size == 0) { add_log("ERROR: Kernel image is empty");
            strcpy(install_error_msg, "Kernel image is empty"); install_error = 1;
            current_state = STATE_FAILURE; return; }
        uint32_t k_sectors = (k_size + 511) / 512;
        if (k_sectors == 0) { add_log("ERROR: Kernel has zero sectors");
            strcpy(install_error_msg, "Kernel has zero sectors"); install_error = 1;
            current_state = STATE_FAILURE; return; }
        if (install_total_write_failures > 3000 && kernel_write_offset < k_sectors) {
            add_log("WARN: Write failure threshold exceeded, skipping remaining kernel sectors");
            kernel_write_offset = k_sectors;
        }
        int sectors_this = 0;
        while (kernel_write_offset < k_sectors && sectors_this < 16) {
            uint8_t buf[512]; memset(buf, 0, 512);
            uint32_t rem = k_size - (kernel_write_offset * 512);
            memcpy(buf, system_bin_start + (kernel_write_offset * 512), (rem>512)?512:rem);
            if (ata_write_sector(selected_drive_idx, 1+kernel_write_offset, buf) < 0) {
                install_idle_ticks++; install_total_write_failures++;
                if (install_idle_ticks > 300) {
                    uint8_t verify[512];
                    if (ata_read_sector(selected_drive_idx, 1+kernel_write_offset, verify) == 0 &&
                        memcmp(buf, verify, 512) == 0)
                        add_log("INFO: ATA write reported error but sector verified OK");
                    else
                        add_log("WARN: ATA write timeout, sector skipped");
                    install_idle_ticks = 0; kernel_write_offset++; sectors_this++; continue;
                }
                return;
            }
            install_idle_ticks = 0; kernel_write_offset++; sectors_this++;
        }
        install_target_pct = 10 + (kernel_write_offset * 20 / k_sectors);
        if (kernel_write_offset >= k_sectors) {
            install_target_pct = 30;
            install_step++; install_step_tick = 0; install_step_start_tick = 0;
            add_log("Kernel copied");
        }
        return;
    }
    if (install_step == 2) {
        if (install_step_tick == 0) {
            strcpy(install_status, "Formatting PFS32 Partition...");
            add_log("Formatting partition (fast mode)");
            uint32_t part_start=16384;
            uint64_t part_size64 = ide_devices[selected_drive_idx].sectors - part_start;
            uint32_t part_size = (part_size64 > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)part_size64;
            pfs32_init(part_start, part_size);
            extern uint32_t pfs32_format_fast(const char* label, uint32_t total);
            uint32_t fmt_result = pfs32_format_fast("Camel Sys", part_size);
            if ((int)fmt_result < 0) add_log("WARN: format returned error, continuing");
            else add_log("Format completed successfully");
            pfs32_sync(); disk_flush_cache();
            {
                int verify_ok = 0;
                for (int va = 0; va < 3; va++) {
                    for (volatile int _d = 0; _d < 10000; _d++) {}
                    if (va > 0) { pfs32_sync(); disk_flush_cache(); }
                    uint8_t vb[512];
                    ata_read_sector(selected_drive_idx, part_start, vb);
                    if (*(uint32_t*)vb == PFS32_MAGIC) { verify_ok = 1; add_log("Superblock verified"); break; }
                }
                if (!verify_ok) add_log("WARN: Superblock verify failed (QEMU cache?)");
            }
            install_step_tick = 1; install_target_pct = 50;
            install_step++; install_step_tick = 0; install_step_start_tick = 0;
            add_log("PFS32 formatted");
            return;
        }
        return;
    }
    if (install_step == 3) {
        if (install_sub_step == 0) {
            if (install_step_tick == 0) {
                strcpy(install_status, "Creating Directory Structure...");
                add_log("Creating directories");
                pfs32_create_directory("/Users");
                pfs32_create_directory("/usr");  pfs32_create_directory("/usr/lib");
                pfs32_create_directory("/usr/apps");
                pfs32_create_directory("/Applications");
                pfs32_create_directory("/Library"); pfs32_create_directory("/Library/Preferences");
                pfs32_create_directory("/etc");
                install_step_tick = 1; install_target_pct = 55;
                install_sub_step=1; init_install_files(); install_file_idx=0; install_step_tick = 0;
                install_step_start_tick = 0;
            }
            return;
        }
        if (install_file_idx < 9) {
            install_file_entry_t* f = &install_files[install_file_idx];
            if (f->path && f->start && f->end) {
                strcpy(install_status, "Installing: "); strcat(install_status, f->path);
                int ifile_res = install_file(f->path, f->start, f->end);
                if (ifile_res < 0) {
                    char warn_buf[128]; strcpy(warn_buf, "WARN: Failed to install "); strcat(warn_buf, f->path);
                    if (strstr(f->path, "gui.cdl") || strstr(f->path, "syskernel.cdl") ||
                        strstr(f->path, "proc.cdl") || strstr(f->path, "timer.cdl")) {
                        strcat(warn_buf, " (CRITICAL — retrying)"); add_log(warn_buf);
                        ifile_res = install_file(f->path, f->start, f->end);
                        if (ifile_res < 0) add_log("ERROR: Critical file install failed after retry");
                    } else { strcat(warn_buf, " (non-critical)"); add_log(warn_buf); }
                } else { char ok_buf[128]; strcpy(ok_buf, "Installed "); strcat(ok_buf, f->path); add_log(ok_buf); }
                { extern int pfs32_sync(void); extern void disk_flush_cache(void);
                  pfs32_sync(); disk_flush_cache(); }
            }
            install_file_idx++;
            install_target_pct = 55 + (install_file_idx * 35) / 9;
            return;
        }
        {
            struct { const char* path; const char* name; const char* cdl; const char* type; } app_bundles[] = {
                {"/Applications/Files.app",     "Files",     "/usr/lib/gui.cdl",     "cdl"},
                {"/Applications/Terminal.app",  "Terminal",  "",                      "builtin"},
                {"/Applications/Monitor.app",   "Monitor",   "/usr/lib/sysmon.cdl",  "cdl"},
                {"/Applications/NetDiag.app",   "NetDiag",   "/usr/apps/NetDiag.cdl","cdl"},
                {"/Applications/TextEdit.app",  "TextEdit",  "",                      "builtin"},
                {"/Applications/Browser.app",   "Browser",   "",                      "builtin"},
                {"/Applications/Settings.app",  "Settings",  "",                      "builtin"},
                {"/Applications/MacTest.app",   "MacTest",   "",                      "builtin"},
            };
            for (int i = 0; i < 8; i++) {
                pfs32_create_directory(app_bundles[i].path);
                char contents_path[256]; strcpy(contents_path, app_bundles[i].path); strcat(contents_path, "/Contents");
                pfs32_create_directory(contents_path);
                char macos_path[256]; strcpy(macos_path, contents_path); strcat(macos_path, "/MacOS");
                pfs32_create_directory(macos_path);
                char res_path[256]; strcpy(res_path, contents_path); strcat(res_path, "/Resources");
                pfs32_create_directory(res_path);
                char plist_path[256]; strcpy(plist_path, contents_path); strcat(plist_path, "/Info.plist");
                char plist[512]; int plen = 0;
                plen += sprintf(plist + plen, "# CamelOS App Bundle Info\n");
                plen += sprintf(plist + plen, "CFBundleName=%s\n", app_bundles[i].name);
                plen += sprintf(plist + plen, "CFBundleIdentifier=com.camelos.%s\n", app_bundles[i].name);
                plen += sprintf(plist + plen, "CFBundleExecutable=%s\n", app_bundles[i].name);
                plen += sprintf(plist + plen, "CFBundleVersion=1.0\n");
                plen += sprintf(plist + plen, "CFBundleType=%s\n", app_bundles[i].type);
                plen += sprintf(plist + plen, "CFBundleMinOSVersion=1.0\n");
                if (app_bundles[i].cdl[0])
                    plen += sprintf(plist + plen, "CFBundleCDLPath=%s\n", app_bundles[i].cdl);
                pfs32_create_file(plist_path);
                pfs32_write_file(plist_path, (uint8_t*)plist, plen);
            }
        }
        pfs32_sync(); disk_flush_cache();
        install_target_pct = 90;
        install_step++; install_sub_step=0; install_step_tick = 0; install_step_start_tick = 0;
        add_log("System files installed");
        return;
    }
    if (install_step == 4) {
        strcpy(install_status, "Finalizing Installation...");
        add_log("Step 4: Finalizing installation");
        pfs32_sync(); disk_flush_cache();
        add_log("Disk cache flushed");
        install_target_pct = 100;
        if (install_pct >= 100) { add_log("Installation complete!"); current_state = STATE_SUCCESS; }
    }
}

/* =============================================================================
 * MAIN
 * ============================================================================= */
int main(uint32_t magic, void* mb_ptr) {
    uint32_t heap = (uint32_t)&_bss_end;
    if (heap % 16) heap += 16 - (heap % 16);
    init_heap(heap, 16 * 1024 * 1024);
    if (mb_ptr) {
        multiboot_info_t* mb = (multiboot_info_t*)mb_ptr;
        if (mb->flags & (1 << 0)) total_memory_kb = mb->mem_lower + mb->mem_upper;
    }
    gfx_init_hal(mb_ptr);
    init_serial();
    vga_mute_log(1);
    init_ps2_mouse();
    theme_init();
    disk_health_init();
    install_step=0; install_sub_step=0; install_file_idx=0;
    install_error=0; install_error_msg[0]=0;
    kernel_write_offset=0; install_pct=0; install_target_pct=0; install_step_tick=0;
    install_idle_ticks=0; install_step_start_tick=0; install_total_write_failures=0;
    sys_check_done=0; util_menu_selected=0;
    scan_hardware();
    add_log("Camel OS Installer started (Aqua Edition)");
    add_log("Mouse: polling mode");
    add_log("Scanning hardware...");
    for (int i = 0; i < 2; i++) {
        if (ide_devices[i].present) {
            char buf[80]; strcpy(buf, "Drive "); char n[4]; int_to_str(i, n);
            strcat(buf, n); strcat(buf, ": ");
            char sz[32]; format_disk_size(ide_devices[i].sectors, sz); strcat(buf, sz);
            strcat(buf, " - "); strcat(buf, ide_devices[i].model);
            add_log(buf);
        }
    }
    while (1) {
        poll_input();
        gfx_fill_rect(0, 0, WIN_W, WIN_H, C_BG);
        process_menu_bar(mx, my, mb_clicked);
        switch (current_state) {
            case STATE_WELCOME:     render_welcome();     break;
            case STATE_UTILITIES:   render_utilities();   break;
            case STATE_SYS_CHECK:   render_sys_check();   break;
            case STATE_DISK_UTIL:   render_disk_utility();break;
            case STATE_SELECT_DISK: render_select_disk(); break;
            case STATE_INSTALLING:  render_installing();  break;
            case STATE_SUCCESS:     render_success();     break;
            case STATE_FAILURE:     render_failure();     break;
        }
        render_logs_window();
        render_disk_tools_window();
        render_modal();
        render_menu_bar();
        draw_cursor();
        gfx_swap_buffers();
        mb_prev = mb_left;
    }
    return 0;
}