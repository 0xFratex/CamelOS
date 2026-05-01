// usr/apps/settings.c - CamelOS Settings App
// System settings viewer with theme, user info, and about panel
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../dock.h"
#include "../../fs/pfs32.h"

// Tab IDs
#define TAB_ABOUT    0
#define TAB_USER     1
#define TAB_DISPLAY  2
#define TAB_NETWORK  3
#define TAB_COUNT    4

static int current_tab = TAB_ABOUT;

// Config data (loaded from system.conf)
static char cfg_username[64] = "(not set)";
static char cfg_computer[64] = "CamelOS";
static char cfg_theme[32] = "Aqua";
static char cfg_timezone[32] = "UTC";

// System info
static char sys_mem_str[32] = "";
static char sys_disk_str[32] = "";

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

static void draw_tab_bar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, 28, 0xFFF2F2F7);
    gfx_draw_rect(x, y + 27, w, 1, 0xFFC6C6C8);
    
    const char* tab_names[] = {"About", "User", "Display", "Network"};
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

static void settings_on_paint(int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
    
    // Tab bar
    draw_tab_bar(x, y, w);
    
    // Content area
    int content_y = y + 32;
    int content_h = h - 32;
    
    switch (current_tab) {
        case TAB_ABOUT:   draw_about_tab(x, content_y, w, content_h); break;
        case TAB_USER:    draw_user_tab(x, content_y, w, content_h); break;
        case TAB_DISPLAY: draw_display_tab(x, content_y, w, content_h); break;
        case TAB_NETWORK: draw_network_tab(x, content_y, w, content_h); break;
    }
}

static void settings_on_mouse(int x, int y, int btn) {
    if (btn != 1) return;
    
    // Tab clicks
    if (y >= 0 && y < 28) {
        int tab_w = 500 / TAB_COUNT;
        int tab = x / tab_w;
        if (tab >= 0 && tab < TAB_COUNT) {
            current_tab = tab;
        }
    }
}

static void settings_on_input(int key) {
    // No keyboard input needed for settings
    (void)key;
}

void init_settings_app() {
    settings_load_config();
    Window* w = fw_create_window("Settings", 500, 420, settings_on_paint, settings_on_input, settings_on_mouse);
    w->min_w = 400;
    
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
    w->menus[1].item_count = 4;
    
    fw_register_dock("Settings", 4, w);
}
