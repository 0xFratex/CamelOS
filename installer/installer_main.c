/* installer/installer_main.c - Camel OS Installer (System Architect Edition) */
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/ata.h"
#include "../hal/drivers/serial.h"
#include "../common/ports.h"
#include "../include/string.h"
#include "../fs/pfs32.h"
#include "../fs/disk.h"
#include "../core/memory.h"
#include "../hal/cpu/idt.h"
#include "../hal/drivers/mouse.h"
#include "../kernel/assets.h"

// --- Payload Externs ---
extern uint8_t system_bin_start[], system_bin_end[];
extern uint8_t mbr_bin_start[];
extern uint32_t _bss_end;

// Apps & Libs (Standard payload externs)
extern uint8_t app_terminal_start[], app_terminal_end[];
extern uint8_t app_files_start[], app_files_end[];
extern uint8_t app_waterhole_start[], app_waterhole_end[];
extern uint8_t app_nettools_start[], app_nettools_end[];
extern uint8_t app_textedit_start[], app_textedit_end[];
extern uint8_t app_browser_start[], app_browser_end[];
extern uint8_t lib_math_start[], lib_math_end[];
extern uint8_t lib_usr32_start[], lib_usr32_end[];
extern uint8_t lib_syskernel_start[], lib_syskernel_end[];
extern uint8_t lib_proc_start[], lib_proc_end[];
extern uint8_t lib_timer_start[], lib_timer_end[];
extern uint8_t lib_gui_start[], lib_gui_end[];
extern uint8_t lib_sysmon_start[], lib_sysmon_end[];

// --- Design Configuration ---
#define WIN_W 1024
#define WIN_H 768
#define CX (WIN_W / 2)
#define CY (WIN_H / 2)

// Colors - macOS X inspired palette
#define C_BG            0xFFF2F2F7
#define C_SIDEBAR       0xFFE8E8ED
#define C_WHITE         0xFFFFFFFF
#define C_TEXT_DARK     0xFF1C1C1E
#define C_TEXT_MUTED    0xFF8E8E93
#define C_ACCENT        0xFF007AFF
#define C_ACCENT_HOVER  0xFF0051D5
#define C_DANGER        0xFFFF375F
#define C_BORDER        0xFFC6C6C8
#define C_MODAL_DIM     0x80000000
#define C_SHADOW        0x40000000

// Partition Colors - macOS X style
#define C_PART_FREE     0xFFE5E5EA
#define C_PART_CAMEL    0xFF007AFF
#define C_PART_OTHER    0xFF5856D6
#define C_PART_BOOT     0xFFFF9500
#define C_PART_SYS      0xFF34C759 // Green

// --- MBR Structures ---
typedef struct {
    uint8_t status;       // 0x80 = Active
    uint8_t chs_start[3];
    uint8_t type;         // 0x83=Linux, 0x07=NTFS, 0x7F=CamelOS
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t lba_length;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    uint8_t bootstrap[446];
    mbr_entry_t partitions[4];
    uint16_t signature;   // 0x55AA
} __attribute__((packed)) mbr_sector_t;

// --- State Management ---
typedef enum {
    STATE_WELCOME,
    STATE_DISK_UTIL,
    STATE_SELECT_DISK,
    STATE_INSTALLING,
    STATE_SUCCESS,
    STATE_FAILURE
} InstallerState;

InstallerState current_state = STATE_WELCOME;

// Selection State
int selected_drive_idx = -1; 
int util_drive_idx = 0;
int util_part_idx = -1; // -1 = None, 0-3 = MBR Slots

// Modal State
int modal_active = 0;
char modal_title[32];
char modal_msg[64];
char modal_action_label[16];
void (*modal_callback)(void) = 0;

// Disk Cache
mbr_sector_t disk_mbr[2]; // Cache for master/slave
int disk_has_mbr[2];

// Installation Progress
int install_step = 0;
int install_sub_step = 0;
int install_file_idx = 0;
int install_pct = 0;
char install_status[64] = "";
uint32_t kernel_write_offset = 0;
int install_error = 0;
char install_error_msg[128] = "";
int install_animation_frame = 0;
uint32_t last_animation_tick = 0;

// Mouse State
extern int mouse_x, mouse_y, mouse_btn_left;
int mx = 512, my = 384;
int mb_left = 0, mb_prev = 0;

// Log Window State
int logs_window_open = 0;
char install_log[2048] = "";
int log_line_count = 0;

// --- Cursor Bitmap (12x19 Arrow) ---
// 0=Trans, 1=Black, 2=White
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

// --- Logging Subsystem ---

void add_log(const char* msg) {
    if (log_line_count >= 32) {
        // Shift log up if full
        char* new_start = strchr(install_log, '\n') + 1;
        memmove(install_log, new_start, strlen(new_start) + 1);
        log_line_count--;
    }
    
    strcat(install_log, msg);
    strcat(install_log, "\n");
    log_line_count++;
}

// --- Input Subsystem ---

#define PS2_MOUSE_PORT 0x60
#define PS2_STATUS_PORT 0x64

void poll_input() {
    static uint8_t packet[3];
    static int cycle = 0;
    
    // Check if there's mouse data available
    while ((inb(PS2_STATUS_PORT) & 1)) {
        uint8_t b = inb(PS2_MOUSE_PORT);
        
        if (cycle == 0 && !(b & 0x08)) {
            // Not a valid start of packet, reset
            cycle = 0;
            continue;
        }
        
        packet[cycle++] = b;
        
        if (cycle == 3) {
            cycle = 0;
            
            // Check for overflow
            if (packet[0] & 0xC0) continue;
            
            int dx = (int8_t)packet[1];
            int dy = (int8_t)packet[2];
            
            mb_left = packet[0] & 1;
            
            // Simple and direct mouse movement (no smoothing for better polling)
            mx += dx;
            my -= dy;
            
            // Clamp mouse position
            if (mx < 0) mx = 0;
            if (mx >= WIN_W) mx = WIN_W - 1;
            if (my < 0) my = 0;
            if (my >= WIN_H) my = WIN_H - 1;
        }
    }
}

void draw_cursor() {
    // Hardware cursor is best, but software cursor logic here:
    // We draw pixel by pixel from bitmap
    for(int y=0; y<18; y++) {
        for(int x=0; x<12; x++) {
            uint8_t p = cursor_bmp[y*12+x];
            if (p == 1) gfx_put_pixel(mx+x, my+y, 0xFF000000); // Black Border
            else if (p == 2) gfx_put_pixel(mx+x, my+y, 0xFFFFFFFF); // White Fill
        }
    }
}

// --- Helpers ---

void format_size(uint32_t sectors, char* out) {
    if (sectors == 0) { strcpy(out, "0 MB"); return; }
    uint32_t mb = sectors / 2048; // 512 bytes per sector
    if (mb >= 1024) {
        int gb = mb / 1024;
        int dec = (mb % 1024) * 10 / 1024;
        char buf[16]; int_to_str(gb, out); strcat(out, "."); 
        int_to_str(dec, buf); strcat(out, buf); strcat(out, " GB");
    } else {
        int_to_str(mb, out); strcat(out, " MB");
    }
}

// Filesystem magic numbers for detection
#define NTFS_MAGIC          0x55AA
#define FAT32_BOOT_SIG      0xAA55
#define EXT4_MAGIC          0xEF53

