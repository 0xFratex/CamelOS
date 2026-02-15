// core/http.c - Optimized HTTP client implementation with async support and TLS
#include "http.h"
#include "socket.h"
#include "string.h"
#include "memory.h"
#include "dns.h"
#include "net.h"
#include "tls.h"
#include "window_server.h"
#include "../hal/video/gfx_hal.h"
#include "../usr/framework.h"

#define HTTP_BUFFER_SIZE 8192
#define HTTP_MAX_REDIRECTS 5
#define HTTP_TIMEOUT 5000 // 5 seconds (reduced from 10)

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production
// ============================================================================
#define HTTP_DEBUG_ENABLED     0

// External references for event processing
extern void rtl8139_poll(void);
extern window_t* active_win;  // From window_server.c
extern int atoi(const char* str);

// TLS session for HTTPS connections
static tls_session_t* current_tls_session = NULL;

// Forward declaration for internal request function
static int http_get_internal(const char* url, char* response, int response_size,
                             const char** headers, int header_count, int redirect_count);

// Full event processing during HTTP requests - redraws window and swaps buffers
static void http_process_events(void) {
    rtl8139_poll();  // Poll network card
    
    // Redraw the active window completely
    if (active_win && active_win->paint_callback) {
        // Draw window frame first
        extern void compositor_draw_window(window_t* w);
        compositor_draw_window(active_win);
        
        // Then draw content
        typedef void (*pcb)(int,int,int,int);
        ((pcb)active_win->paint_callback)(active_win->x, active_win->y + 30, active_win->width, active_win->height - 30);
        
        // Swap buffers to show the update
        gfx_swap_buffers();
    }
    
    for(volatile int i = 0; i < 1000; i++) asm volatile("pause");
}

// Parse URL - returns 1 for HTTPS, 0 for HTTP
static int http_parse_url(const char* url, char* host, char* path, uint16_t* port) {
    const char* proto = strstr(url, "://");
    const char* start;
    int is_https = 0;

    if (proto) {
        // Check if HTTPS
        if (proto - url >= 5 && strncmp(url, "https", 5) == 0) {
            is_https = 1;
        }
        start = proto + 3;
    } else {
        start = url;
    }

    // Find host end
    const char* path_start = strchr(start, '/');
    const char* port_start = strchr(start, ':');

    if (path_start) {
        strncpy(path, path_start, 256);
        path[255] = 0;
    } else {
        strcpy(path, "/");
    }

    // Extract host and port
    int host_len;
    if (port_start && (!path_start || port_start < path_start)) {
        host_len = port_start - start;
        *port = atoi(port_start + 1);
    } else if (path_start) {
        host_len = path_start - start;
        // Default port based on protocol
        *port = is_https ? 443 : 80;
    } else {
        host_len = strlen(start);
        // Default port based on protocol
        *port = is_https ? 443 : 80;
    }

    strncpy(host, start, host_len);
    host[host_len] = 0;

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
    return http_get_internal(url, response, response_size, headers, header_count, 0);
}

