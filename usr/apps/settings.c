// usr/apps/settings.c - CamelOS Settings App
// System settings viewer with theme, user info, and about panel
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../dock.h"
#include "../../fs/pfs32.h"
#include "../../core/window_server.h"

// Tab IDs
#define TAB_ABOUT    0
#define TAB_USER     1
#define TAB_DISPLAY  2
#define TAB_NETWORK  3
#define TAB_HARDWARE 4
#define TAB_COUNT    5

static int current_tab = TAB_ABOUT;
static int settings_scroll_y = 0;

// Config data (loaded from system.conf)
static char cfg_username[64] = "(not set)";
static char cfg_computer[64] = "CamelOS";
static char cfg_theme[32] = "Aqua";
static char cfg_timezone[32] = "UTC";

// System info
static char sys_mem_str[32] = "";
static char sys_disk_str[32] = "";

// Hardware info (detected via CPUID)
static char hw_cpu_vendor[13] = "(unknown)";
static char hw_cpu_model[49] = "(unknown)";
static char hw_cpu_arch[16] = "i386 (32-bit)";
static char hw_mem_total_str[32] = "";
static char hw_disk_size_str[32] = "";
static char hw_disk_free_str[32] = "";
static int hw_cpu_supported = 0;

static void settings_load_config() {
    char buf[1024];
    int len = sys_fs_read("/Library/Preferences/system.conf", buf, sizeof(buf)-1);
    if (len <= 0) len = sys_fs_read("/etc/system.conf", buf, sizeof(buf)-1);
    if (len > 0) {
        buf[len] = 0;
        char* line = buf;
        while (line && *line) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            int llen = strlen(line);
            while (llen > 0 && (line[llen-1] == '\r' || line[llen-1] == ' ')) line[--llen] = 0;
            if (line[0] == '#' || line[0] == 0) { line = next; continue; }
            
            if (strncmp(line, "username=", 9) == 0) {
                strncpy(cfg_username, line + 9, sizeof(cfg_username)-1);
                cfg_username[sizeof(cfg_username)-1] = 0;
            } else if (strncmp(line, "computer=", 9) == 0) {
                strncpy(cfg_computer, line + 9, sizeof(cfg_computer)-1);
            } else if (strncmp(line, "timezone=", 9) == 0) {
                strncpy(cfg_timezone, line + 9, sizeof(cfg_timezone)-1);
            } else if (strncmp(line, "theme=", 6) == 0) {
                int idx = line[6] - '0';
                const char* names[] = {"Aqua", "Graphite", "Sunset", "Ocean", "Forest"};
                if (idx >= 0 && idx < 5) strcpy(cfg_theme, names[idx]);
            }
            line = next;
        }
    }
    
    // Get memory info
    extern uint32_t k_get_free_mem();
    uint32_t free_mem = k_get_free_mem();
    int mem_mb = free_mem / (1024 * 102);
    strcpy(sys_mem_str, "Free: ");
    char num[16];
    int_to_str(mem_mb, num);
    strcat(sys_mem_str, num);
    strcat(sys_mem_str, " MB");
    
    // Get disk info
    pfs32_stats_t stats;
    pfs32_get_stats(&stats);
    strcpy(sys_disk_str, "Reads: ");
    int_to_str(stats.disk_reads, num);
    strcat(sys_disk_str, num);
    strcat(sys_disk_str, " Writes: ");
    int_to_str(stats.disk_writes, num);
    strcat(sys_disk_str, num);
}