int detect_filesystem(int drive, uint32_t lba_start, uint8_t* type_out) {
    uint8_t buf[512];
    ata_read_sector(drive, lba_start, buf);
    
    // Check for filesystem magic numbers
    uint16_t* magic16 = (uint16_t*)buf;
    uint32_t* magic32 = (uint32_t*)buf;
    
    // NTFS: magic at offset 0x55 (0xEB 0x52 0x90 "NTFS")
    if (memcmp(buf + 3, "NTFS", 4) == 0) {
        *type_out = 0x07; // NTFS
        return 1;
    }
    
    // FAT32: boot sector signature 0xAA55 and FAT32 BPB
    if (magic16[255] == FAT32_BOOT_SIG) {
        // Check BPB for FAT32 signature
        uint32_t fat_size = *(uint32_t*)(buf + 0x16);
        if (fat_size != 0) {
            *type_out = 0x0B; // FAT32
            return 1;
        }
    }
    
    // EXT4: superblock at block 0, offset 0x38
    if (lba_start + 2 < ide_devices[drive].sectors) {
        ata_read_sector(drive, lba_start + 2, buf);
        if (magic16[0x19] == EXT4_MAGIC) { // 0xEF53 at offset 0x38
            *type_out = 0x83; // EXT4
            return 1;
        }
    }
    
    // PFS32: magic at offset 0 of sector 16384
    if (lba_start <= 16384 && lba_start + 16384 < ide_devices[drive].sectors) {
        ata_read_sector(drive, lba_start + 16384, buf);
        if (magic32[0] == PFS32_MAGIC) {
            *type_out = 0x7F; // PFS32
            return 1;
        }
    }
    
    *type_out = 0xFF; // RAW
    return 0;
}

void get_part_type_name(uint8_t type, char* out) {
    switch(type) {
        case 0x00: strcpy(out, "Free Space"); break;
        case 0x07: strcpy(out, "NTFS"); break;
        case 0x0B: strcpy(out, "FAT32"); break;
        case 0x83: strcpy(out, "EXT4"); break;
        case 0x7F: strcpy(out, "PFS32"); break;
        case 0xFF: strcpy(out, "RAW"); break;
        default:   strcpy(out, "Unknown"); break;
    }
}

// --- Disk Operations ---

void read_drive_mbr(int drive) {
    if (!ide_devices[drive].present) return;
    disk_set_drive(drive);
    ata_read_sector(drive, 0, (uint8_t*)&disk_mbr[drive]);
    
    // Validate Signature
    if (disk_mbr[drive].signature == 0xAA55) {
        disk_has_mbr[drive] = 1;
    } else {
        disk_has_mbr[drive] = 0; // RAW
        memset(&disk_mbr[drive], 0, sizeof(mbr_sector_t));
    }
}

void scan_hardware() {
    ata_identify_device(0);
    read_drive_mbr(0);
    ata_identify_device(1);
    read_drive_mbr(1);
}

// Actions
void action_erase_disk() {
    int drv = util_drive_idx;
    uint8_t z[512]; memset(z, 0, 512);
    // Wipe MBR
    ata_write_sector(drv, 0, z);
    // Wipe PFS Superblock area (just in case)
    ata_write_sector(drv, 16384, z); // 8MB mark
    scan_hardware(); // Refresh
    modal_active = 0;
}

void action_format_partition(uint8_t fs_type) {
    int drv = util_drive_idx;
    if (util_part_idx < 0) return;
    
    mbr_entry_t* part = &disk_mbr[drv].partitions[util_part_idx];
    if (part->type == 0) return;
    
    strcpy(modal_msg, "Formatting partition...");
    strcpy(modal_action_label, "Format");
    
    // Format according to filesystem type
    switch(fs_type) {
        case 0x7F: // PFS32
            pfs32_init(part->lba_start, part->lba_length);
            pfs32_format("Camel Partition", part->lba_length);
            add_log("Partition formatted as PFS32");
            break;
            
        case 0x0B: // FAT32
            // Simple FAT32 boot sector creation
            uint8_t fat_boot[512];
            memset(fat_boot, 0, 512);
            fat_boot[0] = 0xEB; // Jump instruction
            fat_boot[1] = 0x58;
            fat_boot[2] = 0x90;
            memcpy(fat_boot + 3, "FAT32   ", 8);
            // Set BPB parameters
            *(uint16_t*)(fat_boot + 0x10) = 512; // Bytes per sector
            fat_boot[0x12] = 1; // Sectors per cluster
            *(uint16_t*)(fat_boot + 0x16) = 2048; // Number of FAT copies
            fat_boot[0x52] = 0x29; // Extended boot signature
            memcpy(fat_boot + 0x54, "CamelOS  ", 8); // Volume label
            memcpy(fat_boot + 0x60, "FAT32   ", 8); // File system type
            *(uint16_t*)(fat_boot + 0x1FE) = 0xAA55; // Boot signature
            ata_write_sector(drv, part->lba_start, fat_boot);
            add_log("Partition formatted as FAT32");
            break;
            
        case 0x07: // NTFS
            // Simple NTFS boot sector creation
            uint8_t ntfs_boot[512];
            memset(ntfs_boot, 0, 512);
            ntfs_boot[0] = 0xEB; // Jump instruction
            ntfs_boot[1] = 0x52;
            ntfs_boot[2] = 0x90;
            memcpy(ntfs_boot + 3, "NTFS    ", 8);
            // Set BPB parameters
            *(uint16_t*)(ntfs_boot + 0x10) = 512; // Bytes per sector
            ntfs_boot[0x12] = 1; // Sectors per cluster
            *(uint16_t*)(ntfs_boot + 0x16) = 0; // Reserved sectors
            ntfs_boot[0x18] = 0; // FAT copies
            ntfs_boot[0x52] = 0x80; // Extended boot signature
            memcpy(ntfs_boot + 0x54, "CamelOS  ", 8); // Volume label
            memcpy(ntfs_boot + 0x60, "NTFS    ", 8); // File system type
            *(uint16_t*)(ntfs_boot + 0x1FE) = 0xAA55; // Boot signature
            ata_write_sector(drv, part->lba_start, ntfs_boot);
            add_log("Partition formatted as NTFS");
            break;
            
        case 0x83: // EXT4
            // Simple EXT4 superblock creation
            uint8_t ext4_sb[512];
            memset(ext4_sb, 0, 512);
            *(uint32_t*)(ext4_sb) = 0x1020304; // EXT4 magic
            *(uint32_t*)(ext4_sb + 4) = 1; // Inode count
            *(uint32_t*)(ext4_sb + 8) = 1024; // Block count
            ext4_sb[0x19] = 0x53; // EXT4 magic second part
            ext4_sb[0x18] = 0xEF;
            ata_write_sector(drv, part->lba_start + 2, ext4_sb); // Superblock at block 2
            add_log("Partition formatted as EXT4");
            break;
            
        case 0xFF: // RAW
            // Just wipe the first 100 sectors
            uint8_t zero[512];
            memset(zero, 0, 512);
            for (uint32_t i = 0; i < 100; i++) {
                ata_write_sector(drv, part->lba_start + i, zero);
            }
            add_log("Partition formatted as RAW");
            break;
    }
    
    part->type = fs_type;
    ata_write_sector(drv, 0, (uint8_t*)&disk_mbr[drv]);
    scan_hardware();
    modal_active = 0;
}

void action_create_schema() {
    int drv = util_drive_idx;
    disk_set_drive(drv);
    
    // Create new MBR with one large partition
    mbr_sector_t new_mbr;
    memset(&new_mbr, 0, sizeof(new_mbr));
    
    uint32_t total = ide_devices[drv].sectors;
    uint32_t start = 2048; // Standard 1MB alignment
    uint32_t size = total - start;
    
    new_mbr.partitions[0].status = 0x80; // Bootable
    new_mbr.partitions[0].type = 0x7F;   // Camel OS Type
    new_mbr.partitions[0].lba_start = start;
    new_mbr.partitions[0].lba_length = size;
    new_mbr.signature = 0xAA55;
    
    ata_write_sector(drv, 0, (uint8_t*)&new_mbr);
    
    // Format partition as PFS32 automatically
    action_format_partition(0x7F);
    add_log("Disk initialized and formatted as PFS32");
    scan_hardware();
    modal_active = 0;
}

void action_delete_partition() {
    if (util_part_idx < 0) return;
    int drv = util_drive_idx;
    
    memset(&disk_mbr[drv].partitions[util_part_idx], 0, sizeof(mbr_entry_t));
    ata_write_sector(drv, 0, (uint8_t*)&disk_mbr[drv]);
    
    scan_hardware();
    modal_active = 0;
}

void show_modal(const char* title, const char* msg, const char* btn, void (*cb)(void)) {
    strcpy(modal_title, title);
    strcpy(modal_msg, msg);
    strcpy(modal_action_label, btn);
    modal_callback = cb;
    modal_active = 1;
}

// --- UI Logic ---

// --- Menu System ---