// Internal HTTP GET with redirect tracking
static int http_get_internal(const char* url, char* response, int response_size,
                             const char** headers, int header_count, int redirect_count) {
    char host[256];
    char path[256];
    uint16_t port;
    int is_https;

    // Check redirect limit
    if (redirect_count > HTTP_MAX_REDIRECTS) {
        return -1;
    }

    is_https = http_parse_url(url, host, path, &port);
    if (is_https < 0) {
        return -1;
    }

    // Resolve hostname
    char ip_str[32];
    if (dns_resolve(host, ip_str, sizeof(ip_str)) < 0) {
        return -1;
    }

    // Create socket
    int sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    // Connect
    sockaddr_in_t server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = http_inet_addr(ip_str);

    if (k_connect(sockfd, &server_addr) < 0) {
        k_close(sockfd);
        return -1;
    }

    // TLS handshake for HTTPS
    tls_session_t* tls_session = NULL;
    if (is_https) {
        tls_session = tls_create_session();
        if (!tls_session) {
            k_close(sockfd);
            return -1;
        }
        
        tls_session->socket_fd = sockfd;
        tls_set_hostname(tls_session, host);
        tls_set_verify(tls_session, 0);  // Skip cert verification for now
        
        // Perform TLS handshake
        int tls_result = tls_connect(tls_session, host, port);
        if (tls_result != 0) {
            // TLS failed - try fallback to HTTP on port 80
            tls_destroy_session(tls_session);
            k_close(sockfd);
            
            // Fallback: try HTTP on port 80
            sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                return -1;
            }
            
            server_addr.sin_port = htons(80);
            if (k_connect(sockfd, &server_addr) < 0) {
                k_close(sockfd);
                return -1;
            }
            
            is_https = 0;  // Continue with HTTP
            tls_session = NULL;
        }
        
        current_tls_session = tls_session;
    }

    // Build HTTP request - use HTTP/1.1 for better compatibility
    char request[1024];
    int len = snprintf(request, sizeof(request),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: Mozilla/5.0 (compatible; CamelOS/1.0)\r\n"
                      "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                      "Accept-Language: en-US,en;q=0.5\r\n"
                      "Accept-Encoding: identity\r\n"
                      "Connection: close\r\n", path, host);

    // Add custom headers
    for (int i = 0; i < header_count && headers[i]; i++) {
        len += snprintf(request + len, sizeof(request) - len, "%s\r\n", headers[i]);
    }

    len += snprintf(request + len, sizeof(request) - len, "\r\n");

    // Send request (via TLS if HTTPS)
    int send_result;
    if (is_https && tls_session) {
        send_result = tls_write(tls_session, request, len);
    } else {
        send_result = k_sendto(sockfd, request, len, 0, NULL);
    }
    
    if (send_result < 0) {
        if (tls_session) tls_destroy_session(tls_session);
        k_close(sockfd);
        return -1;
    }

    // Receive response with headers for redirect handling
    int total_received = 0;
    int content_length = -1;
    int in_body = 0;
    int status_code = 0;
    char redirect_url[512] = {0};
    char* response_ptr = response;
    char headers_buffer[4096];
    int headers_len = 0;

    // Use larger buffer for faster reads
    char buffer[2048];
    
    while (total_received < response_size - 1) {
        // Process events to prevent UI freeze
        http_process_events();
        
        int received;
        if (is_https && tls_session) {
            received = tls_read(tls_session, buffer, sizeof(buffer) - 1);
        } else {
            received = k_recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, NULL);
        }

        if (received <= 0) {
            break;
        }

        buffer[received] = 0;

        if (!in_body) {
            // Store headers for redirect parsing
            if (headers_len < (int)sizeof(headers_buffer) - 1) {
                int copy_len = received;
                if (headers_len + copy_len >= (int)sizeof(headers_buffer)) {
                    copy_len = sizeof(headers_buffer) - headers_len - 1;
                }
                memcpy(headers_buffer + headers_len, buffer, copy_len);
                headers_len += copy_len;
                headers_buffer[headers_len] = 0;
            }
            
            // Still in headers - find body start
            char* body_start = strstr(buffer, "\r\n\r\n");
            if (body_start) {
                in_body = 1;
                char* body = body_start + 4;
                int body_len = received - (body - buffer);

                // Parse status code
                if (strncmp(buffer, "HTTP/1.", 7) == 0) {
                    status_code = atoi(buffer + 9);
                }

                // Check for Location header (redirect)
                char* location = strstr(headers_buffer, "Location:");
                if (!location) location = strstr(headers_buffer, "location:");
                if (location) {
                    location += 9;
                    while (*location == ' ') location++;
                    char* end = strstr(location, "\r\n");
                    if (end) {
                        int loc_len = end - location;
                        if (loc_len < (int)sizeof(redirect_url)) {
                            memcpy(redirect_url, location, loc_len);
                            redirect_url[loc_len] = 0;
                        }
                    }
                }

                // Check for Content-Length header (case-insensitive search)
                char* cl_header = strstr(buffer, "Content-Length:");
                if (!cl_header) cl_header = strstr(buffer, "content-length:");
                if (cl_header) {
                    content_length = atoi(cl_header + 15);
                }

                // Copy body to response
                if (body_len > 0) {
                    int copy_len = body_len < response_size - total_received - 1 ? 
                                   body_len : response_size - total_received - 1;
                    memcpy(response_ptr, body, copy_len);
                    response_ptr += copy_len;
                    total_received += copy_len;
                }
            }
        } else {
            // Already in body - copy directly
            int copy_len = received < response_size - total_received - 1 ? 
                           received : response_size - total_received - 1;
            memcpy(response_ptr, buffer, copy_len);
            response_ptr += copy_len;
            total_received += copy_len;
        }

        // Stop if we have all content
        if (content_length > 0 && total_received >= content_length) {
            break;
        }
    }

    response[total_received] = 0;
    
    // Cleanup TLS session
    if (tls_session) {
        tls_close(tls_session);
        tls_destroy_session(tls_session);
        current_tls_session = NULL;
    }
    
    k_close(sockfd);

    // Handle redirects (301, 302, 303, 307, 308)
    if ((status_code == 301 || status_code == 302 || status_code == 303 || 
         status_code == 307 || status_code == 308) && redirect_url[0]) {
        // Follow redirect
        return http_get_internal(redirect_url, response, response_size, headers, header_count, redirect_count + 1);
    }

    return total_received;
}

// Simple HTTP GET with default headers
int http_get_simple(const char* url, char* response, int response_size) {
    const char* headers[] = {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.5",
        "Accept-Encoding: identity",
        NULL
    };

    return http_get(url, response, response_size, headers, 3);
}

// Async HTTP GET with progress callback - allows UI updates during fetch, supports HTTPS
int http_get_async(const char* url, char* response, int response_size,
                   const char** headers, int header_count,
                   http_progress_cb progress_cb, void* user_data) {
    // For simplicity, use the sync version with progress callback
    // In a real implementation, this would be truly async
    int result = http_get(url, response, response_size, headers, header_count);
    
    if (progress_cb && result > 0) {
        progress_cb(result, result, user_data);
    }
    
    return result;
}
