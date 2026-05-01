// usr/apps/browser.c - CamelOS Browser App
// Basic web browser with URL bar and HTML rendering
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/cpu/timer.h"
#include "../../core/tcp.h"
#include "../dock.h"

// Layout
#define URL_BAR_H 32
#define STATUS_BAR_H 20
#define PAD 6

// State
static char url_buf[256] = "http://example.com";
static int url_cursor = 0;
static int url_active = 1;  // URL bar focused by default

// Page content buffer
#define PAGE_LINES 50
#define PAGE_LINE_LEN 256
static char page_lines[PAGE_LINES][PAGE_LINE_LEN];
static int page_line_count = 0;
static int scroll_offset = 0;
static char status_text[64] = "Ready";

// Loading state
static int is_loading = 0;

// Current window dimensions (updated on resize)
static int browser_win_w = 600;
static int browser_win_h = 420;

static void browser_load_page(const char* url) {
    page_line_count = 0;
    scroll_offset = 0;
    is_loading = 1;
    strcpy(status_text, "Loading...");
    
    // Parse URL to extract host and path
    char host[128] = "";
    char path[128] = "/";
    
    const char* url_start = url;
    if (strncmp(url, "http://", 7) == 0) url_start = url + 7;
    else if (strncmp(url, "https://", 8) == 0) url_start = url + 8;
    
    // Extract host
    int hi = 0;
    while (*url_start && *url_start != '/' && hi < 127) {
        host[hi++] = *url_start++;
    }
    host[hi] = 0;
    
    // Extract path
    if (*url_start == '/') {
        strncpy(path, url_start, 127);
        path[127] = 0;
    }
    
    // Try DNS resolution
    char ip_str[16];
    extern int dns_resolve(const char* name, char* ip_buf, int ip_buf_len);
    int dns_ok = dns_resolve(host, ip_str, sizeof(ip_str));
    
    if (dns_ok != 0) {
        // DNS failed - show error page
        strcpy(page_lines[0], "Error: Could not resolve hostname");
        strcpy(page_lines[1], "");
        strcpy(page_lines[2], "The server could not be found.");
        page_line_count = 3;
        strcpy(status_text, "DNS Error");
        is_loading = 0;
        return;
    }
    
    // Try TCP connection
    extern uint32_t ip_parse(const char* str);
    uint32_t ip = ip_parse(ip_str);
    
    void* conn = tcp_connect_with_ptr(ip, 80);
    if (!conn) {
        strcpy(page_lines[0], "Error: Connection failed");
        strcpy(page_lines[1], "");
        strcpy(page_lines[2], "Could not connect to server.");
        page_line_count = 3;
        strcpy(status_text, "Connection Error");
        is_loading = 0;
        return;
    }
    
    // Wait for connection
    uint32_t start = get_tick_count();
    int established = 0;
    while (get_tick_count() - start < 5000) {
        extern void rtl8139_poll();
        rtl8139_poll();
        if (tcp_conn_is_established(conn)) {
            established = 1;
            break;
        }
    }
    
    if (!established) {
        strcpy(page_lines[0], "Error: Connection timeout");
        page_line_count = 1;
        strcpy(status_text, "Timeout");
        is_loading = 0;
        return;
    }
    
    // Send HTTP request
    char request[512];
    int rlen = 0;
    rlen += sprintf(request + rlen, "GET %s HTTP/1.1\r\n", path);
    rlen += sprintf(request + rlen, "Host: %s\r\n", host);
    rlen += sprintf(request + rlen, "User-Agent: CamelOS/3.0\r\n");
    rlen += sprintf(request + rlen, "Connection: close\r\n");
    rlen += sprintf(request + rlen, "\r\n");
    
    tcp_conn_send(conn, request, rlen);
    
    // Read response
    char response[8192];
    int total_read = 0;
    for (int retry = 0; retry < 100 && total_read < (int)sizeof(response) - 1; retry++) {
        extern void rtl8139_poll();
        rtl8139_poll();
        int n = tcp_conn_recv(conn, response + total_read, sizeof(response) - total_read - 1);
        if (n > 0) total_read += n;
        else if (n == 0) break;
        else {
            // Wait a bit
            for (volatile int d = 0; d < 50000; d++);
        }
    }
    response[total_read] = 0;
    
    // Parse response - skip HTTP headers
    char* body = strstr(response, "\r\n\r\n");
    if (body) body += 4;
    else body = response;
    
    // Simple HTML to text conversion
    int line = 0;
    int col = 0;
    int in_tag = 0;
    int in_script = 0;
    
    for (int i = 0; body[i] && line < PAGE_LINES; i++) {
        char c = body[i];
        
        if (c == '<') {
            in_tag = 1;
            // Check for script/style tags to skip
            if (strncmp(body + i, "<script", 7) == 0 || strncmp(body + i, "<style", 6) == 0) {
                in_script = 1;
            }
            if (strncmp(body + i, "</script>", 9) == 0 || strncmp(body + i, "</style>", 8) == 0) {
                in_script = 0;
            }
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag || in_script) continue;
        
        // Handle HTML entities
        if (c == '&') {
            if (strncmp(body + i, "&amp;", 5) == 0) { c = '&'; i += 4; }
            else if (strncmp(body + i, "&lt;", 4) == 0) { c = '<'; i += 3; }
            else if (strncmp(body + i, "&gt;", 4) == 0) { c = '>'; i += 3; }
            else if (strncmp(body + i, "&nbsp;", 6) == 0) { c = ' '; i += 5; }
        }
        
        if (c == '\n' || c == '\r') {
            if (col > 0) {
                page_lines[line][col] = 0;
                line++;
                col = 0;
            }
            continue;
        }
        
        // Skip multiple spaces
        if (c == ' ' && col > 0 && page_lines[line][col-1] == ' ') continue;
        
        if (col < PAGE_LINE_LEN - 1) {
            page_lines[line][col++] = c;
        } else {
            page_lines[line][col] = 0;
            line++;
            col = 0;
        }
    }
    if (col > 0 && line < PAGE_LINES) {
        page_lines[line][col] = 0;
        line++;
    }
    page_line_count = line;
    
    char count_str[16];
    strcpy(status_text, "Loaded (");
    int_to_str(page_line_count, count_str);
    strcat(status_text, count_str);
    strcat(status_text, " lines)");
    is_loading = 0;
}