#define HEADER_HEIGHT 28

int measure_text_width(const char* str) { 
    return strlen(str) * 8; 
}

int process_menu_bar(int mx, int my, int click) {
    // Draw header background (Aqua style)
    for(int i=0; i<HEADER_HEIGHT; i++) {
        uint32_t col = (i < HEADER_HEIGHT/2) ? 0xFFF8F8F8 : 0xFFE8E8E8;
        gfx_fill_rect(0, i, WIN_W, 1, col);
    }
    gfx_draw_rect(0, HEADER_HEIGHT, WIN_W, 1, 0xFF888888); 

    static int open_menu_id = -2;
    static int menu_rect_x = 0, menu_rect_y = 0, menu_rect_w = 0, menu_rect_h = 0;
    
    int cur_x = 15;
    int target_menu = -3; 

    // "Camel" menu (Apple logo equivalent)
    int w = measure_text_width("Camel") + 20;
    
    gfx_draw_string(cur_x + 10, 8, "Camel", 0xFF444444);
    gfx_draw_string(cur_x + 11, 8, "Camel", 0xFF444444); 

    if (mx >= cur_x && mx < cur_x + w && my < HEADER_HEIGHT) {
        if(click) target_menu = -1;
    }

    if (open_menu_id == -1) {
        gfx_fill_rect(cur_x, 0, w, HEADER_HEIGHT, 0xFF3D89D6); 
        gfx_draw_string(cur_x + 10, 8, "Camel", 0xFFFFFFFF);
        int menu_y = HEADER_HEIGHT;
        gfx_fill_rect(cur_x, menu_y, 160, 86, 0xF2F2F2F2);
        gfx_draw_rect(cur_x, menu_y, 160, 86, 0xFF888888);
        gfx_draw_string(cur_x + 10, menu_y + 10, "About Camel OS", 0xFF444444);
        gfx_draw_rect(cur_x + 5, menu_y + 30, 150, 1, 0xFFCCCCCC);
        gfx_draw_string(cur_x + 10, menu_y + 40, "Restart", 0xFF444444);
        gfx_draw_string(cur_x + 10, menu_y + 60, "Shutdown", 0xFF444444);
        
        if (click && mx >= cur_x && mx < cur_x + 160 && my >= menu_y) {
            int rel_y = my - menu_y;
            if (rel_y >= 40 && rel_y < 60) {
                outb(0x64, 0xFE); // Restart
            } else if (rel_y >= 60 && rel_y < 80) {
                outw(0x604, 0x2000); // Shutdown
                outw(0xB004, 0x2000);
                asm volatile("cli; hlt");
            }
            open_menu_id = -2;
        }
    }
    cur_x += w;

    // "View" menu
    const char* view_menu_items[] = { "Installer Logs", "-", "Hide Toolbar" };
    int view_menu_count = 3;
    
    w = measure_text_width("View") + 20;
    
    gfx_draw_string(cur_x + 10, 8, "View", 0xFF444444);
    gfx_draw_string(cur_x + 11, 8, "View", 0xFF444444);
    
    if (mx >= cur_x && mx < cur_x + w && my < HEADER_HEIGHT) {
        if(click) target_menu = 0;
    }

    if (open_menu_id == 0) {
        gfx_fill_rect(cur_x, 0, w, HEADER_HEIGHT, 0xFF3D89D6);
        gfx_draw_string(cur_x + 10, 8, "View", 0xFFFFFFFF);
        int menu_y = HEADER_HEIGHT;
        gfx_fill_rect(cur_x, menu_y, 180, view_menu_count * 20 + 6, 0xF2F2F2F2);
        gfx_draw_rect(cur_x, menu_y, 180, view_menu_count * 20 + 6, 0xFF888888);
        
        for(int i=0; i<view_menu_count; i++) {
            int iy = menu_y + 3 + (i * 20);
            const char* label = view_menu_items[i];
            
            if(strcmp(label, "-") == 0) {
                gfx_draw_rect(cur_x + 5, iy + 10, 170, 1, 0xFFCCCCCC);
                continue;
            }
            
            if (mx >= cur_x && mx < cur_x + 180 && my >= iy && my < iy + 20) {
                gfx_fill_rect(cur_x, iy, 180, 20, 0xFF3D89D6);
                gfx_draw_string(cur_x + 15, iy + 6, label, 0xFFFFFFFF);
                
                if (click && i == 0) {
                    logs_window_open = !logs_window_open;
                    open_menu_id = -2;
                }
            } else {
                gfx_draw_string(cur_x + 15, iy + 6, label, 0xFF444444);
            }
        }
    }
    cur_x += w;

    // "Help" menu
    w = measure_text_width("Help") + 20;
    
    gfx_draw_string(cur_x + 10, 8, "Help", 0xFF444444);
    gfx_draw_string(cur_x + 11, 8, "Help", 0xFF444444);
    
    if (mx >= cur_x && mx < cur_x + w && my < HEADER_HEIGHT) {
        if(click) target_menu = 1;
    }

    if (open_menu_id == 1) {
        gfx_fill_rect(cur_x, 0, w, HEADER_HEIGHT, 0xFF3D89D6);
        gfx_draw_string(cur_x + 10, 8, "Help", 0xFFFFFFFF);
        int menu_y = HEADER_HEIGHT;
        gfx_fill_rect(cur_x, menu_y, 160, 46, 0xF2F2F2F2);
        gfx_draw_rect(cur_x, menu_y, 160, 46, 0xFF888888);
        gfx_draw_string(cur_x + 10, menu_y + 10, "Installation Guide", 0xFF444444);
        gfx_draw_string(cur_x + 10, menu_y + 30, "System Requirements", 0xFF444444);
    }

    if (click && target_menu != -3) {
        if (open_menu_id == target_menu) {
            open_menu_id = -2;
        } else {
            open_menu_id = target_menu;
        }
        return 1;
    }

    if (click && open_menu_id != -2 && !(mx < cur_x && my < HEADER_HEIGHT)) {
        open_menu_id = -2;
    }

    return 0;
}

// Log window state
int log_window_dragging = 0;
int log_window_drag_x = 0;
int log_window_drag_y = 0;
int log_window_x = (WIN_W - 600) / 2;
int log_window_y = (WIN_H - 300) / 2;

void render_logs_window() {
    if (!logs_window_open) return;
    
    int win_w = 600;
    int win_h = 300;
    
    // Handle window dragging
    if (log_window_dragging) {
        log_window_x += mx - log_window_drag_x;
        log_window_y += my - log_window_drag_y;
        log_window_drag_x = mx;
        log_window_drag_y = my;
        
        // Keep window within bounds
        if (log_window_x < 0) log_window_x = 0;
        if (log_window_y < 0) log_window_y = 0;
        if (log_window_x + win_w > WIN_W) log_window_x = WIN_W - win_w;
        if (log_window_y + win_h > WIN_H) log_window_y = WIN_H - win_h;
    }
    
    int win_x = log_window_x;
    int win_y = log_window_y;
    
    // Window shadow
    gfx_fill_rounded_rect(win_x + 2, win_y + 2, win_w, win_h, 0x40000000, 8);
    
    // Window background
    gfx_fill_rounded_rect(win_x, win_y, win_w, win_h, 0xFFFFFFFF, 8);
    gfx_draw_rect(win_x, win_y, win_w, win_h, C_BORDER);
    
    // Title bar
    gfx_fill_rect(win_x, win_y, win_w, 30, C_SIDEBAR);
    gfx_draw_rect(win_x, win_y, win_w, 30, C_BORDER);
    gfx_draw_string_scaled(win_x + 10, win_y + 8, "Installer Logs", C_TEXT_DARK, 1);
    
    // Close button
    int close_x = win_x + win_w - 25;
    int close_y = win_y + 5;
    gfx_fill_rounded_rect(close_x, close_y, 18, 18, C_DANGER, 3);
    gfx_draw_string(close_x + 4, close_y + 2, "×", 0xFFFFFFFF);
    
    if (mx >= close_x && mx < close_x + 18 && my >= close_y && my < close_y + 18 && mb_left && !mb_prev) {
        logs_window_open = 0;
    }
    
    // Dragging handle
    if (mx >= win_x && mx < win_x + win_w && my >= win_y && my < win_y + 30) {
        if (mb_left && !mb_prev) {
            log_window_dragging = 1;
            log_window_drag_x = mx;
            log_window_drag_y = my;
        }
    }
    
    if (!mb_left) {
        log_window_dragging = 0;
    }
    
    // Logs area
    int log_y = win_y + 40;
    int log_x = win_x + 10;
    int log_w = win_w - 20;
    int log_h = win_h - 60;
    
    gfx_fill_rect(log_x, log_y, log_w, log_h, C_BG);
    gfx_draw_rect(log_x, log_y, log_w, log_h, C_BORDER);
    
    // Draw logs with improved readability
    int line_y = log_y + 5;
    char* log_ptr = install_log;
    while (*log_ptr && line_y < log_y + log_h - 16) {
        char* end_line = strchr(log_ptr, '\n');
        if (end_line) {
            int line_len = end_line - log_ptr;
            char line[128];
            strncpy(line, log_ptr, line_len);
            line[line_len] = '\0';
            gfx_draw_string(log_x + 5, line_y, line, 0xFF000000); // Black for better contrast
            line_y += 16;
            log_ptr = end_line + 1;
        } else {
            gfx_draw_string(log_x + 5, line_y, log_ptr, 0xFF000000); // Black for better contrast
            break;
        }
    }
    
    // Scroll bar (visual indicator)
    int total_lines = log_line_count;
    int visible_lines = (log_h - 10) / 16;
    
    if (total_lines > visible_lines) {
        int scroll_h = (visible_lines * log_h) / total_lines;
        if (scroll_h < 20) scroll_h = 20;
        
        int scroll_y = log_y + ((total_lines - visible_lines) * log_h) / total_lines;
        gfx_fill_rect(win_x + win_w - 15, scroll_y, 10, scroll_h, C_SIDEBAR);
    }
}