static void detect_cpu_info(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // CPUID leaf 0 - vendor string
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    memcpy(hw_cpu_vendor, &ebx, 4);
    memcpy(hw_cpu_vendor+4, &edx, 4);
    memcpy(hw_cpu_vendor+8, &ecx, 4);
    hw_cpu_vendor[12] = 0;
    
    // Check for supported CPU vendor
    if (strcmp(hw_cpu_vendor, "GenuineIntel") == 0 ||
        strcmp(hw_cpu_vendor, "AuthenticAMD") == 0 ||
        strcmp(hw_cpu_vendor, "CentaurHauls") == 0) {
        hw_cpu_supported = 1;
    }
    
    // CPUID leaf 0x80000000 - check extended support
    asm volatile("cpuid" : "=a"(eax) : "a"(0x80000000));
    if (eax >= 0x80000004) {
        // Get model name from leaves 0x80000002-0x80000004
        uint32_t* p = (uint32_t*)hw_cpu_model;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf));
            *p++ = eax; *p++ = ebx; *p++ = ecx; *p++ = edx;
        }
        hw_cpu_model[48] = 0;
        // Trim leading spaces
        char* s = hw_cpu_model;
        while (*s == ' ') s++;
        if (s != hw_cpu_model) memmove(hw_cpu_model, s, strlen(s)+1);
    } else {
        strcpy(hw_cpu_model, "(unknown)");
    }
    
    // Get total memory info via kernel API
    extern kernel_api_t g_kernel_api;
    uint32_t mem_bytes = g_kernel_api.mem_total();
    uint32_t mem_mb = mem_bytes / 1024 / 1024;
    if (mem_mb == 0 && mem_bytes > 0) mem_mb = 1;  // At least 1 MB if any memory
    char num[16];
    strcpy(hw_mem_total_str, "");
    int_to_str(mem_mb, num);
    strcat(hw_mem_total_str, num);
    strcat(hw_mem_total_str, " MB");
    
    // Get disk size info
    pfs32_stats_t stats;
    pfs32_get_stats(&stats);
    uint32_t disk_total_mb = stats.total_sectors_used / 2048;
    uint32_t disk_free_mb = stats.blocks_free / 2;  // approximate
    strcpy(hw_disk_size_str, "");
    int_to_str(disk_total_mb, num);
    strcat(hw_disk_size_str, num);
    strcat(hw_disk_size_str, " MB used");
    strcpy(hw_disk_free_str, "");
    int_to_str(disk_free_mb, num);
    strcat(hw_disk_free_str, num);
    strcat(hw_disk_free_str, " blocks free");
}

static void draw_tab_bar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, 28, 0xFFF2F2F7);
    gfx_draw_rect(x, y + 27, w, 1, 0xFFC6C6C8);
    
    const char* tab_names[] = {"About", "User", "Display", "Network", "Hardware"};
    int tab_w = w / TAB_COUNT;
    
    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = x + i * tab_w;
        int is_active = (i == current_tab);
        
        if (is_active) {
            gfx_fill_rect(tx, y, tab_w, 27, 0xFFFFFFFF);
            gfx_fill_rect(tx, y + 25, tab_w, 3, 0xFF007AFF);
        } else {
            gfx_fill_rect(tx, y, tab_w, 27, 0xFFF2F2F7);
        }
        
        int text_w = strlen(tab_names[i]) * 8;
        gfx_draw_string(tx + (tab_w - text_w) / 2, y + 7, tab_names[i], 
                       is_active ? 0xFF007AFF : 0xFF888888);
    }
}

static void draw_about_tab(int x, int y, int w, int h) {
    int cy = y + 20;
    
    // CamelOS Logo/Title
    gfx_draw_string_scaled(x + 20, cy, "Camel", 0xFF007AFF, 3);
    cy += 40;
    gfx_draw_string_scaled(x + 20, cy, "OS", 0xFF333333, 2);
    cy += 35;
    
    gfx_draw_string(x + 20, cy, "Version 3.0 (APFS+ Compatible)", 0xFF666666);
    cy += 25;
    
    gfx_draw_rect(x + 20, cy, w - 40, 1, 0xFFE0E0E0);
    cy += 15;
    
    gfx_draw_string(x + 20, cy, "A macOS-inspired operating system", 0xFF333333);
    cy += 20;
    gfx_draw_string(x + 20, cy, "with Objective-C runtime support", 0xFF333333);
    cy += 20;
    gfx_draw_string(x + 20, cy, "and .app/.dmg compatibility.", 0xFF333333);
    cy += 30;
    
    // System Info
    gfx_draw_string(x + 20, cy, "System Information:", 0xFF007AFF);
    cy += 22;
    
    gfx_draw_string(x + 30, cy, "Memory:  ", 0xFF888888);
    gfx_draw_string(x + 110, cy, sys_mem_str, 0xFF333333);
    cy += 20;
    
    gfx_draw_string(x + 30, cy, "Disk:    ", 0xFF888888);
    gfx_draw_string(x + 110, cy, sys_disk_str, 0xFF333333);
    cy += 20;
    
    gfx_draw_string(x + 30, cy, "FS:      ", 0xFF888888);
    gfx_draw_string(x + 110, cy, "PFS32 v3.0 (APFS+)", 0xFF333333);
    cy += 20;
    
    gfx_draw_string(x + 30, cy, "Runtime: ", 0xFF888888);
    gfx_draw_string(x + 110, cy, "Objective-C + Foundation", 0xFF333333);
    cy += 20;
    
    gfx_draw_string(x + 30, cy, "CPU:     ", 0xFF888888);
    gfx_draw_string(x + 110, cy, hw_cpu_model, 0xFF333333);
    cy += 20;
    
    gfx_draw_string(x + 30, cy, "Arch:    ", 0xFF888888);
    gfx_draw_string(x + 110, cy, hw_cpu_arch, 0xFF333333);
    cy += 30;
    
    gfx_draw_rect(x + 20, cy, w - 40, 1, 0xFFE0E0E0);
    cy += 10;
    gfx_draw_string(x + 20, cy, "Built with love by 0xFratex", 0xFF999999);
}

