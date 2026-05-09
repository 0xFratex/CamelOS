// core/http.c - Optimized HTTP client implementation with async support, TLS, and loading animations
#include "http.h"
#include "socket.h"
#include "string.h"
#include "memory.h"
#include "dns.h"
#include "net.h"
#include "tls.h"
#include "firewall.h"
#include "window_server.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/video/loading_animation.h"
#include "../hal/cpu/timer.h"
#include "../usr/framework.h"

#define HTTP_BUFFER_SIZE (8192 + 16)
#define HTTP_MAX_REDIRECTS 5
#define HTTP_TIMEOUT 15000 // 15 seconds - increased to get full page content
#define HTTP_MAX_URL_LEN 1024
#define HTTP_MAX_HEADERS_SIZE (8192 + 16)

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production
// ============================================================================
#define HTTP_DEBUG_ENABLED     0

// External references for event processing
extern void rtl8139_poll(void);
extern window_t* active_win;  // From window_server.c
extern void s_printf(const char* fmt, ...);

// TLS session for HTTPS connections
static tls_session_t* current_tls_session = NULL;

// Global loading state for UI feedback
static http_loading_state_t http_loading_state = {
    .is_loading = 0,
    .phase = HTTP_PHASE_IDLE,
    .bytes_received = 0,
    .total_bytes = 0,
    .status_text = "",
    .progress_callback = NULL,
    .user_data = NULL
};

// Forward declaration for internal request function
static int http_get_internal(const char* url, char* response, int response_size,
                             const char** headers, int header_count, int redirect_count,
                             http_progress_cb progress_cb, void* user_data);

// Get the current loading state
http_loading_state_t* http_get_loading_state(void) {
    return &http_loading_state;
}

// Draw a loading overlay on the window content area
static void __attribute__((unused)) draw_loading_overlay(int x, int y, int w, int h) {
    // Semi-transparent overlay background
    (void)0;  // overlay_bg removed
    
    // Draw overlay
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            if (px >= 0 && px < 1024 && py >= 0 && py < 768) {
                uint32_t* buf = gfx_get_active_buffer();
                if (buf) {
                    int idx = py * 1024 + px;  // Assuming 1024 width
                    uint32_t bg = buf[idx];
                    // Alpha blend
                    uint8_t bg_r = (bg >> 16) & 0xFF;
                    uint8_t bg_g = (bg >> 8) & 0xFF;
                    uint8_t bg_b = bg & 0xFF;

                    uint8_t r = (bg_r * 128 + 0xFF * 127) / 255;
                    uint8_t g = (bg_g * 128 + 0xFF * 127) / 255;
                    uint8_t b = (bg_b * 128 + 0xFF * 127) / 255;

                    buf[idx] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }
        }
    }
    
    // Draw spinner in center
    int center_x = x + w / 2;
    int center_y = y + h / 2;
    int radius = 20;
    
    // Spinning animation
    static int spinner_frame = 0;
    spinner_frame = (spinner_frame + 1) % 12;
    
    draw_spinner(center_x, center_y, radius, 0xFF4A90D9, spinner_frame);
    
    // Draw status text below spinner
    if (http_loading_state.status_text[0]) {
        int text_y = center_y + radius + 20;
        gfx_draw_string(center_x - strlen(http_loading_state.status_text) * 4, text_y, 
                       http_loading_state.status_text, 0xFF333333);
    }
    
    // Draw progress bar if we have content length
    if (http_loading_state.total_bytes > 0) {
        int bar_width = w / 2;
        int bar_height = 8;
        int bar_x = center_x - bar_width / 2;
        int bar_y = center_y + radius + 40;
        
        draw_progress_bar(bar_x, bar_y, bar_width, bar_height,
                         http_loading_state.bytes_received, http_loading_state.total_bytes,
                         0xFF4A90D9, 0xFFE0E0E0);
        
        // Draw percentage
        char percent_text[16];
        int percent = (http_loading_state.bytes_received * 100) / http_loading_state.total_bytes;
        snprintf(percent_text, sizeof(percent_text), "%d%%", percent);
        gfx_draw_string(center_x - strlen(percent_text) * 4, bar_y + 12, percent_text, 0xFF666666);
    }
}