void draw_centered_text(int y, const char* str, int scale, uint32_t color) {
    int w = strlen(str) * 8 * scale;
    gfx_draw_string_scaled((WIN_W - w) / 2, y, str, color, scale);
}

int ui_button(int x, int y, int w, int h, const char* label, uint32_t color) {
    if (modal_active) return 0; // Block input if modal
    
    int hover = (mx >= x && mx <= x+w && my >= y && my <= y+h);
    int pressed = (hover && mb_left);
    uint32_t bg = color;
    
    if (hover) {
        // Darken for hover effect
        uint32_t r = (bg>>16)&0xFF; uint32_t g = (bg>>8)&0xFF; uint32_t b = bg&0xFF;
        if(r>20)r-=20; if(g>20)g-=20; if(b>20)b-=20;
        bg = 0xFF000000|(r<<16)|(g<<8)|b;
    }

    // Shadow
    gfx_fill_rounded_rect(x+2, y+3, w, h, C_SHADOW, 10);
    
    // Button background
    gfx_fill_rounded_rect(x, y, w, h, bg, 10);
    
    // Pressed effect
    if (pressed) {
        gfx_draw_rect(x, y, w, h, C_BORDER);
    }
    
    // Button text
    int tlen = strlen(label) * 8;
    uint32_t tcol = (color == C_WHITE || color == C_BG) ? C_TEXT_DARK : C_WHITE;
    int tx = x + (w-tlen)/2;
    int ty = y + (h-16)/2 + (pressed ? 1 : 0);
    gfx_draw_string(tx, ty, label, tcol);
    
    return (hover && mb_left && !mb_prev);
}

void render_modal() {
    if (!modal_active) return;
    
    // Check if it's format modal
    int is_format_modal = strcmp(modal_title, "Format Partition") == 0;
    int box_w = is_format_modal ? 400 : 400;
    int box_h = is_format_modal ? 350 : 200;
    
    // Dim Background
    gfx_fill_rect(0, 0, WIN_W, WIN_H, C_MODAL_DIM);
    
    // Box
    int bx = (WIN_W - box_w)/2;
    int by = (WIN_H - box_h)/2;
    
    gfx_fill_rounded_rect(bx, by, box_w, box_h, C_WHITE, 12);
    
    gfx_draw_string_scaled(bx + 20, by + 20, modal_title, C_TEXT_DARK, 2);
    gfx_draw_string(bx + 20, by + 60, modal_msg, C_TEXT_MUTED);
    
    if (is_format_modal) {
        // Format options
        int opt_y = by + 90;
        int opt_w = 360;
        int opt_h = 35;
        
        // PFS32
        int pfs_hov = (mx >= bx+20 && mx <= bx+20+opt_w && my >= opt_y && my <= opt_y+opt_h);
        gfx_fill_rounded_rect(bx+20, opt_y, opt_w, opt_h, pfs_hov ? C_ACCENT_HOVER : C_BG, 6);
        gfx_draw_string(bx+30, opt_y+10, "PFS32 (Camel OS Native)", C_TEXT_DARK);
        if (pfs_hov && mb_left && !mb_prev) {
            action_format_partition(0x7F);
        }
        opt_y += 45;
        
        // NTFS
        int ntfs_hov = (mx >= bx+20 && mx <= bx+20+opt_w && my >= opt_y && my <= opt_y+opt_h);
        gfx_fill_rounded_rect(bx+20, opt_y, opt_w, opt_h, ntfs_hov ? C_ACCENT_HOVER : C_BG, 6);
        gfx_draw_string(bx+30, opt_y+10, "NTFS (Windows)", C_TEXT_DARK);
        if (ntfs_hov && mb_left && !mb_prev) {
            action_format_partition(0x07);
        }
        opt_y += 45;
        
        // FAT32
        int fat_hov = (mx >= bx+20 && mx <= bx+20+opt_w && my >= opt_y && my <= opt_y+opt_h);
        gfx_fill_rounded_rect(bx+20, opt_y, opt_w, opt_h, fat_hov ? C_ACCENT_HOVER : C_BG, 6);
        gfx_draw_string(bx+30, opt_y+10, "FAT32 (Compatibility)", C_TEXT_DARK);
        if (fat_hov && mb_left && !mb_prev) {
            action_format_partition(0x0B);
        }
        opt_y += 45;
        
        // EXT4
        int ext_hov = (mx >= bx+20 && mx <= bx+20+opt_w && my >= opt_y && my <= opt_y+opt_h);
        gfx_fill_rounded_rect(bx+20, opt_y, opt_w, opt_h, ext_hov ? C_ACCENT_HOVER : C_BG, 6);
        gfx_draw_string(bx+30, opt_y+10, "EXT4 (Linux)", C_TEXT_DARK);
        if (ext_hov && mb_left && !mb_prev) {
            action_format_partition(0x83);
        }
        opt_y += 45;
        
        // RAW
        int raw_hov = (mx >= bx+20 && mx <= bx+20+opt_w && my >= opt_y && my <= opt_y+opt_h);
        gfx_fill_rounded_rect(bx+20, opt_y, opt_w, opt_h, raw_hov ? C_ACCENT_HOVER : C_BG, 6);
        gfx_draw_string(bx+30, opt_y+10, "RAW (Unformatted)", C_TEXT_DARK);
        if (raw_hov && mb_left && !mb_prev) {
            action_format_partition(0xFF);
        }
    }
    
    // Cancel button (always at bottom)
    int cancel_y = by + box_h - 60;
    if (mx >= bx+20 && mx <= bx+120 && my >= cancel_y && my <= cancel_y+40 && mb_left && !mb_prev) {
        modal_active = 0;
    }
    gfx_fill_rounded_rect(bx+20, cancel_y, 100, 40, C_SIDEBAR, 6);
    gfx_draw_string(bx+45, cancel_y+12, "Cancel", C_TEXT_DARK);
    
    // Action button (only if not format modal)
    if (!is_format_modal) {
        int action_y = by + box_h - 60;
        int hov = (mx >= bx+260 && mx <= bx+380 && my >= action_y && my <= action_y+40);
        gfx_fill_rounded_rect(bx+260, action_y, 120, 40, hov ? C_ACCENT_HOVER : C_ACCENT, 6);
        gfx_draw_string(bx+285, action_y+12, modal_action_label, C_WHITE);
        
        if (hov && mb_left && !mb_prev) {
            if(modal_callback) modal_callback();
        }
    }
}