static void draw_user_tab(int x, int y, int w, int h) {
    int cy = y + 20;
    
    // User icon
    int icon_x = x + 30;
    gfx_fill_rounded_rect(icon_x, cy, 50, 50, 0xFF007AFF, 25);
    gfx_fill_rounded_rect(icon_x + 17, cy + 10, 16, 16, 0xFFFFFFFF, 8);
    gfx_fill_rounded_rect(icon_x + 10, cy + 28, 30, 20, 0xFFFFFFFF, 6);
    
    // Username
    gfx_draw_string(icon_x + 65, cy + 5, "Username:", 0xFF888888);
    gfx_draw_string(icon_x + 65, cy + 22, cfg_username, 0xFF333333);
    cy += 60;
    
    gfx_draw_rect(x + 20, cy, w - 40, 1, 0xFFE0E0E0);
    cy += 15;
    
    // Account details
    gfx_draw_string(x + 30, cy, "Computer Name:", 0xFF888888);
    gfx_draw_string(x + 170, cy, cfg_computer, 0xFF333333);
    cy += 25;
    
    gfx_draw_string(x + 30, cy, "Home Directory:", 0xFF888888);
    char home[128] = "/Users/";
    strcat(home, cfg_username);
    gfx_draw_string(x + 170, cy, home, 0xFF333333);
    cy += 25;
    
    gfx_draw_string(x + 30, cy, "Timezone:", 0xFF888888);
    gfx_draw_string(x + 170, cy, cfg_timezone, 0xFF333333);
    cy += 25;
    
    gfx_draw_string(x + 30, cy, "Password:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "(encrypted SHA-256)", 0xFF333333);
    cy += 40;
    
    gfx_draw_rect(x + 20, cy, w - 40, 1, 0xFFE0E0E0);
    cy += 15;
    gfx_draw_string(x + 30, cy, "Configuration Path:", 0xFF888888);
    cy += 20;
    gfx_draw_string(x + 40, cy, "/Library/Preferences/system.conf", 0xFF007AFF);
}

static void draw_display_tab(int x, int y, int w, int h) {
    int cy = y + 20;
    
    gfx_draw_string(x + 20, cy, "Theme:", 0xFF888888);
    gfx_draw_string(x + 90, cy, cfg_theme, 0xFF333333);
    cy += 35;
    
    // Theme preview
    const char* theme_names[] = {"Aqua", "Graphite", "Sunset", "Ocean", "Forest"};
    uint32_t theme_colors[] = {0xFF007AFF, 0xFF8E8E93, 0xFFFF9500, 0xFF00C7BE, 0xFF34C759};
    
    for (int i = 0; i < 5; i++) {
        int bx = x + 20 + i * 90;
        int is_current = (strcmp(cfg_theme, theme_names[i]) == 0);
        
        // Color swatch
        gfx_fill_rounded_rect(bx, cy, 70, 40, theme_colors[i], 8);
        if (is_current) {
            gfx_draw_rect(bx - 2, cy - 2, 74, 44, 0xFF007AFF);
        }
        
        // Name
        gfx_draw_string(bx + (70 - strlen(theme_names[i]) * 8) / 2, cy + 48, theme_names[i], 
                       is_current ? 0xFF007AFF : 0xFF666666);
    }
    cy += 80;
    
    gfx_draw_rect(x + 20, cy, w - 40, 1, 0xFFE0E0E0);
    cy += 15;
    
    // Resolution
    gfx_draw_string(x + 20, cy, "Display Resolution:", 0xFF888888);
    extern int screen_w, screen_h;
    char res[32];
    strcpy(res, "");
    char num[16];
    int_to_str(screen_w, num); strcat(res, num);
    strcat(res, " x ");
    int_to_str(screen_h, num); strcat(res, num);
    gfx_draw_string(x + 200, cy, res, 0xFF333333);
    cy += 30;
    
    gfx_draw_string(x + 20, cy, "Color Depth:", 0xFF888888);
    gfx_draw_string(x + 200, cy, "32-bit (ARGB)", 0xFF333333);
}

static void draw_hardware_tab(int x, int y, int w, int h) {
    // Apply scroll offset
    int scroll = settings_scroll_y;
    int cy = y + 20 - scroll;
    int max_y = y + h;  // Bottom clipping boundary
    
    // Helper: only draw if within visible area
    #define DRAW_STRING_IF_VISIBLE(sx, sy, str, col) do { \
        if ((sy) >= y - 16 && (sy) < max_y) gfx_draw_string(sx, sy, str, col); \
    } while(0)
    #define FILL_RECT_IF_VISIBLE(rx, ry, rw, rh, col) do { \
        if ((ry) + (rh) > y && (ry) < max_y) gfx_fill_rect(rx, ry, rw, rh, col); \
    } while(0)
    #define FILL_ROUNDED_IF_VISIBLE(rx, ry, rw, rh, col, rad) do { \
        if ((ry) + (rh) > y && (ry) < max_y) gfx_fill_rounded_rect(rx, ry, rw, rh, col, rad); \
    } while(0)
    #define DRAW_RECT_IF_VISIBLE(rx, ry, rw, rh, col) do { \
        if ((ry) + (rh) > y && (ry) < max_y) gfx_draw_rect(rx, ry, rw, rh, col); \
    } while(0)
    
    // CPU Section
    DRAW_STRING_IF_VISIBLE(x + 20, cy, "Processor", 0xFF007AFF);
    cy += 25;
    
    FILL_ROUNDED_IF_VISIBLE(x + 20, cy, w - 40, 90, 0xFFF2F2F7, 8);
    DRAW_RECT_IF_VISIBLE(x + 20, cy, w - 40, 90, 0xFFE0E0E0);
    
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 10, "Vendor:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 120, cy + 10, hw_cpu_vendor, 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 30, "Model:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 120, cy + 30, hw_cpu_model, 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 50, "Architecture:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 120, cy + 50, hw_cpu_arch, 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 70, "Status:", 0xFF8E8E93);
    if (cy + 67 + 18 > y && cy + 67 < max_y) {
        if (hw_cpu_supported) {
            gfx_fill_rounded_rect(x + 120, cy + 67, 70, 18, 0xFFE8F5E9, 4);
            gfx_draw_string(x + 130, cy + 70, "Supported", 0xFF34C759);
        } else {
            gfx_fill_rounded_rect(x + 120, cy + 67, 80, 18, 0xFFFFEBEE, 4);
            gfx_draw_string(x + 130, cy + 70, "Unknown", 0xFFFF3B30);
        }
    }
    cy += 105;
    
    // Features
    DRAW_STRING_IF_VISIBLE(x + 20, cy, "CPU Features", 0xFF007AFF);
    cy += 25;
    
    FILL_ROUNDED_IF_VISIBLE(x + 20, cy, w - 40, 30, 0xFFF2F2F7, 8);
    DRAW_RECT_IF_VISIBLE(x + 20, cy, w - 40, 30, 0xFFE0E0E0);
    // Check basic CPU features via CPUID leaf 1
    uint32_t eax1, ebx1, ecx1, edx1;
    asm volatile("cpuid" : "=a"(eax1), "=b"(ebx1), "=c"(ecx1), "=d"(edx1) : "a"(1));
    char feat_str[128] = "";
    if (edx1 & (1 << 0)) strcat(feat_str, "FPU ");
    if (edx1 & (1 << 23)) strcat(feat_str, "MMX ");
    if (edx1 & (1 << 25)) strcat(feat_str, "SSE ");
    if (edx1 & (1 << 26)) strcat(feat_str, "SSE2 ");
    if (ecx1 & (1 << 0)) strcat(feat_str, "SSE3 ");
    if (ecx1 & (1 << 9)) strcat(feat_str, "SSSE3 ");
    if (ecx1 & (1 << 19)) strcat(feat_str, "SSE4.1 ");
    if (ecx1 & (1 << 20)) strcat(feat_str, "SSE4.2 ");
    if (ecx1 & (1 << 28)) strcat(feat_str, "AVX ");
    if (edx1 & (1 << 24)) strcat(feat_str, "FXSR ");
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 8, feat_str, 0xFF333333);
    cy += 45;
    
    // Memory Section
    DRAW_STRING_IF_VISIBLE(x + 20, cy, "Memory", 0xFF007AFF);
    cy += 25;
    
    FILL_ROUNDED_IF_VISIBLE(x + 20, cy, w - 40, 50, 0xFFF2F2F7, 8);
    DRAW_RECT_IF_VISIBLE(x + 20, cy, w - 40, 50, 0xFFE0E0E0);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 10, "Total RAM:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 10, hw_mem_total_str, 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 30, "Free:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 30, sys_mem_str + 6, 0xFF333333);  // skip "Free: " prefix
    cy += 65;
    
    // Disk Section
    DRAW_STRING_IF_VISIBLE(x + 20, cy, "Disk (PFS32)", 0xFF007AFF);
    cy += 25;
    
    FILL_ROUNDED_IF_VISIBLE(x + 20, cy, w - 40, 70, 0xFFF2F2F7, 8);
    DRAW_RECT_IF_VISIBLE(x + 20, cy, w - 40, 70, 0xFFE0E0E0);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 10, "Filesystem:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 10, "PFS32 v3.0 (APFS+)", 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 30, "Usage:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 30, hw_disk_size_str, 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 50, "Free blocks:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 50, hw_disk_free_str, 0xFF333333);
    cy += 85;
    
    // Network Section
    DRAW_STRING_IF_VISIBLE(x + 20, cy, "Network Interfaces", 0xFF007AFF);
    cy += 25;
    
    FILL_ROUNDED_IF_VISIBLE(x + 20, cy, w - 40, 50, 0xFFF2F2F7, 8);
    DRAW_RECT_IF_VISIBLE(x + 20, cy, w - 40, 50, 0xFFE0E0E0);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 10, "Driver:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 10, "RTL8139 Ethernet", 0xFF333333);
    DRAW_STRING_IF_VISIBLE(x + 32, cy + 30, "MAC:", 0xFF8E8E93);
    DRAW_STRING_IF_VISIBLE(x + 140, cy + 30, "52:54:00:12:34:56", 0xFF333333);
    
    #undef DRAW_STRING_IF_VISIBLE
    #undef FILL_RECT_IF_VISIBLE
    #undef FILL_ROUNDED_IF_VISIBLE
    #undef DRAW_RECT_IF_VISIBLE
    
    // Draw scrollbar if content overflows
    int total_content_h = cy + 75 - (y + 20 - scroll);  // Estimated total content height
    // Clamp scroll to valid range
    int max_scroll = (total_content_h > h) ? (total_content_h - h) : 0;
    if (settings_scroll_y > max_scroll) settings_scroll_y = max_scroll;
    if (total_content_h > h) {
        int sb_x = x + w - 12;
        int sb_h = h - 4;
        int thumb_h = (h * h) / total_content_h;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int scroll_range = total_content_h - h;
        int thumb_y = y + 2;
        if (scroll_range > 0) {
            thumb_y = y + 2 + (settings_scroll_y * (sb_h - thumb_h)) / scroll_range;
        }
        // Ensure thumb stays within scrollbar bounds
        if (thumb_y < y + 2) thumb_y = y + 2;
        if (thumb_y + thumb_h > y + 2 + sb_h) thumb_y = y + 2 + sb_h - thumb_h;
        gfx_fill_rect(sb_x, y + 2, 8, sb_h, 0x20C0C0C0);
        gfx_fill_rounded_rect(sb_x + 1, thumb_y, 6, thumb_h, 0xFFC0C0C0, 3);
    }
}