// Full event processing during HTTP requests - keeps system responsive
void http_process_events(void) {
    rtl8139_poll();  // Poll network card for incoming packets

    // Poll USB HID and PS/2 mouse so the system doesn't appear frozen
    // while the browser is loading a page (fixes "browser freezes" bug).
    extern void usb_hid_poll(void);
    usb_hid_poll();
    extern void mouse_poll_fallback(void);
    mouse_poll_fallback();

    // Consume any pending keyboard/mouse events so the input queue
    // doesn't overflow during long network operations.
    extern int sys_get_key(void);
    while (sys_get_key() != 0) { /* drain */ }

    // Minimal sleep to allow other interrupts to fire and prevent
    // the CPU from being consumed 100% by polling loops.
    // Using 1 tick (~20ms) which is short enough for responsiveness
    // but long enough to let the scheduler and other IRQs run.
    timer_sleep(1);
}

// Parse URL - returns 1 for HTTPS, 0 for HTTP
static int http_parse_url(const char* url, char* host, char* path, uint16_t* port) {
    const char* proto = strstr(url, "://");
    const char* start;
    int is_https = 0;

    // Initialize outputs with safe defaults
    host[0] = '\0';
    strcpy(path, "/");
    *port = 80;

    if (proto) {
        // Check if HTTPS
        if (proto - url >= 5 && strncmp(url, "https", 5) == 0) {
            is_https = 1;
            *port = 443;
        }
        start = proto + 3;
    } else {
        start = url;
    }

    // Find host end
    const char* path_start = strchr(start, '/');
    const char* port_start = strchr(start, ':');

    // Extract path safely
    if (path_start) {
        int path_len = strlen(path_start);
        if (path_len >= HTTP_MAX_URL_LEN) path_len = HTTP_MAX_URL_LEN - 1;
        strncpy(path, path_start, path_len);
        path[path_len] = '\0';
    }

    // Extract host and port
    int host_len;
    if (port_start && (!path_start || port_start < path_start)) {
        host_len = port_start - start;
        
        // Robust inline port parser instead of external atoi
        int parsed_port = 0;
        const char* p_str = port_start + 1;
        while (*p_str >= '0' && *p_str <= '9') {
            parsed_port = parsed_port * 10 + (*p_str - '0');
            p_str++;
        }
        *port = parsed_port;
        
        // Validate port
        if (*port == 0) *port = is_https ? 443 : 80;
    } else if (path_start) {
        host_len = path_start - start;
    } else {
        host_len = strlen(start);
    }
    
    // Validate and copy host
    if (host_len <= 0 || host_len >= HTTP_MAX_URL_LEN) {
        host_len = 0;
    }
    strncpy(host, start, host_len);
    host[host_len] = '\0';

    return is_https;
}

// Convert IP string to network byte order uint32_t
static uint32_t http_inet_addr(const char* ip_str) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    int num = 0;
    int byte_idx = 0;
    
    while (*ip_str && byte_idx < 4) {
        if (*ip_str == '.') {
            bytes[byte_idx++] = num;
            num = 0;
        } else if (*ip_str >= '0' && *ip_str <= '9') {
            num = num * 10 + (*ip_str - '0');
        }
        ip_str++;
    }
    if (byte_idx < 4) bytes[byte_idx] = num;
    
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | 
           ((uint32_t)bytes[2] << 8) | ((uint32_t)bytes[3]);
}

// HTTP GET request - Supports both HTTP and HTTPS with redirect handling
int http_get(const char* url, char* response, int response_size,
             const char** headers, int header_count) {
    return http_get_internal(url, response, response_size, headers, header_count, 0, NULL, NULL);
}