// --- Screens ---

void render_welcome() {
    // macOS X style welcome screen with gradient background
    for(int y=0; y<WIN_H; y++) {
        // Gradient from light blue to white
        uint8_t blend = (y * 255 / WIN_H);
        uint8_t r = 0xF2 + ((0xFF - 0xF2) * blend / 255);
        uint8_t g = 0xF2 + ((0xFF - 0xF2) * blend / 255);
        uint8_t b = 0xF7 + ((0xFF - 0xF7) * blend / 255);
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        gfx_fill_rect(0, y, WIN_W, 1, col);
    }
    
    // Logo or icon - use HDD icon from assets
    const embedded_image_t* images;
    uint32_t image_count;
    images = get_embedded_images(&image_count);
    
    // Find HDD icon
    const embedded_image_t* hdd_icon = 0;
    for(uint32_t i=0; i<image_count; i++) {
        if(strcmp(images[i].name, "hdd_icon") == 0) {
            hdd_icon = &images[i];
            break;
        }
    }
    
    // Icon with shadow
    gfx_fill_rounded_rect(CX - 60, CY - 180, 120, 120, C_SHADOW, 20);
    
    if(hdd_icon) {
        int icon_x = CX - hdd_icon->width/2;
        int icon_y = CY - 170;
        gfx_draw_asset_scaled(0, icon_x, icon_y, hdd_icon->data, hdd_icon->width, hdd_icon->height, 
                             hdd_icon->width, hdd_icon->height);
    } else {
        // Fallback if icon not found
        gfx_fill_rounded_rect(CX - 50, CY - 170, 100, 100, C_WHITE, 20);
        gfx_draw_string(CX - 35, CY - 130, "Camel", C_ACCENT);
    }
    
    // Title with shadow
    gfx_draw_string_scaled(CX - 70 + 2, CY - 50 + 2, "Camel OS", C_SHADOW, 3);
    draw_centered_text(CY - 50, "Camel OS", 3, C_TEXT_DARK);
    
    // Subtitle
    draw_centered_text(CY, "Welcome to the installation assistant", 1, C_TEXT_MUTED);
    
    // Feature highlights
    int feat_y = CY + 50;
    gfx_draw_string(CX - 200, feat_y, "Fast and lightweight operating system", C_TEXT_MUTED);
    gfx_draw_string(CX - 200, feat_y + 20, "Built-in applications and utilities", C_TEXT_MUTED);
    gfx_draw_string(CX - 200, feat_y + 40, "Modern graphical interface", C_TEXT_MUTED);
    
    // Buttons with enhanced styling
    if (ui_button(CX - 220, CY + 130, 210, 55, "Install System", C_ACCENT)) {
        scan_hardware();
        current_state = STATE_SELECT_DISK;
    }
    
    if (ui_button(CX + 10, CY + 130, 210, 55, "Disk Utility", C_WHITE)) {
        scan_hardware();
        current_state = STATE_DISK_UTIL;
    }
}

void render_disk_utility() {
    // Sidebar - macOS X style
    gfx_fill_rect(0, 0, 280, WIN_H, C_SIDEBAR);
    gfx_draw_rect(0, 0, 280, WIN_H, C_BORDER);
    gfx_draw_string_scaled(20, 20, "DISK UTILITY", C_TEXT_MUTED, 1);
    
    int y = 70;
    for (int i=0; i<2; i++) {
        int active = (util_drive_idx == i);
        uint32_t bg = active ? C_ACCENT : C_SIDEBAR;
        uint32_t fg = active ? C_WHITE : C_TEXT_DARK;
        
        if (active) {
            gfx_fill_rounded_rect(10, y, 260, 45, bg, 8);
        } else {
            gfx_fill_rounded_rect(10, y, 260, 45, C_BG, 8);
            gfx_draw_rect(10, y, 260, 45, C_BORDER);
        }
        
        if (ide_devices[i].present) {
            char device_info[64];
            char size_str[32];
            format_size(ide_devices[i].sectors, size_str);
            strcpy(device_info, (i==0)?"Internal Disk 0": "Internal Disk 1");
            strcat(device_info, " (");
            strcat(device_info, size_str);
            strcat(device_info, ")");
            gfx_draw_string(40, y+15, device_info, fg);
        } else {
            gfx_draw_string(40, y+15, "Empty Bay", C_TEXT_MUTED);
        }
        
        if (!modal_active && mx < 280 && my >= y && my < y+45 && mb_left && ide_devices[i].present) {
            util_drive_idx = i;
            util_part_idx = -1; // Reset part selection
        }
        y += 60;
    }
    
    if (ui_button(20, WIN_H - 80, 240, 45, "Back to Menu", C_WHITE)) current_state = STATE_WELCOME;

    // Main Content - macOS X inspired layout
    int mx_off = 320;
    if (ide_devices[util_drive_idx].present) {
        ide_device_t* dev = &ide_devices[util_drive_idx];
        char sz[32]; format_size(dev->sectors, sz);
        
        gfx_draw_string_scaled(mx_off, 60, "Drive Information", C_TEXT_DARK, 2);
        
        // Info Box - Enhanced with shadow
        gfx_fill_rounded_rect(mx_off+2, 110+2, 600, 120, C_SHADOW, 12);
        gfx_fill_rounded_rect(mx_off, 110, 600, 120, C_WHITE, 12);
        gfx_draw_rect(mx_off, 110, 600, 120, C_BORDER);
        
        // Drive icon
        gfx_fill_rounded_rect(mx_off+20, 120, 60, 60, C_SIDEBAR, 10);
        gfx_draw_string(mx_off+32, 138, "HDD", C_TEXT_DARK);
        
        // Drive details
        gfx_draw_string(mx_off+90, 130, "Model:", C_TEXT_MUTED);
        gfx_draw_string(mx_off+170, 130, dev->model, C_TEXT_DARK);
        gfx_draw_string(mx_off+90, 155, "Capacity:", C_TEXT_MUTED);
        gfx_draw_string(mx_off+170, 155, sz, C_TEXT_DARK);
        
        // Scheme Detect
        char scheme[32];
        if (disk_has_mbr[util_drive_idx]) strcpy(scheme, "Master Boot Record");
        else strcpy(scheme, "Uninitialized (Raw)");
        gfx_draw_string(mx_off+90, 180, "Scheme:", C_TEXT_MUTED);
        gfx_draw_string(mx_off+170, 180, scheme, C_ACCENT);

        // Partition Map Visualizer - Enhanced
        int vis_y = 280;
        gfx_draw_string_scaled(mx_off, vis_y, "Partition Map", C_TEXT_DARK, 1);
        
        int bar_w = 600;
        int bar_h = 70;
        int bar_y = vis_y + 40;
        
        gfx_fill_rounded_rect(mx_off+2, bar_y+2, bar_w, bar_h, C_SHADOW, 10);
        gfx_fill_rounded_rect(mx_off, bar_y, bar_w, bar_h, C_WHITE, 10);
        gfx_draw_rect(mx_off, bar_y, bar_w, bar_h, C_BORDER);
        
        if (disk_has_mbr[util_drive_idx]) {
            uint32_t total = dev->sectors;
            int px = mx_off + 5;
            
            for(int k=0; k<4; k++) {
                mbr_entry_t* part = &disk_mbr[util_drive_idx].partitions[k];
                if (part->type == 0) continue;
                
                int pw = (int)((uint64_t)part->lba_length * (bar_w - 10) / total);
                if (pw < 5) pw = 5; // Min visibility
                
                uint32_t col = (part->type == 0x7F) ? C_PART_CAMEL : C_PART_OTHER;
                if (util_part_idx == k) col = C_ACCENT_HOVER; // Highlight selected
                
                gfx_fill_rounded_rect(px, bar_y + 5, pw, bar_h - 10, col, 6);
                
                // Click detect
                if (!modal_active && mx >= px && mx < px+pw && my >= bar_y && my <= bar_y+bar_h && mb_left) {
                    util_part_idx = k;
                }
                px += pw;
            }
            // Fill rest
            if (px < mx_off + bar_w - 5) {
                gfx_fill_rounded_rect(px, bar_y + 5, (mx_off+bar_w-5)-px, bar_h - 10, C_PART_FREE, 6);
            }
        } else {
            gfx_fill_rounded_rect(mx_off+5, bar_y+5, bar_w-10, bar_h-10, C_PART_FREE, 6);
            gfx_draw_string(mx_off + 250, bar_y + 28, "Unallocated", C_TEXT_MUTED);
        }
        
        // Controls - Enhanced with better layout
        int ctrl_y = bar_y + 120;
        
        if (disk_has_mbr[util_drive_idx]) {
            if (util_part_idx != -1) {
                char lbl[64];
                char type_name[32];
                get_part_type_name(disk_mbr[util_drive_idx].partitions[util_part_idx].type, type_name);
                strcpy(lbl, "Selected: Partition "); 
                char num[2]; int_to_str(util_part_idx+1, num); strcat(lbl, num);
                strcat(lbl, " ("); strcat(lbl, type_name); strcat(lbl, ")");
                
                gfx_draw_string_scaled(mx_off, ctrl_y, lbl, C_TEXT_DARK, 1);
                
                if (ui_button(mx_off, ctrl_y + 40, 150, 45, "Delete", C_DANGER)) {
                    show_modal("Confirm Delete", "This will permanently erase the partition.", "Delete", action_delete_partition);
                }
                
                if (ui_button(mx_off + 170, ctrl_y + 40, 190, 45, "Format", C_ACCENT)) {
                    // Format options modal
                    strcpy(modal_title, "Format Partition");
                    strcpy(modal_msg, "Select filesystem type:");
                    strcpy(modal_action_label, "PFS32");
                    modal_active = 1;
                    modal_callback = 0; // We'll handle formatting directly in modal
                }
            }
            if (ui_button(mx_off + 480, ctrl_y + 40, 170, 45, "Wipe Disk", C_DANGER)) {
                show_modal("Erase Entire Disk", "All data and partitions will be lost.", "Erase", action_erase_disk);
            }
        } else {
            gfx_draw_string(mx_off, ctrl_y, "Disk is uninitialized.", C_TEXT_MUTED);
            if (ui_button(mx_off, ctrl_y + 40, 220, 45, "Initialize (MBR)", C_ACCENT)) {
                action_create_schema();
                add_log("Initialized disk with MBR partition table");
            }
        }
        
        // Disk usage statistics
        if (disk_has_mbr[util_drive_idx]) {
            uint32_t used = 0;
            for(int k=0; k<4; k++) {
                mbr_entry_t* part = &disk_mbr[util_drive_idx].partitions[k];
                if (part->type != 0) {
                    used += part->lba_length;
                }
            }
            char used_str[32], free_str[32];
            format_size(used, used_str);
            format_size(dev->sectors - used, free_str);
            
            int stats_y = ctrl_y + 110;
            gfx_draw_string(mx_off, stats_y, "Disk Usage:", C_TEXT_MUTED);
            gfx_draw_string(mx_off + 120, stats_y, used_str, C_TEXT_DARK);
            gfx_draw_string(mx_off + 200, stats_y, "used,", C_TEXT_MUTED);
            gfx_draw_string(mx_off + 250, stats_y, free_str, C_TEXT_DARK);
            gfx_draw_string(mx_off + 330, stats_y, "free", C_TEXT_MUTED);
        }
    }
}