static void draw_network_tab(int x, int y, int w, int h) {
    int cy = y + 20;
    
    gfx_draw_string(x + 20, cy, "Network Configuration:", 0xFF007AFF);
    cy += 25;
    
    gfx_draw_string(x + 30, cy, "IP Address:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "10.0.2.15", 0xFF333333);
    cy += 22;
    
    gfx_draw_string(x + 30, cy, "Gateway:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "10.0.2.2", 0xFF333333);
    cy += 22;
    
    gfx_draw_string(x + 30, cy, "DNS:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "10.0.2.3", 0xFF333333);
    cy += 22;
    
    gfx_draw_string(x + 30, cy, "Subnet:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "255.255.255.0", 0xFF333333);
    cy += 22;
    
    gfx_draw_string(x + 30, cy, "Driver:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "RTL8139", 0xFF333333);
    cy += 22;
    
    gfx_draw_string(x + 30, cy, "MAC:", 0xFF888888);
    gfx_draw_string(x + 170, cy, "52:54:00:12:34:56", 0xFF333333);
    cy += 35;
    
    gfx_draw_rect(x + 20, cy, w - 40, 1, 0xFFE0E0E0);
    cy += 15;
    
    gfx_draw_string(x + 20, cy, "Supported Protocols:", 0xFF007AFF);
    cy += 25;
    
    const char* protocols[] = {"Ethernet (RTL8139)", "IPv4 / ARP / ICMP", 
                                "TCP / UDP Sockets", "DNS Resolution",
                                "TLS 1.3 (HTTPS)", "HTTP/2"};
    for (int i = 0; i < 6; i++) {
        gfx_draw_string(x + 40, cy, protocols[i], 0xFF333333);
        cy += 20;
    }
}

static void settings_on_paint(window_t* win, int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
    
    // Tab bar
    draw_tab_bar(x, y, w);
    
    // Content area
    int content_y = y + 32;
    int content_h = h - 32;
    
    switch (current_tab) {
        case TAB_ABOUT:    draw_about_tab(x, content_y, w, content_h); break;
        case TAB_USER:     draw_user_tab(x, content_y, w, content_h); break;
        case TAB_DISPLAY:  draw_display_tab(x, content_y, w, content_h); break;
        case TAB_NETWORK:  draw_network_tab(x, content_y, w, content_h); break;
        case TAB_HARDWARE: draw_hardware_tab(x, content_y, w, content_h); break;
    }
}

// Current window dimensions (updated on resize)
static int settings_win_w = 500;

static void settings_on_mouse(window_t* win, int x, int y, int btn) {
    if (btn != 1) return;
    
    // Tab clicks - use dynamic width from actual window size
    if (y >= 0 && y < 28) {
        int tab_w = settings_win_w / TAB_COUNT;
        int tab = x / tab_w;
        if (tab >= 0 && tab < TAB_COUNT) {
            current_tab = tab;
            settings_scroll_y = 0;  // Reset scroll when switching tabs
        }
    }
}

// Menu action handler for Settings app
static void settings_on_menu_action(int menu_id, int item_idx) {
    if (menu_id == 0) { // File menu
        if (item_idx == 0) { // Refresh
            settings_load_config();
            detect_cpu_info();
        }
        // item_idx == 1 is Close - handled by window close button
    } else if (menu_id == 1) { // View menu
        if (item_idx >= 0 && item_idx < TAB_COUNT) {
            current_tab = item_idx;
        }
    }
}

static void settings_on_input(window_t* win, int key) {
    // No keyboard input needed for settings
    (void)key;
}

static void settings_on_scroll(window_t* win, int delta) {
    settings_scroll_y += delta * 20;
    if (settings_scroll_y < 0) settings_scroll_y = 0;
    // Upper bound is clamped dynamically in draw_hardware_tab based on content height
    // A generous max prevents overflow while the exact clamp is computed at draw time
    if (settings_scroll_y > 2000) settings_scroll_y = 2000;
}

static void settings_on_resize(window_t* win, int new_w, int new_h) {
    settings_win_w = new_w;
}

void init_settings_app() {
    settings_load_config();
    Window* w = fw_create_window("Settings", 500, 420, settings_on_paint, settings_on_input, settings_on_mouse);
    if (!w) return;  // Guard against window creation failure
    w->min_w = 400;
    
    // Wire up scroll and resize callbacks
    w->scroll_callback = (void*)settings_on_scroll;
    w->resize_callback = (void*)settings_on_resize;
    
    // Detect hardware info
    detect_cpu_info();
    
    w->menu_count = 2;
    strcpy(w->menus[0].name, "File");
    strcpy(w->menus[0].items[0].label, "Refresh");
    strcpy(w->menus[0].items[1].label, "Close");
    w->menus[0].item_count = 2;
    
    strcpy(w->menus[1].name, "View");
    strcpy(w->menus[1].items[0].label, "About");
    strcpy(w->menus[1].items[1].label, "User");
    strcpy(w->menus[1].items[2].label, "Display");
    strcpy(w->menus[1].items[3].label, "Network");
    strcpy(w->menus[1].items[4].label, "Hardware");
    w->menus[1].item_count = 5;
    
    // Set menu action handler to prevent crash when clicking menu items
    w->on_menu_action = (void*)settings_on_menu_action;
    
    fw_register_dock("Settings", 4, w);
}