// Cancel current request
void http_cancel_request(void) {
    http_loading_state.is_loading = 0;
    http_loading_state.phase = HTTP_PHASE_IDLE;
    http_loading_state.bytes_received = 0;
    http_loading_state.total_bytes = 0;
    http_loading_state.status_text[0] = 0;
}

// Robust case-insensitive header extraction helper
static char* http_get_header(const char* headers, const char* name) {
    if (!headers || !name) return NULL;
    int name_len = 0;
    while(name[name_len]) name_len++;
    const char* p = headers;
    int iterations = 0;
    while (*p && iterations++ < 1000) {
        int match = 1;
        for (int i = 0; i < name_len; i++) {
            if (!p[i]) { match = 0; break; }
            char c1 = p[i];
            char c2 = name[i];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) {
                match = 0;
                break;
            }
        }
        if (match) {
            char* val = (char*)(p + name_len);
            if (!val) return NULL;  // Safety check
            while (*val == ' ' || *val == '\t') val++;
            return val;
        }
        // Move to next line
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}

// Helper to decode chunked transfer encoding in place
static int http_decode_chunked_in_place(char* buffer, int len) {
    char* src = buffer;
    char* dst = buffer;
    int total_len = 0;
    
    while (src - buffer < len) {
        // Read chunk size (hex)
        while (*src && (*src == '\r' || *src == '\n' || *src == ' ')) src++;
        if (!*src || src - buffer >= len) break;

        char hex[16]; int hi = 0;
        while (*src && *src != '\r' && *src != '\n' && *src != ';' && hi < 15) {
            hex[hi++] = *src++;
        }
        hex[hi] = 0;
        
        // Convert hex string to int
        int chunk_size = 0;
        for (int i = 0; hex[i]; i++) {
            char c = hex[i];
            int v = 0;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            chunk_size = (chunk_size << 4) | v;
        }
        
        // Skip till end of chunk size line
        while (*src && *src != '\r' && *src != '\n') src++;
        while (*src == '\r' || *src == '\n') src++;
        
        if (chunk_size <= 0) break;
        
        // Copy data
        int remain = len - (src - buffer);
        if (chunk_size > remain) chunk_size = remain;
        
        for (int i = 0; i < chunk_size; i++) {
            *dst++ = *src++;
        }
        total_len += chunk_size;
        
        // Skip CRLF after chunk
        while (*src == '\r' || *src == '\n') src++;
    }
    *dst = 0;
    return total_len;
}