void render_select_disk() {
    // Header with gradient
    for(int y=0; y<60; y++) {
        uint8_t intensity = 240 - (y * 20 / 60);
        uint32_t col = 0xFF000000 | (intensity << 16) | (intensity << 8) | intensity;
        gfx_fill_rect(0, y, WIN_W, 1, col);
    }
    draw_centered_text(30, "Select Installation Destination", 2, C_TEXT_DARK);
    
    // Warning box
    gfx_fill_rounded_rect(CX - 350, 80, 700, 50, 0xFFFFF3CD, 8);
    gfx_draw_rect(CX - 350, 80, 700, 50, 0xFFFFCA28);
    gfx_draw_string(CX - 330, 95, "Warning: All data on the selected drive will be erased!", 0xFF856404);
    
    int y = 160;
    for (int i=0; i<2; i++) {
        int hover = (mx >= CX-300 && mx <= CX+300 && my >= y && my < y+100);
        int selected = (selected_drive_idx == i);
        
        uint32_t bg = selected ? 0xFFE3F2FD : C_WHITE;
        uint32_t border = selected ? C_ACCENT : (hover ? C_TEXT_MUTED : C_BORDER);
        
        // Shadow effect
        gfx_fill_rounded_rect(CX-300+2, y+2, 600, 100, C_SHADOW, 12);
        gfx_fill_rounded_rect(CX-300, y, 600, 100, bg, 12);
        gfx_draw_rect(CX-300, y, 600, 100, border);
        
        // Icon - macOS X style
        gfx_fill_rounded_rect(CX-270, y+20, 60, 60, C_SIDEBAR, 10);
        gfx_draw_string(CX-250, y+40, "HDD", C_TEXT_DARK);
        
        if (ide_devices[i].present) {
            gfx_draw_string(CX-190, y+25, (i==0)?"Internal Drive 0":"Internal Drive 1", C_TEXT_DARK);
            char sz[32]; format_size(ide_devices[i].sectors, sz);
            gfx_draw_string(CX-190, y+50, sz, C_TEXT_MUTED);
            
            // Show partition info
            if (disk_has_mbr[i]) {
                int part_count = 0;
                for(int p=0; p<4; p++) {
                    if (disk_mbr[i].partitions[p].type != 0) part_count++;
                }
                char part_info[32];
                strcpy(part_info, part_count > 0 ? "Has partitions" : "Empty MBR");
                gfx_draw_string(CX-190, y+70, part_info, C_TEXT_MUTED);
            } else {
                gfx_draw_string(CX-190, y+70, "Uninitialized", C_DANGER);
            }
            
            if (hover && mb_left && !mb_prev) {
                selected_drive_idx = i;
            }
        } else {
            gfx_draw_string(CX-190, y+40, "Empty Slot", C_TEXT_MUTED);
            gfx_draw_string(CX-190, y+60, "No drive detected", C_TEXT_MUTED);
        }
        y += 120;
    }
    
    // Navigation buttons
    if (ui_button(CX-250, WIN_H-80, 200, 50, "< Back", C_WHITE)) current_state = STATE_WELCOME;
    
    if (selected_drive_idx != -1 && ide_devices[selected_drive_idx].present) {
        // Size Check (>100MB)
        uint32_t caps = ide_devices[selected_drive_idx].sectors;
        if (caps < 204800) { // 204800 sectors * 512 = 100MB
            gfx_fill_rounded_rect(CX + 50, WIN_H - 85, 300, 40, 0xFFFFEBEE, 8);
            gfx_draw_rect(CX + 50, WIN_H - 85, 300, 40, C_DANGER);
            gfx_draw_string(CX + 70, WIN_H - 75, "Disk too small (<100MB)", C_DANGER);
        } else {
            // Show install size requirement
            char req[64];
            strcpy(req, "Requires ~8MB minimum");
            gfx_draw_string(CX + 50, WIN_H - 85, req, C_TEXT_MUTED);
            
            if (ui_button(CX+50, WIN_H-80, 200, 50, "Install >", C_ACCENT)) {
                // Reset installation state
                install_step = 0;
                install_sub_step = 0;
                install_file_idx = 0;
                install_error = 0;
                install_error_msg[0] = 0;
                kernel_write_offset = 0;
                install_pct = 0;
                current_state = STATE_INSTALLING;
                add_log("Starting installation process");
            }
        }
    } else {
        // No disk selected message
        gfx_fill_rounded_rect(CX + 50, WIN_H - 85, 300, 40, 0xFFF5F5F5, 8);
        gfx_draw_string(CX + 70, WIN_H - 75, "Select a drive to continue", C_TEXT_MUTED);
    }
}

// --- Installation Logic ---

