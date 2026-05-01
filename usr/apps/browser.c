// usr/apps/browser.c - CamelOS Browser App
// Full web browser with URL bar, HTML rendering, HTTPS, bookmarks, and download/install
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/cpu/timer.h"
#include "../../core/tcp.h"
#include "../dock.h"

// Layout
#define URL_BAR_H 36
#define STATUS_BAR_H 22
#define PAD 8
#define BOOKMARK_BAR_H 24

// State
static char url_buf[256] = "";
static int url_cursor = 0;
static int url_active = 1;  // URL bar focused by default

// Page content buffer
#define PAGE_LINES 100
#define PAGE_LINE_LEN 256
static char page_lines[PAGE_LINES][PAGE_LINE_LEN];
static int page_line_count = 0;
static int scroll_offset = 0;
static char status_text[64] = "Ready";

// Loading state
static int is_loading = 0;

// Current window dimensions (updated on resize)
static int browser_win_w = 700;
static int browser_win_h = 500;

// Download/Install state
static int download_progress = 0;     // 0-100
static int download_active = 0;
static char download_filename[64] = "";
static char download_url[256] = "";
static int download_is_app = 0;       // 1 if .app/.cdl/.dmg

// Navigation history
#define HISTORY_MAX 16
static char history[HISTORY_MAX][256];
static int history_pos = -1;
static int history_count = 0;

// Bookmarks
#define BOOKMARK_MAX 8
static struct {
    char name[32];
    char url[256];
} bookmarks[BOOKMARK_MAX] = {
    {"CamelOS",    "http://camelos.local"},
    {"Example",    "http://example.com"},
    {"GitHub",     "http://github.com"},
    {"Wikipedia",  "http://wikipedia.org"},
};
static int bookmark_count = 4;
static int show_bookmarks = 0;

// Page title (extracted from HTML <title>)
static char page_title[64] = "";

// Link detection for clickable links
#define MAX_LINKS 32
static struct {
    int line;
    int col;
    int len;
    char url[256];
} links[MAX_LINKS];
static int link_count = 0;
static int hovered_link = -1;

static void browser_navigate(const char* url);

// ---------- HTTP Fetch with HTTPS support ----------