// Internal HTTP GET with redirect tracking and loading state
static int http_get_internal(const char* url, char* response, int response_size,
                              const char** headers, int header_count, int redirect_count,
                              http_progress_cb progress_cb, void* user_data) {
    // CRITICAL: Moved large arrays from stack to heap to prevent stack overflow
    // Kernel stack is only 16KB, and these arrays totaled ~7KB + deep call chain = crash
    char* current_url = (char*)kmalloc(HTTP_MAX_URL_LEN * 2);
    if (!current_url) {
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        return -1;
    }
    strncpy(current_url, url, HTTP_MAX_URL_LEN * 2 - 1);
    current_url[HTTP_MAX_URL_LEN * 2 - 1] = '\0';
    int cur_redirect = redirect_count;
    int tried_http_fallback = 0;  // Track if we fell back from HTTPS to HTTP

    char* buffer = NULL;
    char* headers_buffer = NULL;
    // Heap-allocate host and path to save ~2KB of stack per iteration
    char* host = (char*)kmalloc(HTTP_MAX_URL_LEN);
    char* path = (char*)kmalloc(HTTP_MAX_URL_LEN);
    char* redirect_url = (char*)kmalloc(HTTP_MAX_URL_LEN);
    if (!host || !path || !redirect_url) {
        if (current_url) kfree(current_url);
        if (host) kfree(host);
        if (path) kfree(path);
        if (redirect_url) kfree(redirect_url);
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        return -1;
    }
    memset(redirect_url, 0, HTTP_MAX_URL_LEN);

    int total_received = 0;

    while (cur_redirect <= HTTP_MAX_REDIRECTS) {
    {
        uint16_t port;
        int is_https;

    // Check redirect limit
    if (cur_redirect > HTTP_MAX_REDIRECTS) {
        http_loading_state.phase = HTTP_PHASE_ERROR;
        strcpy(http_loading_state.status_text, "Too many redirects");
        kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
        if (buffer) kfree(buffer);
        if (headers_buffer) kfree(headers_buffer);
        return -1;
    }

    // Set loading state
    http_loading_state.is_loading = 1;
    http_loading_state.phase = HTTP_PHASE_DNS;
    http_loading_state.bytes_received = 0;
    http_loading_state.total_bytes = 0;
    strcpy(http_loading_state.status_text, "Resolving host...");
    http_loading_state.progress_callback = progress_cb;
    http_loading_state.user_data = user_data;

    is_https = http_parse_url(current_url, host, path, &port);
    if (is_https < 0) {
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
        return -1;
    }

    // Update status with host name
    snprintf(http_loading_state.status_text, sizeof(http_loading_state.status_text), 
             "Connecting to %s...", host);

    // Resolve hostname
    char ip_str[32];
    if (dns_resolve(host, ip_str, sizeof(ip_str)) < 0) {
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        strcpy(http_loading_state.status_text, "DNS lookup failed");
        kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
        return -1;
    }

    // Check firewall for outgoing connection
    uint32_t dst_ip = http_inet_addr(ip_str);
    if (firewall_is_enabled()) {
        if (firewall_check_outgoing(net_get_ip(), 0, dst_ip, port, FW_PROTO_TCP) == FW_ACTION_BLOCK) {
            http_loading_state.is_loading = 0;
            http_loading_state.phase = HTTP_PHASE_ERROR;
            strcpy(http_loading_state.status_text, "Blocked by firewall");
            kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
            return -1;
        }
    }

    http_loading_state.phase = HTTP_PHASE_CONNECTING;
    http_process_events();

    // Create socket
    int sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
        return -1;
    }

    // Connect
    sockaddr_in_t server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = dst_ip;

    if (k_connect(sockfd, &server_addr) < 0) {
        k_close(sockfd);
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        strcpy(http_loading_state.status_text, "Connection failed");
        kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
        return -1;
    }

    // TLS handshake for HTTPS
    tls_session_t* tls_session = NULL;
    if (is_https) {
        http_loading_state.phase = HTTP_PHASE_TLS_HANDSHAKE;
        strcpy(http_loading_state.status_text, "Establishing secure connection...");
        http_process_events();
        
        tls_session = tls_create_session();
        if (!tls_session) {
            k_close(sockfd);
            http_loading_state.is_loading = 0;
            http_loading_state.phase = HTTP_PHASE_ERROR;
            kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
            return -1;
        }
        
        tls_session->socket_fd = sockfd;
        tls_set_hostname(tls_session, host);
        tls_set_verify(tls_session, 0);  // Skip strict cert verification for broader compatibility
        
        // Perform TLS handshake
        int tls_result = tls_connect(tls_session, host, port);
        if (tls_result != 0) {
            // TLS failed - try fallback to HTTP on port 80
            // Clear socket_fd first so tls_destroy_session doesn't close our socket
            tls_session->socket_fd = -1;
            tls_destroy_session(tls_session);
            k_close(sockfd);

            // Fallback: try HTTP on port 80
            http_loading_state.phase = HTTP_PHASE_CONNECTING;
            strcpy(http_loading_state.status_text, "Falling back to HTTP...");
            http_process_events();

            sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                http_loading_state.is_loading = 0;
                http_loading_state.phase = HTTP_PHASE_ERROR;
                kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
                return -1;
            }

            server_addr.sin_port = htons(80);
            if (k_connect(sockfd, &server_addr) < 0) {
                k_close(sockfd);
                http_loading_state.is_loading = 0;
                http_loading_state.phase = HTTP_PHASE_ERROR;
                kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
                return -1;
            }

            is_https = 0;  // Continue with HTTP
            tls_session = NULL;
            tried_http_fallback = 1;  // Mark that we fell back to HTTP

            // CRITICAL WARNING: TLS handshake failed — falling back to plaintext HTTP.
            // This means the connection is UNENCRYPTED. The page content will still
            // be fetched, but any sensitive data is transmitted in the clear.
            // This fallback exists because CamelOS's TLS stack has limited ECDH support.
            s_printf("[HTTP] WARNING: TLS handshake failed - falling back to HTTP (unencrypted)\n");
        }
        
        current_tls_session = tls_session;
    }

    // Build HTTP request - use HTTP/1.1 with modern browser spoofing to bypass blocks
    http_loading_state.phase = HTTP_PHASE_SENDING_REQUEST;
    strcpy(http_loading_state.status_text, "Sending request...");
    http_process_events();
    
    char* request = (char*)kmalloc(2048);
    if (!request) {
        if (tls_session) { tls_session->socket_fd = -1; tls_destroy_session(tls_session); }
        k_close(sockfd);
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        return -1;
    }

    int len = snprintf(request, 2048,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  // Updated User-Agent to avoid bot detection
                  "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                  "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
                  "Accept-Language: en-US,en;q=0.9\r\n"
                  "Accept-Encoding: gzip, deflate\r\n"
                  "Upgrade-Insecure-Requests: 1\r\n"
                  "Sec-Fetch-Dest: document\r\n"
                  "Sec-Fetch-Mode: navigate\r\n"
                  "Sec-Fetch-Site: none\r\n"
                  "Sec-Fetch-User: ?1\r\n"
                  "Connection: close\r\n", path, host);

    // Add custom headers if specified
    for (int i = 0; i < header_count && headers[i]; i++) {
        len += snprintf(request + len, 2048 - len, "%s\r\n", headers[i]);
    }

    len += snprintf(request + len, 2048 - len, "\r\n");


    // Send request (via TLS if HTTPS)
    int send_result;
    if (is_https && tls_session) {
        send_result = tls_write(tls_session, request, len);
    } else {
        send_result = k_sendto(sockfd, request, len, 0, NULL);
    }
    
    if (send_result < 0) {
        if (tls_session) { tls_session->socket_fd = -1; tls_destroy_session(tls_session); }
        k_close(sockfd);
        kfree(request);
        http_loading_state.is_loading = 0;
        http_loading_state.phase = HTTP_PHASE_ERROR;
        strcpy(http_loading_state.status_text, "Failed to send request");
        kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
        if (buffer) kfree(buffer);
        if (headers_buffer) kfree(headers_buffer);
        return -1;
    }
    kfree(request);

    // Receive response with headers for redirect handling
    int content_length = -1;
    int in_body = 0;
    int status_code = 0;
    int is_chunked = 0;
    // redirect_url is already heap-allocated at function start
    // Clear it for this request iteration
    memset(redirect_url, 0, HTTP_MAX_URL_LEN);
    char* response_ptr = response;
    
    if (!headers_buffer) {
        headers_buffer = (char*)kmalloc(HTTP_MAX_HEADERS_SIZE);
        if (!headers_buffer) {
            if (tls_session) { tls_session->socket_fd = -1; tls_destroy_session(tls_session); }
            if (sockfd >= 0) k_close(sockfd);
            kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
            if (buffer) kfree(buffer);
            return -1;
        }
    }
    int headers_len = 0;

    http_loading_state.phase = HTTP_PHASE_RECEIVING_HEADERS;
    strcpy(http_loading_state.status_text, "Receiving headers...");

    // Massive buffer for faster reads of big pages (like Google/YouTube)
    if (!buffer) {
        buffer = (char*)kmalloc(HTTP_BUFFER_SIZE);
        if (!buffer) {
            kfree(headers_buffer);
            headers_buffer = NULL;
            if (tls_session) { tls_session->socket_fd = -1; tls_destroy_session(tls_session); }
            k_close(sockfd);
            kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
            return -1;
        }
    }

    // Timeout tracking for receive loop
    uint32_t start_time = get_tick_count();

    while (total_received < response_size - 1) {
        // Check for timeout
        if (get_tick_count() - start_time > HTTP_TIMEOUT) {
            strcpy(http_loading_state.status_text, "Request timed out");
            break;
        }
        http_loading_state.bytes_received = total_received;
        if (content_length > 0) {
            http_loading_state.total_bytes = content_length;
            snprintf(http_loading_state.status_text, sizeof(http_loading_state.status_text),
                     "Loading %d/%d bytes", total_received, content_length);
        }
        
        http_process_events();
        
        int received;
        if (is_https && tls_session) {
            received = tls_read(tls_session, buffer, 32767);
        } else {
            received = k_recvfrom(sockfd, buffer, 32767, 0, NULL);
        }

        if (received <= 0) {
            break;
        }

        if (received <= 32767) buffer[received] = 0;

        if (!in_body) {
            if (headers_len < HTTP_MAX_HEADERS_SIZE - 1) {
                int copy_len = received;
                if (headers_len + copy_len >= HTTP_MAX_HEADERS_SIZE) {
                    copy_len = HTTP_MAX_HEADERS_SIZE - headers_len - 1;
                }
                memcpy(headers_buffer + headers_len, buffer, copy_len);
                headers_len += copy_len;
                headers_buffer[headers_len] = 0;
            }
            
            char* body_start = strstr(headers_buffer, "\r\n\r\n");
            if (body_start) {
                in_body = 1;
                http_loading_state.phase = HTTP_PHASE_RECEIVING_BODY;
                
                // Parse status code robustly WITHOUT depending on an external atoi 
                char* status_line = NULL;
                if (strncmp(headers_buffer, "HTTP/", 5) == 0) {
                    status_line = headers_buffer;
                } else {
                    status_line = strstr(headers_buffer, "HTTP/");
                }
                
                if (status_line) {
                    char* space = strchr(status_line, ' ');
                    if (space) {
                        while (*space == ' ') space++;
                        
                        // Inline integer parse
                        int sc = 0;
                        while (*space >= '0' && *space <= '9') {
                            sc = sc * 10 + (*space - '0');
                            space++;
                        }
                        if (sc > 0) status_code = sc;
                    }
                }

                // Parse Location header
                char* location = http_get_header(headers_buffer, "Location:");
                if (location) {
                    char* end = location;
                    while (*end && *end != '\r' && *end != '\n') end++;
                    
                    // Strip trailing spaces explicitly
                    while (end > location && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
                    
                    int loc_len = end - location;
                    if (loc_len > 0) {
                        // Prevent buffer overflows from massive URL redirects without breaking entirely
                        if (loc_len >= HTTP_MAX_URL_LEN) loc_len = HTTP_MAX_URL_LEN - 1;
                        memcpy(redirect_url, location, loc_len);
                        redirect_url[loc_len] = '\0';
                    }
                }

                // Check for Content-Length header using inline parsing
                char* cl_header = http_get_header(headers_buffer, "Content-Length:");
                if (cl_header) {
                    int cl = 0;
                    while (*cl_header >= '0' && *cl_header <= '9') {
                        cl = cl * 10 + (*cl_header - '0');
                        cl_header++;
                    }
                    if (cl > 0) {
                        content_length = cl;
                        http_loading_state.total_bytes = content_length;
                    }
                }
                
                // Check if chunked
                char* te_header = http_get_header(headers_buffer, "Transfer-Encoding:");
                if (te_header) {
                    if (strstr(te_header, "chunked") || strstr(te_header, "Chunked")) {
                        is_chunked = 1;
                    }
                }

                // Copy body remainder already in headers_buffer to response
                char* body = body_start + 4;
                int body_len = (headers_buffer + headers_len) - body;
                if (body_len > 0) {
                    int copy_len = body_len < response_size - total_received - 1 ? 
                                   body_len : response_size - total_received - 1;
                    memcpy(response_ptr, body, copy_len);
                    response_ptr += copy_len;
                    total_received += copy_len;
                }
            }
        } else {
            int copy_len = received < response_size - total_received - 1 ? 
                           received : response_size - total_received - 1;
            memcpy(response_ptr, buffer, copy_len);
            response_ptr += copy_len;
            total_received += copy_len;
        }

        if (progress_cb) {
            progress_cb(total_received, content_length > 0 ? content_length : total_received, user_data);
        }

        if (content_length > 0 && total_received >= content_length) {
            break;
        }
    }

    response[total_received] = 0;
    
    // Decode if chunked
    if (is_chunked) {
        total_received = http_decode_chunked_in_place(response, total_received);
    }
    
    // Decompress gzip if Content-Encoding: gzip and body has gzip magic bytes
    if (total_received >= 2 && headers_buffer) {
        char* ce_header = http_get_header(headers_buffer, "Content-Encoding:");
        int is_gzip_response = 0;
        if (ce_header) {
            // Case-insensitive check for "gzip" value
            while (*ce_header == ' ') ce_header++;
            if ((ce_header[0]=='g'||ce_header[0]=='G') &&
                (ce_header[1]=='z'||ce_header[1]=='Z') &&
                (ce_header[2]=='i'||ce_header[2]=='I') &&
                (ce_header[3]=='p'||ce_header[3]=='P')) {
                is_gzip_response = 1;
            }
        }
        if (is_gzip_response && (uint8_t)response[0] == 0x1F && (uint8_t)response[1] == 0x8B) {
            extern int gzip_inflate(const uint8_t* src, uint32_t src_len,
                                    uint8_t* dst, uint32_t dst_cap,
                                    uint32_t* dst_len);
            // Allocate decompression buffer (up to 4x compressed size, capped at response_size)
            int dec_cap = response_size;
            char* decompressed = (char*)kmalloc(dec_cap);
            if (decompressed) {
                uint32_t decompressed_len = 0;
                int compressed_len = total_received;
                int gz_result = gzip_inflate((const uint8_t*)response, total_received,
                                             (uint8_t*)decompressed, dec_cap - 1,
                                             &decompressed_len);
                if (gz_result == 0 && decompressed_len > 0 && (int)decompressed_len < response_size) {
                    memcpy(response, decompressed, decompressed_len);
                    response[decompressed_len] = 0;
                    total_received = decompressed_len;
                    s_printf("[HTTP] Gzip decompressed: %d -> %u bytes\n", compressed_len, decompressed_len);
                }
                kfree(decompressed);
            }
        }
    }
    
    // Cleanup Resources BEFORE potential recursion
    if (tls_session) {
        // Clear socket_fd so tls_destroy_session doesn't double-close
        // (we close the socket ourselves below)
        tls_session->socket_fd = -1;
        tls_close(tls_session);
        tls_destroy_session(tls_session);
        current_tls_session = NULL;
    }
    k_close(sockfd);

        // Handle redirects (301, 302, 303, 307, 308)
    if ((status_code == 301 || status_code == 302 || status_code == 303 ||
          status_code == 307 || status_code == 308) && redirect_url[0]) {

        // Prevent redirect loops: if we fell back to HTTP, don't redirect back to HTTPS
        if (tried_http_fallback && strncmp(redirect_url, "https://", 8) == 0) {
            // Skip redirect to avoid loop
            http_loading_state.is_loading = 0;
            http_loading_state.phase = HTTP_PHASE_COMPLETE;
            strcpy(http_loading_state.status_text, "Redirect to HTTPS skipped");
            kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
            if (buffer) kfree(buffer);
            if (headers_buffer) kfree(headers_buffer);
            return total_received;
        }

        // Handle relative URLs explicitly
        // CRITICAL: Use heap for absolute_url to prevent stack overflow
        char* absolute_url = (char*)kmalloc(HTTP_MAX_URL_LEN * 2);
        if (!absolute_url) {
            // Can't resolve redirect - just return what we have
            http_loading_state.is_loading = 0;
            http_loading_state.phase = HTTP_PHASE_COMPLETE;
            kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
            if (buffer) kfree(buffer);
            if (headers_buffer) kfree(headers_buffer);
            return total_received;
        }
        
        // Protocol-relative URL (e.g. //www.google.com/)
        if (redirect_url[0] == '/' && redirect_url[1] == '/') {
            snprintf(absolute_url, HTTP_MAX_URL_LEN * 2, "%s:%s", 
                    is_https ? "https" : "http", redirect_url);
            strncpy(redirect_url, absolute_url, HTTP_MAX_URL_LEN - 1);
            redirect_url[HTTP_MAX_URL_LEN - 1] = '\0';
        } 
        // Root-relative URL (e.g. /search?q=test)
        else if (redirect_url[0] == '/') {
            snprintf(absolute_url, HTTP_MAX_URL_LEN * 2, "%s://%s%s", 
                    is_https ? "https" : "http", host, redirect_url);
            strncpy(redirect_url, absolute_url, HTTP_MAX_URL_LEN - 1);
            redirect_url[HTTP_MAX_URL_LEN - 1] = '\0';
        } 
        // Relative URL missing root path (e.g. "search?q=test")
        else if (strstr(redirect_url, "://") == NULL) {
            snprintf(absolute_url, HTTP_MAX_URL_LEN * 2, "%s://%s/%s",
                    is_https ? "https" : "http", host, redirect_url);
            strncpy(redirect_url, absolute_url, HTTP_MAX_URL_LEN - 1);
            redirect_url[HTTP_MAX_URL_LEN - 1] = '\0';
        }
        kfree(absolute_url);

        
        kfree(headers_buffer);
        headers_buffer = NULL;  // Prevent double-free on next iteration
        // Note: buffer is kept alive for the next iteration since it's reused
        // Follow redirect iteratively
        s_printf("[REDIR] processing\n");
        strncpy(current_url, redirect_url, HTTP_MAX_URL_LEN * 2 - 1);
        current_url[HTTP_MAX_URL_LEN * 2 - 1] = '\0';
        cur_redirect++;
        continue;
    }
    } // end while

    // Mark complete and free heap-allocated URL buffers
    http_loading_state.is_loading = 0;
    http_loading_state.phase = HTTP_PHASE_COMPLETE;
    strcpy(http_loading_state.status_text, "Done");

    kfree(current_url); kfree(host); kfree(path); kfree(redirect_url);
    if (buffer) kfree(buffer);
    if (headers_buffer) kfree(headers_buffer);
    return total_received;
    }
    return -1; /* fallback */
}

// Simple HTTP GET with default headers
int http_get_simple(const char* url, char* response, int response_size) {
    return http_get(url, response, response_size, NULL, 0);
}

// Async HTTP GET with progress callback
int http_get_async(const char* url, char* response, int response_size,
                   const char** headers, int header_count,
                   http_progress_cb progress_cb, void* user_data) {
    int result = http_get(url, response, response_size, headers, header_count);
    
    if (progress_cb && result > 0) {
        progress_cb(result, result, user_data);
    }
    
    return result;
}