static void browser_on_paint(int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
    
    // URL Bar
    gfx_fill_rect(x, y, w, URL_BAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + URL_BAR_H - 1, w, 1, 0xFFC6C6C8);
    
    // Navigation buttons
    int bx = x + 6;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 8, y + 9, "<", 0xFF555555);
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 8, y + 9, ">", 0xFF555555);
    bx += 32;
    gfx_fill_rounded_rect(bx, y + 4, 28, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 6, y + 9, "R", 0xFF555555);
    bx += 36;
    
    // URL input field
    int url_w = w - (bx - x) - 60;
    if (url_w < 40) url_w = 40;
    gfx_fill_rounded_rect(bx, y + 4, url_w, 24, 0xFFFFFFFF, 4);
    gfx_draw_rect(bx, y + 4, url_w, 24, url_active ? 0xFF007AFF : 0xFFC6C6C8);
    
    // URL text
    gfx_draw_string(bx + 6, y + 9, url_buf, 0xFF333333);
    
    // Cursor
    if (url_active) {
        static int blink = 0; blink++;
        if (blink % 60 < 30) {
            gfx_fill_rect(bx + 6 + url_cursor * 8, y + 9, 1, 14, 0xFF007AFF);
        }
    }
    
    // Go button
    int go_x = bx + url_w + 4;
    gfx_fill_rounded_rect(go_x, y + 4, 40, 24, 0xFF007AFF, 4);
    gfx_draw_string(go_x + 8, y + 9, "Go", 0xFFFFFFFF);
    
    // Page content
    int content_y = y + URL_BAR_H + 4;
    int content_h = h - URL_BAR_H - STATUS_BAR_H - 4;
    if (content_h < 0) content_h = 0;
    int visible_lines = content_h / 16;
    
    // Clamp scroll offset
    if (scroll_offset < 0) scroll_offset = 0;
    int max_scroll = page_line_count - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    
    for (int i = 0; i < visible_lines && (i + scroll_offset) < page_line_count; i++) {
        gfx_draw_string(x + PAD, content_y + i * 16, page_lines[i + scroll_offset], 0xFF333333);
    }
    
    // Empty state
    if (page_line_count == 0) {
        gfx_draw_string_centered(x + w/2, y + h/2, "Enter a URL and press Go", 0xFF999999, 1);
    }
    
    // Status bar
    int status_y = y + h - STATUS_BAR_H;
    gfx_fill_rect(x, status_y, w, STATUS_BAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, status_y, w, 1, 0xFFC6C6C8);
    gfx_draw_string(x + 8, status_y + 4, status_text, 0xFF888888);
    
    // Loading indicator
    if (is_loading) {
        static int dots = 0; dots++;
        int nd = (dots / 20) % 4;
        char loading[16] = "Loading";
        for (int d = 0; d < nd; d++) strcat(loading, ".");
        gfx_draw_string(x + w - 100, status_y + 4, loading, 0xFF007AFF);
    }
}