static void browser_load_page(const char* url) {
    page_line_count = 0;
    scroll_offset = 0;
    is_loading = 1;
    link_count = 0;
    page_title[0] = 0;
    strcpy(status_text, "Loading...");
    
    // Parse URL to extract host, path, and scheme
    int use_tls = 0;
    char host[128] = "";
    char path[128] = "/";
    int port = 80;
    
    const char* url_start = url;
    if (strncmp(url, "https://", 8) == 0) {
        use_tls = 1;
        url_start = url + 8;
        port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        url_start = url + 7;
    }
    
    // Extract host (and optional port)
    int hi = 0;
    while (*url_start && *url_start != '/' && hi < 127) {
        host[hi++] = *url_start++;
    }
    host[hi] = 0;
    
    // Check for port in hostname
    char* colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = 0;
        colon++;
        while (*colon >= '0' && *colon <= '9') {
            port = port * 10 + (*colon - '0');
            colon++;
        }
        if (port == 0) port = use_tls ? 443 : 80;
    }
    
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
        strcpy(page_lines[0], "Error: Could not resolve hostname");
        strcpy(page_lines[1], "");
        snprintf(page_lines[2], PAGE_LINE_LEN, "The server '%s' could not be found.", host);
        page_line_count = 3;
        strcpy(status_text, "DNS Error");
        is_loading = 0;
        return;
    }
    
    // Try TCP connection
    extern uint32_t ip_parse(const char* str);
    uint32_t ip = ip_parse(ip_str);
    
    void* conn = tcp_connect_with_ptr(ip, port);
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
    
    // TLS handshake if HTTPS
    if (use_tls) {
        extern int tls_client_handshake(void* conn);
        int tls_result = tls_client_handshake(conn);
        if (tls_result != 0) {
            strcpy(page_lines[0], "Error: TLS handshake failed");
            strcpy(page_lines[1], "");
            strcpy(page_lines[2], "Could not establish secure connection.");
            page_line_count = 3;
            strcpy(status_text, "TLS Error");
            is_loading = 0;
            return;
        }
    }
    
    // Send HTTP request
    char request[512];
    int rlen = 0;
    rlen += sprintf(request + rlen, "GET %s HTTP/1.1\r\n", path);
    rlen += sprintf(request + rlen, "Host: %s\r\n", host);
    rlen += sprintf(request + rlen, "User-Agent: CamelOS/3.0 (compatible)\r\n");
    rlen += sprintf(request + rlen, "Accept: text/html,application/xhtml+xml,*/*\r\n");
    rlen += sprintf(request + rlen, "Accept-Language: en-US,en;q=0.9\r\n");
    rlen += sprintf(request + rlen, "Connection: close\r\n");
    rlen += sprintf(request + rlen, "\r\n");
    
    if (use_tls) {
        extern int tls_client_send(void* conn, const char* data, int len);
        tls_client_send(conn, request, rlen);
    } else {
        tcp_conn_send(conn, request, rlen);
    }
    
    // Read response
    char response[16384];
    int total_read = 0;
    for (int retry = 0; retry < 200 && total_read < (int)sizeof(response) - 1; retry++) {
        extern void rtl8139_poll();
        rtl8139_poll();
        int n;
        if (use_tls) {
            extern int tls_client_recv(void* conn, char* buf, int len);
            n = tls_client_recv(conn, response + total_read, sizeof(response) - total_read - 1);
        } else {
            n = tcp_conn_recv(conn, response + total_read, sizeof(response) - total_read - 1);
        }
        if (n > 0) total_read += n;
        else if (n == 0) break;
        else {
            for (volatile int d = 0; d < 50000; d++);
        }
    }
    response[total_read] = 0;
    
    // Parse HTTP status
    int http_status = 0;
    if (strncmp(response, "HTTP/", 5) == 0) {
        char* sp = strchr(response, ' ');
        if (sp) http_status = atoi(sp + 1);
    }
    
    // Handle redirects (301, 302, 307)
    if (http_status == 301 || http_status == 302 || http_status == 307) {
        char* location = strstr(response, "Location: ");
        if (location) {
            location += 10;
            char redirect_url[256];
            int ri = 0;
            while (location[ri] && location[ri] != '\r' && location[ri] != '\n' && ri < 255) {
                redirect_url[ri] = location[ri];
                ri++;
            }
            redirect_url[ri] = 0;
            strcpy(status_text, "Redirecting...");
            browser_navigate(redirect_url);
            return;
        }
    }
    
    // Parse response - skip HTTP headers
    char* body = strstr(response, "\r\n\r\n");
    if (body) body += 4;
    else body = response;
    
    // Extract <title> from HTML
    char* title_start = strstr(body, "<title>");
    if (title_start) {
        title_start += 7;
        char* title_end = strstr(title_start, "</title>");
        if (title_end) {
            int tlen = title_end - title_start;
            if (tlen > 63) tlen = 63;
            strncpy(page_title, title_start, tlen);
            page_title[tlen] = 0;
        }
    }
    
    // Enhanced HTML to text conversion
    int line = 0;
    int col = 0;
    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int in_link = 0;
    char link_url[256];
    int link_start_line = 0;
    int link_start_col = 0;
    
    for (int i = 0; body[i] && line < PAGE_LINES; i++) {
        char c = body[i];
        
        if (c == '<') {
            in_tag = 1;
            // Check for script/style tags to skip
            if (strncmp(body + i, "<script", 7) == 0) in_script = 1;
            if (strncmp(body + i, "<style", 6) == 0) in_style = 1;
            if (strncmp(body + i, "</script>", 9) == 0) in_script = 0;
            if (strncmp(body + i, "</style>", 8) == 0) in_style = 0;
            
            // Handle <a href="..."> links
            if (strncmp(body + i, "<a ", 3) == 0 || strncmp(body + i, "<A ", 3) == 0) {
                char* href = strstr(body + i, "href=\"");
                if (!href) href = strstr(body + i, "HREF=\"");
                if (href && href - (body + i) < 200) {
                    href += 6;
                    int li = 0;
                    while (href[li] && href[li] != '"' && li < 255) {
                        link_url[li] = href[li];
                        li++;
                    }
                    link_url[li] = 0;
                    in_link = 1;
                    link_start_line = line;
                    link_start_col = col;
                }
            }
            if (strncmp(body + i, "</a>", 4) == 0 || strncmp(body + i, "</A>", 4) == 0) {
                if (in_link && link_count < MAX_LINKS) {
                    links[link_count].line = link_start_line;
                    links[link_count].col = link_start_col;
                    links[link_count].len = col - link_start_col;
                    strncpy(links[link_count].url, link_url, 255);
                    links[link_count].url[255] = 0;
                    link_count++;
                }
                in_link = 0;
            }
            
            // Handle block-level tags (add line breaks)
            if (strncmp(body + i, "<br", 3) == 0 || strncmp(body + i, "<BR", 3) == 0 ||
                strncmp(body + i, "<p", 2) == 0 || strncmp(body + i, "<P", 2) == 0 ||
                strncmp(body + i, "<div", 4) == 0 || strncmp(body + i, "<DIV", 4) == 0 ||
                strncmp(body + i, "<h1", 3) == 0 || strncmp(body + i, "<h2", 3) == 0 ||
                strncmp(body + i, "<h3", 3) == 0 || strncmp(body + i, "<h4", 3) == 0 ||
                strncmp(body + i, "<li", 3) == 0 || strncmp(body + i, "<LI", 3) == 0 ||
                strncmp(body + i, "<tr", 3) == 0 || strncmp(body + i, "<TR", 3) == 0) {
                if (col > 0) {
                    page_lines[line][col] = 0;
                    line++;
                    col = 0;
                }
            }
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag || in_script || in_style) continue;
        
        // Handle HTML entities
        if (c == '&') {
            if (strncmp(body + i, "&amp;", 5) == 0) { c = '&'; i += 4; }
            else if (strncmp(body + i, "&lt;", 4) == 0) { c = '<'; i += 3; }
            else if (strncmp(body + i, "&gt;", 4) == 0) { c = '>'; i += 3; }
            else if (strncmp(body + i, "&nbsp;", 6) == 0) { c = ' '; i += 5; }
            else if (strncmp(body + i, "&quot;", 6) == 0) { c = '"'; i += 5; }
            else if (strncmp(body + i, "&#39;", 5) == 0) { c = '\''; i += 4; }
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
    if (use_tls) strcat(status_text, " [TLS]");
    is_loading = 0;
}

static void browser_navigate(const char* url) {
    // Push to history
    if (history_pos < HISTORY_MAX - 1) {
        history_pos++;
        strncpy(history[history_pos], url, 255);
        history[history_pos][255] = 0;
        history_count = history_pos + 1;
    }
    
    // Update URL bar
    strncpy(url_buf, url, 255);
    url_buf[255] = 0;
    url_cursor = strlen(url_buf);
    
    browser_load_page(url);
}

static void browser_go_back(void) {
    if (history_pos > 0) {
        history_pos--;
        strncpy(url_buf, history[history_pos], 255);
        url_buf[255] = 0;
        url_cursor = strlen(url_buf);
        browser_load_page(url_buf);
    }
}

static void browser_go_forward(void) {
    if (history_pos < history_count - 1) {
        history_pos++;
        strncpy(url_buf, history[history_pos], 255);
        url_buf[255] = 0;
        url_cursor = strlen(url_buf);
        browser_load_page(url_buf);
    }
}

// ---------- Download ----------

static void browser_download_file(const char* url) {
    // Parse URL
    int use_tls = 0;
    char host[128] = "";
    char path[128] = "/";
    int port = 80;
    
    const char* url_start = url;
    if (strncmp(url, "https://", 8) == 0) { use_tls = 1; url_start = url + 8; port = 443; }
    else if (strncmp(url, "http://", 7) == 0) url_start = url + 7;
    
    int hi = 0;
    while (*url_start && *url_start != '/' && hi < 127) host[hi++] = *url_start++;
    host[hi] = 0;
    if (*url_start == '/') { strncpy(path, url_start, 127); path[127] = 0; }
    
    // Extract filename
    const char* last_slash = strrchr(path, '/');
    const char* fname = last_slash ? last_slash + 1 : path;
    if (!fname[0]) fname = "download.dat";
    strncpy(download_filename, fname, 63);
    download_filename[63] = 0;
    
    int flen = strlen(download_filename);
    download_is_app = (flen > 4 && (
        strcmp(download_filename + flen - 4, ".app") == 0 ||
        strcmp(download_filename + flen - 4, ".cdl") == 0 ||
        strcmp(download_filename + flen - 4, ".dmg") == 0));
    
    // DNS + TCP (simplified - reuse the same pattern as load_page)
    char ip_str[16];
    extern int dns_resolve(const char* name, char* ip_buf, int ip_buf_len);
    if (dns_resolve(host, ip_str, sizeof(ip_str)) != 0) {
        strcpy(status_text, "Download: DNS Error");
        return;
    }
    
    extern uint32_t ip_parse(const char* str);
    uint32_t ip = ip_parse(ip_str);
    void* conn = tcp_connect_with_ptr(ip, port);
    if (!conn) { strcpy(status_text, "Download: Connect Error"); return; }
    
    uint32_t start = get_tick_count();
    while (get_tick_count() - start < 5000) {
        extern void rtl8139_poll();
        rtl8139_poll();
        if (tcp_conn_is_established(conn)) goto connected;
    }
    strcpy(status_text, "Download: Timeout");
    return;
    
connected:
    if (use_tls) {
        extern int tls_client_handshake(void* conn);
        if (tls_client_handshake(conn) != 0) {
            strcpy(status_text, "Download: TLS Error");
            return;
        }
    }
    
    char request[512];
    int rlen = sprintf(request, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CamelOS/3.0\r\nConnection: close\r\n\r\n", path, host);
    
    if (use_tls) {
        extern int tls_client_send(void* conn, const char* data, int len);
        tls_client_send(conn, request, rlen);
    } else {
        tcp_conn_send(conn, request, rlen);
    }
    
    download_active = 1;
    download_progress = 10;
    strcpy(status_text, "Downloading...");
    
    char response[16384];
    int total_read = 0;
    for (int retry = 0; retry < 200 && total_read < (int)sizeof(response) - 1; retry++) {
        extern void rtl8139_poll();
        rtl8139_poll();
        int n;
        if (use_tls) {
            extern int tls_client_recv(void* conn, char* buf, int len);
            n = tls_client_recv(conn, response + total_read, sizeof(response) - total_read - 1);
        } else {
            n = tcp_conn_recv(conn, response + total_read, sizeof(response) - total_read - 1);
        }
        if (n > 0) { total_read += n; download_progress = 10 + (total_read * 80) / (int)sizeof(response); }
        else if (n == 0) break;
        else { for (volatile int d = 0; d < 50000; d++); }
    }
    response[total_read] = 0;
    
    char* body = strstr(response, "\r\n\r\n");
    int body_len = 0;
    if (body) { body += 4; body_len = total_read - (body - response); }
    else { body = response; body_len = total_read; }
    
    char save_path[128];
    if (download_is_app) {
        strcpy(save_path, "/tmp/");
        strcat(save_path, download_filename);
    } else {
        extern char g_desktop_path[128];
        strcpy(save_path, g_desktop_path);
        strcat(save_path, "/");
        strcat(save_path, download_filename);
    }
    
    int res = sys_fs_write(save_path, body, body_len);
    download_progress = 100;
    
    if (res >= 0) {
        strcpy(status_text, "Downloaded: ");
        strcat(status_text, download_filename);
        if (download_is_app) {
            extern void desktop_install_app(const char*);
            desktop_install_app(save_path);
            strcat(status_text, " (installed)");
        }
    } else {
        strcpy(status_text, "Download: Save Error");
    }
    download_active = 0;
}

// ---------- Rendering ----------

static void browser_on_paint(int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
    
    // URL Bar
    gfx_fill_rect(x, y, w, URL_BAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, y + URL_BAR_H - 1, w, 1, 0xFFC6C6C8);
    
    // Navigation buttons
    int bx = x + 6;
    // Back button
    int can_back = (history_pos > 0);
    gfx_fill_rounded_rect(bx, y + 6, 26, 24, can_back ? 0xFFE8E8ED : 0xFFF0F0F0, 4);
    gfx_draw_string(bx + 7, y + 11, "<", can_back ? 0xFF555555 : 0xFFCCCCCC);
    bx += 30;
    // Forward button
    int can_fwd = (history_pos < history_count - 1);
    gfx_fill_rounded_rect(bx, y + 6, 26, 24, can_fwd ? 0xFFE8E8ED : 0xFFF0F0F0, 4);
    gfx_draw_string(bx + 7, y + 11, ">", can_fwd ? 0xFF555555 : 0xFFCCCCCC);
    bx += 30;
    // Refresh button
    gfx_fill_rounded_rect(bx, y + 6, 26, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 7, y + 11, "R", 0xFF555555);
    bx += 30;
    // Home button
    gfx_fill_rounded_rect(bx, y + 6, 26, 24, 0xFFE8E8ED, 4);
    gfx_draw_string(bx + 5, y + 11, "H", 0xFF555555);
    bx += 34;
    
    // URL input field
    int url_w = w - (bx - x) - 100;
    if (url_w < 60) url_w = 60;
    gfx_fill_rounded_rect(bx, y + 6, url_w, 24, 0xFFFFFFFF, 4);
    gfx_draw_rect(bx, y + 6, url_w, 24, url_active ? 0xFF007AFF : 0xFFC6C6C8);
    
    // Lock icon for HTTPS
    int text_x = bx + 6;
    if (strncmp(url_buf, "https://", 8) == 0) {
        gfx_draw_string(text_x, y + 11, "Lock", 0xFF34C759);
        text_x += 32;
    }
    
    // URL text (clip to field width)
    int max_chars = (url_w - 12) / 8;
    int url_len = strlen(url_buf);
    int scroll_chars = 0;
    if (url_cursor > max_chars) scroll_chars = url_cursor - max_chars + 5;
    char display_url[256];
    strncpy(display_url, url_buf + scroll_chars, max_chars);
    display_url[max_chars] = 0;
    gfx_draw_string(text_x, y + 11, display_url, 0xFF333333);
    
    // Cursor
    if (url_active) {
        static int blink = 0; blink++;
        if (blink % 60 < 30) {
            int cur_x = text_x + (url_cursor - scroll_chars) * 8;
            if (cur_x < bx + url_w - 4)
                gfx_fill_rect(cur_x, y + 11, 1, 14, 0xFF007AFF);
        }
    }
    
    // Go button
    int go_x = bx + url_w + 4;
    gfx_fill_rounded_rect(go_x, y + 6, 28, 24, 0xFF007AFF, 4);
    gfx_draw_string(go_x + 7, y + 11, "Go", 0xFFFFFFFF);
    
    // Download button (DL)
    int dl_x = go_x + 32;
    gfx_fill_rounded_rect(dl_x, y + 6, 28, 24, 0xFF34C759, 4);
    gfx_draw_string(dl_x + 4, y + 11, "DL", 0xFFFFFFFF);
    
    // Bookmark button
    int bm_x = dl_x + 32;
    gfx_fill_rounded_rect(bm_x, y + 6, 22, 24, show_bookmarks ? 0xFFFF9500 : 0xFFE8E8ED, 4);
    gfx_draw_string(bm_x + 4, y + 11, "*", show_bookmarks ? 0xFFFFFFFF : 0xFF555555);
    
    // Bookmark bar (when toggled)
    int content_y_start = y + URL_BAR_H;
    if (show_bookmarks) {
        gfx_fill_rect(x, content_y_start, w, BOOKMARK_BAR_H, 0xFFFFF8E8);
        gfx_draw_rect(x, content_y_start + BOOKMARK_BAR_H - 1, w, 1, 0xFFE0D8C0);
        int bmx = x + 8;
        for (int bi = 0; bi < bookmark_count && bmx < x + w - 60; bi++) {
            int bm_w = strlen(bookmarks[bi].name) * 8 + 16;
            if (bmx + bm_w > x + w - 10) break;
            gfx_fill_rounded_rect(bmx, content_y_start + 3, bm_w, 18, 0xFFFFF0D0, 3);
            gfx_draw_rect(bmx, content_y_start + 3, bm_w, 18, 0xFFE0D8C0);
            gfx_draw_string(bmx + 8, content_y_start + 6, bookmarks[bi].name, 0xFF8B6914);
            bmx += bm_w + 4;
        }
        content_y_start += BOOKMARK_BAR_H;
    }
    
    // Page content
    int content_h = y + h - content_y_start - STATUS_BAR_H;
    if (content_h < 0) content_h = 0;
    int visible_lines = content_h / 16;
    
    // Clamp scroll offset
    if (scroll_offset < 0) scroll_offset = 0;
    int max_scroll = page_line_count - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    
    // Find hovered link
    hovered_link = -1;
    int mmx, mmy, mml;
    sys_mouse_read(&mmx, &mmy, &mml);
    // (Hover detection done in mouse callback)
    
    for (int i = 0; i < visible_lines && (i + scroll_offset) < page_line_count; i++) {
        int ly = content_y_start + i * 16;
        int line_idx = i + scroll_offset;
        
        // Check if this line has a link - draw in blue/underline
        int is_link_line = 0;
        for (int li = 0; li < link_count; li++) {
            if (links[li].line == line_idx) {
                is_link_line = 1;
                break;
            }
        }
        
        if (is_link_line) {
            gfx_draw_string(x + PAD, ly, page_lines[line_idx], 0xFF007AFF);
            // Underline
            int tw = strlen(page_lines[line_idx]) * 8;
            gfx_fill_rect(x + PAD, ly + 14, tw, 1, 0xFF007AFF);
        } else {
            gfx_draw_string(x + PAD, ly, page_lines[line_idx], 0xFF333333);
        }
    }
    
    // Empty state
    if (page_line_count == 0) {
        gfx_draw_string_centered(x + w/2, y + h/2 - 20, "CamelOS Browser", 0xFF999999, 1);
        gfx_draw_string_centered(x + w/2, y + h/2 + 4, "Enter a URL and press Go", 0xFFCCCCCC, 1);
    }
    
    // Scrollbar
    if (page_line_count > visible_lines) {
        int sb_x = x + w - 10;
        int sb_h = content_h;
        int thumb_h = (visible_lines * sb_h) / page_line_count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y_pos = content_y_start + (scroll_offset * (sb_h - thumb_h)) / (page_line_count - visible_lines);
        gfx_fill_rect(sb_x, content_y_start, 8, sb_h, 0x10C0C0C0);
        gfx_fill_rounded_rect(sb_x + 1, thumb_y_pos, 6, thumb_h, 0xFFC0C0C0, 3);
    }
    
    // Status bar
    int status_y = y + h - STATUS_BAR_H;
    gfx_fill_rect(x, status_y, w, STATUS_BAR_H, 0xFFF2F2F7);
    gfx_draw_rect(x, status_y, w, 1, 0xFFC6C6C8);
    gfx_draw_string(x + 8, status_y + 5, status_text, 0xFF888888);
    
    // Loading indicator
    if (is_loading) {
        static int dots = 0; dots++;
        int nd = (dots / 20) % 4;
        char loading[16] = "Loading";
        for (int d = 0; d < nd; d++) strcat(loading, ".");
        gfx_draw_string(x + w - 100, status_y + 5, loading, 0xFF007AFF);
    }
    
    // Download progress bar
    if (download_active || download_progress == 100) {
        int bar_x = x + 8;
        int bar_y = status_y + 16;
        int bar_w = w - 16;
        gfx_fill_rect(bar_x, bar_y, bar_w, 3, 0xFFE0E0E0);
        int fill_w = (bar_w * download_progress) / 100;
        uint32_t bar_col = download_progress >= 100 ? 0xFF34C759 : 0xFF007AFF;
        if (fill_w > 0) gfx_fill_rect(bar_x, bar_y, fill_w, 3, bar_col);
    }
}

// ---------- Event Handlers ----------

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
            browser_navigate(url_buf);
        } else if (key == '\b') {
            if (url_cursor > 0) {
                url_cursor--;
                for (int i = url_cursor; url_buf[i]; i++) url_buf[i] = url_buf[i+1];
            }
        } else if (key == 27) {
            url_active = 0;
        } else if (key >= 32 && key < 127 && url_cursor < 254) {
            int len = strlen(url_buf);
            for (int i = len; i > url_cursor; i--) url_buf[i] = url_buf[i-1];
            url_buf[url_cursor++] = (char)key;
            url_buf[len + 1] = 0;
        }
    } else {
        // Content area shortcuts
        if (key == 'l' || key == 'L') {
            url_active = 1;  // Focus URL bar (like Ctrl+L)
        } else if (key == 'b' || key == 'B') {
            browser_go_back();
        } else if (key == 'f' || key == 'F') {
            browser_go_forward();
        } else if (key == 'r' || key == 'R') {
            if (history_pos >= 0) browser_load_page(url_buf);
        }
    }
}

