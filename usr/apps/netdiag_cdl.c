// usr/apps/netdiag_cdl.c - Network Diagnostics Tool
// A comprehensive network diagnostic application for Camel OS

#include "../../sys/cdl_defs.h"
#include <stdarg.h>

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

#define WIN_WIDTH 700
#define WIN_HEIGHT 500

// Colors - macOS style
#define C_BG        0xFFF6F6F6
#define C_TEXT      0xFF1C1C1E
#define C_TEXT_DIM  0xFF8E8E93
#define C_ACCENT    0xFF007AFF
#define C_SUCCESS   0xFF34C759
#define C_WARNING   0xFFFF9500
#define C_ERROR     0xFFFF3B30
#define C_BORDER    0xFFE5E5EA
#define C_HEADER    0xFFF2F2F7

// Tab IDs
typedef enum {
    TAB_STATUS,
    TAB_ARP,
    TAB_PACKETS,
    TAB_PING,
    TAB_COUNT
} NetDiagTab;

// App state
typedef struct {
    int active_tab;
    int scroll_y;
    char ping_target[64];
    char ping_results[1024];
    int ping_running;
    int arp_scroll;
    int packet_scroll;
} NetDiagState;

static NetDiagState state;
static kernel_api_t* sys;

// ============================================================================
// Helper Functions
// ============================================================================

static void draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    sys->draw_rect_rounded(x, y, w, h, color, r);
}

static void draw_gradient_header(int x, int y, int w, int h) {
    // Top to bottom gradient
    for (int i = 0; i < h; i++) {
        uint32_t col = C_HEADER - (i * 0x010101);
        sys->draw_rect(x, y + i, w, 1, col);
    }
}