static void browser_on_scroll(int delta) {
    scroll_offset -= delta * 3;
    if (scroll_offset < 0) scroll_offset = 0;
}

static void browser_on_resize(int new_w, int new_h) {
    browser_win_w = new_w;
    browser_win_h = new_h;
}

static void browser_on_input(int key) {
    if (key == 0) return;
    
    if (url_active) {
        if (key == '\n') {
            // Navigate
            browser_load_page(url_buf);
        } else if (key == '\b') {
            if (url_cursor > 0) {
                url_cursor--;
                // Shift chars left
                for (int i = url_cursor; url_buf[i]; i++) {
                    url_buf[i] = url_buf[i+1];
                }
            }
        } else if (key == 27) {
            url_active = 0;
        } else if (key >= 32 && key < 127 && url_cursor < 254) {
            // Insert char
            int len = strlen(url_buf);
            for (int i = len; i > url_cursor; i--) {
                url_buf[i] = url_buf[i-1];
            }
            url_buf[url_cursor++] = (char)key;
            url_buf[len + 1] = 0;
        }
    }
}

static void browser_on_mouse(int x, int y, int btn) {
    if (btn != 1) return;
    
    // URL bar click
    if (y >= 0 && y < URL_BAR_H) {
        url_active = 1;
        
        // Calculate Go button position dynamically (same as paint)
        int bx = 6 + 32 + 32 + 36;
        int url_w = browser_win_w - bx - 60;
        if (url_w < 40) url_w = 40;
        int go_x = bx + url_w + 4;
        if (x >= go_x && x <= go_x + 40) {
            browser_load_page(url_buf);
        }
        return;
    }
    
    // Click in content area - deselect URL bar
    if (y > URL_BAR_H) {
        url_active = 0;
    }
}

void init_browser_app() {
    // Initialize with empty page
    page_line_count = 0;
    url_buf[0] = 0;
    url_cursor = 0;
    
    Window* w = fw_create_window("Browser", 600, 420, browser_on_paint, browser_on_input, browser_on_mouse);
    w->min_w = 400;
    
    // Wire up scroll and resize callbacks
    w->scroll_callback = (void*)browser_on_scroll;
    w->resize_callback = (void*)browser_on_resize;
    
    w->menu_count = 3;
    strcpy(w->menus[0].name, "File");
    strcpy(w->menus[0].items[0].label, "New Tab");
    strcpy(w->menus[0].items[1].label, "Close");
    w->menus[0].item_count = 2;
    
    strcpy(w->menus[1].name, "Edit");
    strcpy(w->menus[1].items[0].label, "Copy URL");
    strcpy(w->menus[1].items[1].label, "Paste");
    w->menus[1].item_count = 2;
    
    strcpy(w->menus[2].name, "View");
    strcpy(w->menus[2].items[0].label, "Refresh");
    strcpy(w->menus[2].items[1].label, "Source");
    w->menus[2].item_count = 2;
    
    fw_register_dock("Browser", 5, w);
}