static void browser_on_mouse(int lx, int ly, int btn) {
    if (btn != 1) return;
    
    // URL bar click
    if (ly >= 0 && ly < URL_BAR_H) {
        url_active = 1;
        
        int bx = 6 + 30 + 30 + 30 + 34;
        int url_w = browser_win_w - bx - 100;
        if (url_w < 60) url_w = 60;
        int go_x = bx + url_w + 4;
        int dl_x = go_x + 32;
        int bm_x = dl_x + 32;
        
        // Back button
        if (lx >= 6 && lx <= 32) { browser_go_back(); return; }
        // Forward button
        if (lx >= 36 && lx <= 62) { browser_go_forward(); return; }
        // Refresh
        if (lx >= 66 && lx <= 92) { if (history_pos >= 0) browser_load_page(url_buf); return; }
        // Home
        if (lx >= 96 && lx <= 122) { browser_navigate("http://camelos.local"); return; }
        // Go button
        if (lx >= go_x && lx <= go_x + 28) { browser_navigate(url_buf); return; }
        // Download
        if (lx >= dl_x && lx <= dl_x + 28) { browser_download_file(url_buf); return; }
        // Bookmark toggle
        if (lx >= bm_x && lx <= bm_x + 22) { show_bookmarks = !show_bookmarks; return; }
        return;
    }
    
    // Bookmark bar click
    int content_y_start = URL_BAR_H;
    if (show_bookmarks) {
        if (ly >= content_y_start && ly < content_y_start + BOOKMARK_BAR_H) {
            int bmx = 8;
            for (int bi = 0; bi < bookmark_count; bi++) {
                int bm_w = strlen(bookmarks[bi].name) * 8 + 16;
                if (lx >= bmx && lx <= bmx + bm_w) {
                    browser_navigate(bookmarks[bi].url);
                    return;
                }
                bmx += bm_w + 4;
            }
            return;
        }
        content_y_start += BOOKMARK_BAR_H;
    }
    
    // Click on content area - check for link clicks
    if (ly > content_y_start) {
        url_active = 0;
        
        int click_line = (ly - content_y_start) / 16 + scroll_offset;
        if (click_line >= 0 && click_line < page_line_count) {
            // Check if this line has a clickable link
            for (int li = 0; li < link_count; li++) {
                if (links[li].line == click_line) {
                    int link_x_start = PAD + links[li].col * 8;
                    int link_x_end = link_x_start + links[li].len * 8;
                    if (lx >= link_x_start && lx <= link_x_end) {
                        browser_navigate(links[li].url);
                        return;
                    }
                }
            }
        }
    }
}

// ---------- Init ----------

void init_browser_app() {
    page_line_count = 0;
    url_buf[0] = 0;
    url_cursor = 0;
    history_pos = -1;
    history_count = 0;
    
    Window* w = fw_create_window("Browser", 700, 500, browser_on_paint, browser_on_input, browser_on_mouse);
    w->min_w = 400;
    
    // Wire up scroll and resize callbacks
    w->scroll_callback = (void*)browser_on_scroll;
    w->resize_callback = (void*)browser_on_resize;
    
    w->menu_count = 4;
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
    
    strcpy(w->menus[3].name, "Bookmarks");
    strcpy(w->menus[3].items[0].label, "Show Bookmarks");
    strcpy(w->menus[3].items[1].label, "Add Current");
    w->menus[3].item_count = 2;
    
    fw_register_dock("Browser", 5, w);
}