int install_file(const char* path, uint8_t* start, uint8_t* end) {
    uint32_t size = (uint32_t)(end - start);
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "Installing %s (%u bytes)", path, size);
    add_log(log_buf);
    
    int create_res = pfs32_create_file(path);
    if (create_res != 0 && create_res != -5) { // -5 is PFS_ERR_EXISTS
        snprintf(log_buf, sizeof(log_buf), "ERROR: Failed to create %s: %d", path, create_res);
        add_log(log_buf);
        return -1;
    }
    
    int write_res = pfs32_write_file(path, start, size);
    if (write_res < 0) {
        snprintf(log_buf, sizeof(log_buf), "ERROR: Failed to write %s: %d", path, write_res);
        add_log(log_buf);
        return -2;
    }
    
    return 0;
}

// File installation list
typedef struct {
    const char* path;
    uint8_t* start;
    uint8_t* end;
} install_file_entry_t;

static install_file_entry_t install_files[] = {
    {"/usr/lib/math.cdl", 0, 0},
    {"/usr/lib/usr32.cdl", 0, 0},
    {"/usr/lib/syskernel.cdl", 0, 0},
    {"/usr/lib/proc.cdl", 0, 0},
    {"/usr/lib/timer.cdl", 0, 0},
    {"/usr/lib/gui.cdl", 0, 0},
    {"/usr/lib/sysmon.cdl", 0, 0},
    {"/usr/apps/Terminal.cdl", 0, 0},
    {"/usr/apps/Files.cdl", 0, 0},
    {"/usr/apps/Waterhole.cdl", 0, 0},
    {"/usr/apps/NetTools.cdl", 0, 0},
    {"/usr/apps/TextEdit.cdl", 0, 0},
    {"/usr/apps/Browser.cdl", 0, 0},
    {0, 0, 0} // Sentinel
};

void init_install_files() {
    // Initialize file pointers
    install_files[0].start = lib_math_start; install_files[0].end = lib_math_end;
    install_files[1].start = lib_usr32_start; install_files[1].end = lib_usr32_end;
    install_files[2].start = lib_syskernel_start; install_files[2].end = lib_syskernel_end;
    install_files[3].start = lib_proc_start; install_files[3].end = lib_proc_end;
    install_files[4].start = lib_timer_start; install_files[4].end = lib_timer_end;
    install_files[5].start = lib_gui_start; install_files[5].end = lib_gui_end;
    install_files[6].start = lib_sysmon_start; install_files[6].end = lib_sysmon_end;
    install_files[7].start = app_terminal_start; install_files[7].end = app_terminal_end;
    install_files[8].start = app_files_start; install_files[8].end = app_files_end;
    install_files[9].start = app_waterhole_start; install_files[9].end = app_waterhole_end;
    install_files[10].start = app_nettools_start; install_files[10].end = app_nettools_end;
    install_files[11].start = app_textedit_start; install_files[11].end = app_textedit_end;
    install_files[12].start = app_browser_start; install_files[12].end = app_browser_end;
}

void install_tick() {
    disk_set_drive(selected_drive_idx);
    
    // Check for previous error
    if (install_error) {
        current_state = STATE_FAILURE;
        return;
    }

    if (install_step == 0) {
        strcpy(install_status, "Writing Bootloader & Tables...");
        add_log("Writing bootloader and partition tables");
        
        // 1. Wipe MBR first
        uint8_t z[512]; memset(z, 0, 512);
        if (ata_write_sector(selected_drive_idx, 0, z) < 0) {
            strcpy(install_error_msg, "Failed to wipe MBR");
            install_error = 1;
            add_log("ERROR: Failed to wipe MBR sector");
            return;
        }
        
        // 2. Create MBR with System Partition
        mbr_sector_t mbr;
        memcpy(&mbr, mbr_bin_start, 512); // Copy boot code
        
        uint32_t total = ide_devices[selected_drive_idx].sectors;
        // Start after kernel space (approx 8MB reserved for raw kernel image)
        uint32_t part_start = 16384; 
        
        mbr.partitions[0].status = 0x80;
        mbr.partitions[0].type = 0x7F; // Camel Type
        mbr.partitions[0].lba_start = part_start;
        mbr.partitions[0].lba_length = total - part_start;
        mbr.signature = 0xAA55;
        
        if (ata_write_sector(selected_drive_idx, 0, (uint8_t*)&mbr) < 0) {
            strcpy(install_error_msg, "Failed to write MBR");
            install_error = 1;
            add_log("ERROR: Failed to write MBR");
            return;
        }
        
        install_pct = 5;
        install_step++;
        return;
    }
    
    if (install_step == 1) {
        // Non-blocking kernel copy - write small chunks per tick
        strcpy(install_status, "Copying Kernel Image...");
        
        // Write raw kernel to sectors 1 -> 16383
        uint32_t k_size = system_bin_end - system_bin_start;
        uint32_t k_sectors = (k_size + 511) / 512;
        
        // Write only 16 sectors per tick to prevent freezing
        int sectors_this_tick = 0;
        while (kernel_write_offset < k_sectors && sectors_this_tick < 16) {
            uint8_t buf[512]; memset(buf, 0, 512);
            uint32_t rem = k_size - (kernel_write_offset * 512);
            memcpy(buf, system_bin_start + (kernel_write_offset * 512), (rem > 512) ? 512 : rem);
            
            if (ata_write_sector(selected_drive_idx, 1 + kernel_write_offset, buf) < 0) {
                strcpy(install_error_msg, "Failed to write kernel sector");
                install_error = 1;
                add_log("ERROR: Failed to write kernel sector");
                return;
            }
            
            kernel_write_offset++;
            sectors_this_tick++;
        }
        
        // Update progress
        install_pct = 5 + (kernel_write_offset * 25 / k_sectors);
        
        if (kernel_write_offset >= k_sectors) {
            install_step++;
            install_pct = 30;
            add_log("Kernel copy complete");
        }
        return;
    }
    
    if (install_step == 2) {
        strcpy(install_status, "Formatting PFS32 Partition...");
        add_log("Formatting partition with PFS32 filesystem");
        
        uint32_t part_start = 16384;
        uint32_t part_size = ide_devices[selected_drive_idx].sectors - part_start;
        
        // Initialize PFS32
        pfs32_init(part_start, part_size);
        
        // Format (this may take a moment but is necessary)
        int fmt_result = pfs32_format("Camel Sys", part_size);
        if (fmt_result < 0) {
            strcpy(install_error_msg, "Failed to format partition");
            install_error = 1;
            add_log("ERROR: PFS32 format failed");
            return;
        }
        
        install_pct = 45;
        install_step++;
        add_log("PFS32 formatting complete");
        return;
    }
    
    if (install_step == 3) {
        // Create directories (fast operation)
        if (install_sub_step == 0) {
            strcpy(install_status, "Creating Directory Structure...");
            add_log("Creating directory structure");
            
            pfs32_create_directory("/home");
            pfs32_create_directory("/home/desktop");
            pfs32_create_directory("/usr");
            pfs32_create_directory("/usr/lib");
            pfs32_create_directory("/usr/apps");
            
            install_sub_step = 1;
            init_install_files();
            install_file_idx = 0;
            return;
        }
        
        // Install files one at a time to prevent freezing
        if (install_file_idx < 13) {
            install_file_entry_t* f = &install_files[install_file_idx];
            
            if (f->path && f->start && f->end) {
                strcpy(install_status, "Installing: ");
                strcat(install_status, f->path);
                
                int result = install_file(f->path, f->start, f->end);
                if (result < 0) {
                    strcpy(install_error_msg, "Failed to install: ");
                    strcat(install_error_msg, f->path);
                    install_error = 1;
                    return;
                }
            }
            
            install_file_idx++;
            // Update progress: 45% to 90% for file installation
            install_pct = 45 + (install_file_idx * 45) / 13;
            return;
        }
        
        // All files installed
        install_pct = 90;
        install_step++;
        install_sub_step = 0;
        add_log("System files expanded successfully");
        return;
    }
    
    if (install_step == 4) {
        strcpy(install_status, "Finalizing Installation...");
        add_log("Syncing filesystem");
        pfs32_sync();
        install_pct = 100;
        current_state = STATE_SUCCESS;
        add_log("Installation complete!");
    }
}