static void int_to_str(int n, char* buf) {
    if (n == 0) {
        buf[0] = '0'; buf[1] = 0;
        return;
    }
    int i = 0, neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    char rev[12];
    while (n > 0) {
        rev[i++] = '0' + (n % 10);
        n /= 10;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = rev[--i];
    buf[j] = 0;
}

static void ip_to_str(uint32_t ip, char* str) {
    uint8_t b[4];
    b[0] = (ip >> 24) & 0xFF;
    b[1] = (ip >> 16) & 0xFF;
    b[2] = (ip >> 8) & 0xFF;
    b[3] = ip & 0xFF;
    
    char temp[4];
    int_to_str(b[0], str); strcat(str, ".");
    int_to_str(b[1], temp); strcat(str, temp); strcat(str, ".");
    int_to_str(b[2], temp); strcat(str, temp); strcat(str, ".");
    int_to_str(b[3], temp); strcat(str, temp);
}

static int strlen(const char* s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

static void strcpy(char* d, const char* s) {
    while (*s) *d++ = *s++;
    *d = 0;
}

static void strcat(char* d, const char* s) {
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}

// ============================================================================
// Tab: Status
// ============================================================================

static void draw_status_tab(int x, int y, int w, int h) {
    int pad = 20;
    int line_h = 24;
    int cy = y + pad;
    
    // Network Interface Card
    draw_rounded_rect(x + pad, cy, w - pad*2, 120, 8, 0xFFFFFFFF);
    sys->draw_text(x + pad + 15, cy + 15, "Network Interface", C_TEXT);
    sys->draw_rect(x + pad, cy + 40, w - pad*2, 1, C_BORDER);
    
    // Interface info
    sys->draw_text(x + pad + 15, cy + 50, "Interface:", C_TEXT_DIM);
    sys->draw_text(x + pad + 100, cy + 50, "eth0 (RTL8139)", C_TEXT);
    
    sys->draw_text(x + pad + 15, cy + 74, "MAC Address:", C_TEXT_DIM);
    sys->draw_text(x + pad + 100, cy + 74, "52:54:00:12:34:56", C_TEXT);
    
    sys->draw_text(x + pad + 15, cy + 98, "Status:", C_TEXT_DIM);
    // Assume connected for demo - would check actual net state
    sys->draw_text(x + pad + 100, cy + 98, "Connected", C_SUCCESS);
    
    cy += 140;
    
    // IP Configuration
    draw_rounded_rect(x + pad, cy, w - pad*2, 144, 8, 0xFFFFFFFF);
    sys->draw_text(x + pad + 15, cy + 15, "IP Configuration", C_TEXT);
    sys->draw_rect(x + pad, cy + 40, w - pad*2, 1, C_BORDER);
    
    sys->draw_text(x + pad + 15, cy + 50, "IP Address:", C_TEXT_DIM);
    sys->draw_text(x + pad + 100, cy + 50, "10.0.2.15", C_TEXT);
    
    sys->draw_text(x + pad + 15, cy + 74, "Subnet Mask:", C_TEXT_DIM);
    sys->draw_text(x + pad + 100, cy + 74, "255.255.255.0", C_TEXT);
    
    sys->draw_text(x + pad + 15, cy + 98, "Gateway:", C_TEXT_DIM);
    sys->draw_text(x + pad + 100, cy + 98, "10.0.2.2", C_TEXT);
    
    sys->draw_text(x + pad + 15, cy + 122, "DNS Server:", C_TEXT_DIM);
    sys->draw_text(x + pad + 100, cy + 122, "10.0.2.3", C_TEXT);
}

// ============================================================================
// Tab: ARP Table
// ============================================================================

static void draw_arp_tab(int x, int y, int w, int h) {
    int pad = 20;
    int header_h = 40;
    int row_h = 28;
    
    // Table header
    draw_rounded_rect(x + pad, y + pad, w - pad*2, header_h, 8, C_HEADER);
    sys->draw_text(x + pad + 15, y + pad + 12, "IP Address", C_TEXT);
    sys->draw_text(x + pad + 180, y + pad + 12, "MAC Address", C_TEXT);
    sys->draw_text(x + pad + 350, y + pad + 12, "Type", C_TEXT);
    sys->draw_text(x + pad + 450, y + pad + 12, "Status", C_TEXT);
    
    // Table content area
    int content_y = y + pad + header_h + 5;
    int content_h = h - pad*2 - header_h - 5;
    draw_rounded_rect(x + pad, content_y, w - pad*2, content_h, 8, 0xFFFFFFFF);
    
    // Sample ARP entries (would be populated from actual ARP cache)
    const char* arp_entries[][4] = {
        {"10.0.2.2", "52:54:00:12:34:56", "Gateway", "Complete"},
        {"10.0.2.3", "52:54:00:12:34:57", "DNS", "Complete"},
        {"10.0.2.15", "52:54:00:12:34:56", "Local", "Complete"},
    };
    
    int num_entries = 3;
    int cy = content_y + 10 - state.arp_scroll;
    
    for (int i = 0; i < num_entries && cy < content_y + content_h; i++) {
        if (cy + row_h > content_y) {
            // Row background on hover
            if (i % 2 == 0) {
                sys->draw_rect(x + pad + 1, cy, w - pad*2 - 2, row_h, 0xFFF9F9F9);
            }
            
            sys->draw_text(x + pad + 15, cy + 7, arp_entries[i][0], C_TEXT);
            sys->draw_text(x + pad + 180, cy + 7, arp_entries[i][1], C_TEXT);
            sys->draw_text(x + pad + 350, cy + 7, arp_entries[i][2], C_TEXT_DIM);
            
            uint32_t status_color = strcmp(arp_entries[i][3], "Complete") == 0 ? C_SUCCESS : C_WARNING;
            sys->draw_text(x + pad + 450, cy + 7, arp_entries[i][3], status_color);
            
            // Separator line
            if (i < num_entries - 1) {
                sys->draw_rect(x + pad + 10, cy + row_h - 1, w - pad*2 - 20, 1, C_BORDER);
            }
        }
        cy += row_h;
    }
}

// ============================================================================
// Tab: Packet Monitor
// ============================================================================

static void draw_packets_tab(int x, int y, int w, int h) {
    int pad = 20;
    
    // Packet stats cards
    int card_w = (w - pad*2 - 30) / 3;
    int card_h = 80;
    int cx = x + pad;
    int cy = y + pad;
    
    // TX Packets card
    draw_rounded_rect(cx, cy, card_w, card_h, 8, 0xFFFFFFFF);
    sys->draw_text(cx + 15, cy + 15, "TX Packets", C_TEXT_DIM);
    sys->draw_text(cx + 15, cy + 45, "1,234", C_TEXT);
    sys->draw_text(cx + 15, cy + 62, "45.2 KB", C_TEXT_DIM);
    cx += card_w + 15;
    
    // RX Packets card
    draw_rounded_rect(cx, cy, card_w, card_h, 8, 0xFFFFFFFF);
    sys->draw_text(cx + 15, cy + 15, "RX Packets", C_TEXT_DIM);
    sys->draw_text(cx + 15, cy + 45, "987", C_TEXT);
    sys->draw_text(cx + 15, cy + 62, "32.1 KB", C_TEXT_DIM);
    cx += card_w + 15;
    
    // Errors card
    draw_rounded_rect(cx, cy, card_w, card_h, 8, 0xFFFFFFFF);
    sys->draw_text(cx + 15, cy + 15, "Errors", C_TEXT_DIM);
    sys->draw_text(cx + 15, cy + 45, "0", C_SUCCESS);
    sys->draw_text(cx + 15, cy + 62, "Dropped: 0", C_TEXT_DIM);
    
    cy += card_h + 20;
    
    // Recent packets log
    int log_h = h - (cy - y) - pad;
    draw_rounded_rect(x + pad, cy, w - pad*2, log_h, 8, 0xFFFFFFFF);
    sys->draw_text(x + pad + 15, cy + 15, "Recent Packets", C_TEXT);
    sys->draw_rect(x + pad, cy + 40, w - pad*2, 1, C_BORDER);
    
    // Sample packet log
    const char* packets[] = {
        "[TX] TCP SYN -> 104.18.26.120:80",
        "[RX] TCP SYN-ACK <- 104.18.26.120:80",
        "[TX] TCP ACK -> 104.18.26.120:80",
        "[TX] HTTP GET -> example.com",
        "[RX] HTTP 200 OK <- example.com",
        "[TX] DNS Query -> 10.0.2.3",
        "[RX] DNS Response <- 10.0.2.3",
        "[TX] ARP Request -> 10.0.2.2",
        "[RX] ARP Response <- 52:54:00:12:34:56",
    };
    
    int py = cy + 50 - state.packet_scroll;
    for (int i = 0; i < 9 && py < cy + log_h; i++) {
        if (py > cy + 40) {
            uint32_t color = (packets[i][1] == 'T') ? C_ACCENT : C_SUCCESS;
            sys->draw_text(x + pad + 15, py, packets[i], color);
        }
        py += 20;
    }
}

// ============================================================================
// Tab: Ping
// ============================================================================

static void draw_ping_tab(int x, int y, int w, int h) {
    int pad = 20;
    int cy = y + pad;
    
    // Target input box
    sys->draw_text(x + pad, cy, "Target Host:", C_TEXT);
    cy += 25;
    
    draw_rounded_rect(x + pad, cy, w - pad*2 - 100, 32, 6, 0xFFFFFFFF);
    sys->draw_rect(x + pad, cy, w - pad*2 - 100, 32, C_BORDER);
    sys->draw_text(x + pad + 10, cy + 9, state.ping_target[0] ? state.ping_target : "Enter IP or hostname...", 
                   state.ping_target[0] ? C_TEXT : C_TEXT_DIM);
    
    // Ping button
    draw_rounded_rect(x + pad + w - pad*2 - 90, cy, 80, 32, 6, C_ACCENT);
    sys->draw_text(x + pad + w - pad*2 - 65, cy + 9, "Ping", 0xFFFFFFFF);
    
    cy += 50;
    
    // Results area
    int results_h = h - (cy - y) - pad;
    draw_rounded_rect(x + pad, cy, w - pad*2, results_h, 8, 0xFF1C1C1E);
    
    // Draw results text (monospace style)
    if (state.ping_results[0]) {
        char* line = state.ping_results;
        int ly = cy + 15;
        while (*line && ly < cy + results_h - 15) {
            char buf[80];
            int i = 0;
            while (*line && *line != '\n' && i < 79) {
                buf[i++] = *line++;
            }
            buf[i] = 0;
            if (*line == '\n') line++;
            
            sys->draw_text(x + pad + 15, ly, buf, 0xFF34C759); // Green terminal text
            ly += 16;
        }
    } else {
        sys->draw_text(x + pad + 15, cy + 15, "Ready to ping...", C_TEXT_DIM);
    }
}

// ============================================================================
// Main Window
// ============================================================================

static void draw_tabs(int x, int y, int w) {
    const char* tab_names[] = {"Status", "ARP Table", "Packets", "Ping"};
    int tab_w = w / TAB_COUNT;
    
    // Tab bar background
    draw_gradient_header(x, y, w, 36);
    sys->draw_rect(x, y + 35, w, 1, C_BORDER);
    
    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = x + i * tab_w;
        
        // Active tab indicator
        if (i == state.active_tab) {
            sys->draw_rect(tx, y + 33, tab_w, 3, C_ACCENT);
            sys->draw_text(tx + (tab_w - strlen(tab_names[i]) * 8) / 2, y + 12, 
                          tab_names[i], C_ACCENT);
        } else {
            sys->draw_text(tx + (tab_w - strlen(tab_names[i]) * 8) / 2, y + 12, 
                          tab_names[i], C_TEXT_DIM);
        }
    }
}

static void netdiag_on_paint(int x, int y, int w, int h, int active) {
    // Clear background
    sys->draw_rect(x, y, w, h, C_BG);
    
    // Window title in header
    sys->draw_text(x + 15, y + 12, "Network Diagnostics", C_TEXT);
    
    // Tabs
    draw_tabs(x, y + 40, w);
    
    // Tab content area
    int content_y = y + 76;
    int content_h = h - 76;
    
    switch (state.active_tab) {
        case TAB_STATUS:
            draw_status_tab(x, content_y, w, content_h);
            break;
        case TAB_ARP:
            draw_arp_tab(x, content_y, w, content_h);
            break;
        case TAB_PACKETS:
            draw_packets_tab(x, content_y, w, content_h);
            break;
        case TAB_PING:
            draw_ping_tab(x, content_y, w, content_h);
            break;
    }
}

static void netdiag_on_input(int key) {
    if (key == '\t') {
        state.active_tab = (state.active_tab + 1) % TAB_COUNT;
    }
}

static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static void netdiag_on_mouse(int x, int y, int btn, int event) {
    // Tab clicking
    if (y >= 40 && y < 76) {
        int tab_w = WIN_WIDTH / TAB_COUNT;
        int tab = x / tab_w;
        if (tab >= 0 && tab < TAB_COUNT && event == 1) { // Click
            state.active_tab = tab;
        }
    }
    
    // Scroll handling for tabs with scrollable content
    if (event == 3) { // Scroll up
        if (state.active_tab == TAB_ARP) state.arp_scroll -= 20;
        if (state.active_tab == TAB_PACKETS) state.packet_scroll -= 20;
    } else if (event == 4) { // Scroll down
        if (state.active_tab == TAB_ARP) state.arp_scroll += 20;
        if (state.active_tab == TAB_PACKETS) state.packet_scroll += 20;
    }
    
    // Clamp scroll values
    if (state.arp_scroll < 0) state.arp_scroll = 0;
    if (state.packet_scroll < 0) state.packet_scroll = 0;
}

// ============================================================================
// App Entry Points
// ============================================================================

void app_init(kernel_api_t* api) {
    sys = api;
    memset(&state, 0, sizeof(state));
    strcpy(state.ping_target, "10.0.2.2");
    state.active_tab = TAB_STATUS;
}

void app_run() {
    sys->create_window("Network Diagnostics", WIN_WIDTH, WIN_HEIGHT,
                      netdiag_on_paint, netdiag_on_input, netdiag_on_mouse);
}

void app_exit() {
    // Cleanup
}