void render_installing() {
    // Simple tick counter using a static variable
    static uint32_t anim_counter = 0;
    anim_counter++;
    
    // Draw animated header
    for(int y=0; y<80; y++) {
        uint8_t intensity = 200 + (y * 55 / 80);
        uint32_t col = 0xFF000000 | (intensity << 8) | 0x007AFF;
        gfx_fill_rect(0, y, WIN_W, 1, col);
    }
    
    // Animated spinner
    const char* spinner_frames = "|/-\\";
    char spinner[2] = {spinner_frames[(anim_counter / 4) % 4], 0};
    
    // Title with spinner
    char title[64];
    strcpy(title, "Installing Camel OS... ");
    strcat(title, spinner);
    draw_centered_text(CY - 100, title, 2, C_TEXT_DARK);
    
    int bar_w = 550;
    int bar_h = 24;
    int bx = CX - bar_w/2;
    int by = CY;
    
    // Progress bar container with shadow
    gfx_fill_rounded_rect(bx+2, by+2, bar_w, bar_h, C_SHADOW, 10);
    gfx_fill_rounded_rect(bx, by, bar_w, bar_h, C_WHITE, 10);
    gfx_draw_rect(bx, by, bar_w, bar_h, C_BORDER);
    
    // Progress fill
    int fill = (bar_w * install_pct) / 100;
    if (fill > 0) {
        gfx_fill_rounded_rect(bx+2, by+2, fill, bar_h-4, C_ACCENT, 8);
    }
    
    // Progress percentage text with shadow
    char pct_str[16];
    int_to_str(install_pct, pct_str);
    strcat(pct_str, "%");
    gfx_draw_string_scaled(CX - strlen(pct_str)*4 + 1, by + bar_h + 21, pct_str, C_SHADOW, 1);
    gfx_draw_string_scaled(CX - strlen(pct_str)*4, by + bar_h + 20, pct_str, C_TEXT_DARK, 1);
    
    // Status text with icon
    int status_y = CY + 80;
    gfx_draw_string(CX - 200, status_y, "Status:", C_TEXT_MUTED);
    gfx_draw_string(CX - 140, status_y, install_status, C_TEXT_DARK);
    
    // Step indicator
    char step_str[64];
    strcpy(step_str, "Step ");
    char num[8];
    int_to_str(install_step + 1, num);
    strcat(step_str, num);
    strcat(step_str, " of 5");
    gfx_draw_string(CX + 100, status_y, step_str, C_TEXT_MUTED);
    
    // Animated dots at bottom
    int dots_y = WIN_H - 60;
    for(int i=0; i<5; i++) {
        int dot_x = CX - 40 + i * 20;
        int active = ((anim_counter / 8) % 5) == i;
        uint32_t dot_col = active ? C_ACCENT : C_BORDER;
        gfx_fill_rounded_rect(dot_x, dots_y, 10, 10, dot_col, 5);
    }
    
    // Tip text
    draw_centered_text(WIN_H - 30, "Please wait, this may take a few minutes...", 1, C_TEXT_MUTED);
    
    // Process one tick of installation
    install_tick();
}

void render_success() {
    // Success gradient background
    for(int y=0; y<WIN_H; y++) {
        uint8_t g = 0xC7 + (y * 0x38 / WIN_H);
        uint32_t col = 0xFF000000 | (0x34 << 16) | (g << 8) | 0x59;
        gfx_fill_rect(0, y, WIN_W, 1, col);
    }
    
    // Success checkmark icon
    gfx_fill_rounded_rect(CX - 50, CY - 130, 100, 100, 0xFFFFFFFF, 50);
    gfx_draw_string_scaled(CX - 30, CY - 110, "OK", C_PART_SYS, 3);
    
    draw_centered_text(CY - 10, "Installation Complete!", 2, C_WHITE);
    draw_centered_text(CY + 40, "Camel OS has been successfully installed.", 1, 0xFFFFFFFF);
    draw_centered_text(CY + 60, "Remove the installation media and restart.", 1, 0xD0FFFFFF);
    
    // Summary box
    gfx_fill_rounded_rect(CX - 200, CY + 90, 400, 60, 0x40FFFFFF, 8);
    gfx_draw_string(CX - 180, CY + 100, "Installed to:", C_WHITE);
    gfx_draw_string(CX - 80, CY + 100, selected_drive_idx == 0 ? "Drive 0" : "Drive 1", 0xFFFFFFFF);
    gfx_draw_string(CX - 180, CY + 120, "Filesystem:", C_WHITE);
    gfx_draw_string(CX - 80, CY + 120, "PFS32", 0xFFFFFFFF);
    
    if (ui_button(CX - 100, CY + 170, 200, 50, "Restart Now", C_WHITE)) {
        outb(0x64, 0xFE);
    }
}

void render_failure() {
    // Error background
    for(int y=0; y<WIN_H; y++) {
        uint8_t r = 0xFF;
        uint8_t g = 0x30 + (y * 30 / WIN_H);
        uint8_t b = 0x40 + (y * 20 / WIN_H);
        gfx_fill_rect(0, y, WIN_W, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }
    
    // Error icon
    gfx_fill_rounded_rect(CX - 40, CY - 120, 80, 80, 0xFFFFFFFF, 40);
    gfx_draw_string_scaled(CX - 15, CY - 95, "!", C_DANGER, 4);
    
    draw_centered_text(CY - 20, "Installation Failed", 2, C_WHITE);
    
    // Show error message if available
    if (install_error_msg[0]) {
        gfx_fill_rounded_rect(CX - 250, CY + 20, 500, 40, 0x40FFFFFF, 8);
        gfx_draw_string_scaled(CX - strlen(install_error_msg)*4, CY + 32, install_error_msg, C_WHITE, 1);
    }
    
    // Show log hint
    draw_centered_text(CY + 80, "Check View > Installer Logs for details", 1, 0xFFFFFFFF);
    
    if (ui_button(CX-100, CY+120, 200, 50, "Restart", C_WHITE)) outb(0x64, 0xFE);
}

// --- Main ---

int main(uint32_t magic, void* mb_ptr) {
    uint32_t heap = (uint32_t)&_bss_end;
    if (heap%16) heap += 16 - (heap%16);
    init_heap(heap, 16*1024*1024);
    
    gfx_init_hal(mb_ptr);
    init_serial();
    
    // Initialize mouse (simple polling for compatibility)
    outb(0x64, 0xA8); outb(0x64, 0xD4); outb(0x60, 0xF4);
    
    // Initialize installation state
    install_step = 0;
    install_sub_step = 0;
    install_file_idx = 0;
    install_error = 0;
    install_error_msg[0] = 0;
    kernel_write_offset = 0;
    install_pct = 0;
    
    scan_hardware();
    
    // Add initial log entries
    add_log("Camel OS Installer started");
    add_log("Video system initialized: 1024x768");
    add_log("Mouse support enabled (polling)");
    add_log("Scanning for hardware...");
    
    // Log detected drives
    for(int i=0; i<2; i++) {
        if(ide_devices[i].present) {
            char buf[64];
            strcpy(buf, "Found drive ");
            char num[8];
            int_to_str(i, num);
            strcat(buf, num);
            strcat(buf, ": ");
            char sz[32];
            format_size(ide_devices[i].sectors, sz);
            strcat(buf, sz);
            add_log(buf);
        }
    }
    
    while(1) {
        poll_input();
        
        gfx_fill_rect(0, 0, WIN_W, WIN_H, C_BG);
        
        // Header with menu bar
        int menu_clicked = process_menu_bar(mx, my, mb_left && !mb_prev);
        
        switch(current_state) {
            case STATE_WELCOME: render_welcome(); break;
            case STATE_DISK_UTIL: render_disk_utility(); break;
            case STATE_SELECT_DISK: render_select_disk(); break;
            case STATE_INSTALLING: render_installing(); break;
            case STATE_SUCCESS: render_success(); break;
            case STATE_FAILURE: render_failure(); break;
        }
        
        render_logs_window();
        render_modal();
        draw_cursor();
        
        gfx_swap_buffers();
        mb_prev = mb_left;
    }
    return 0;
}