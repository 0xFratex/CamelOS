// core/tls_client.c - TLS Client Wrapper for CamelOS
// Provides the tls_client_* API used by the browser and download manager.
// Now bridges to the BSD socket-based TLS implementation in tls.c.
//
// How it works:
//   1. Browser calls tls_client_handshake(conn) where conn is a void*
//      returned by tcp_connect_with_ptr().
//   2. We create a BSD socket via k_socket() and connect it through
//      the socket layer, which internally wraps the same TCP stack.
//   3. We then invoke tls_connect() from tls.c with the socket fd,
//      which performs the full TLS 1.2 handshake using k_sendto/k_recvfrom.
//   4. After a successful handshake, tls_read()/tls_write() give us
//      encrypted streams over the socket.
//   5. The browser uses tls_client_send/recv which forward to the
//      session's TLS-encrypted read/write.

#include "tls.h"
#include "tls13.h"
#include "tcp.h"
#include "socket.h"
#include "dns.h"
#include "string.h"
#include "memory.h"
#include "../common/serial.h"
#include "../hal/cpu/timer.h"

// Maximum concurrent TLS sessions (browser + downloads)
#define MAX_TLS_CLIENT_SESSIONS 4

// Stored TLS session context associated with a TCP connection
typedef struct {
    int active;
    void* tcp_conn;          // The underlying raw TCP connection (for fallback)
    int socket_fd;           // The BSD socket fd (for TLS)
    tls_session_t* session;  // The TLS session state
    char hostname[256];      // Server hostname for SNI
    uint32_t remote_ip;      // Server IP for socket connection
    uint16_t remote_port;    // Server port for socket connection
} tls_client_ctx_t;

static tls_client_ctx_t g_tls_client_ctx[MAX_TLS_CLIENT_SESSIONS];

// Find or allocate a TLS client context for a given TCP connection
static tls_client_ctx_t* find_ctx(void* conn) {
    // First try to find existing
    for (int i = 0; i < MAX_TLS_CLIENT_SESSIONS; i++) {
        if (g_tls_client_ctx[i].active && g_tls_client_ctx[i].tcp_conn == conn) {
            return &g_tls_client_ctx[i];
        }
    }
    // Allocate a free slot
    for (int i = 0; i < MAX_TLS_CLIENT_SESSIONS; i++) {
        if (!g_tls_client_ctx[i].active) {
            memset(&g_tls_client_ctx[i], 0, sizeof(tls_client_ctx_t));
            g_tls_client_ctx[i].active = 1;
            g_tls_client_ctx[i].tcp_conn = conn;
            g_tls_client_ctx[i].socket_fd = -1;
            return &g_tls_client_ctx[i];
        }
    }
    return 0;
}

// Find existing context without allocating
static tls_client_ctx_t* find_existing_ctx(void* conn) {
    for (int i = 0; i < MAX_TLS_CLIENT_SESSIONS; i++) {
        if (g_tls_client_ctx[i].active && g_tls_client_ctx[i].tcp_conn == conn) {
            return &g_tls_client_ctx[i];
        }
    }
    return 0;
}

// Find context by socket fd
static tls_client_ctx_t* find_ctx_by_socket(int fd) {
    for (int i = 0; i < MAX_TLS_CLIENT_SESSIONS; i++) {
        if (g_tls_client_ctx[i].active && g_tls_client_ctx[i].socket_fd == fd) {
            return &g_tls_client_ctx[i];
        }
    }
    return 0;
}

// Extract remote IP and port from a tcp_connection_t
// This uses the internal TCP connection structure to get the peer address
static void get_tcp_conn_info(void* conn, uint32_t* ip, uint16_t* port) {
    if (!conn || !ip || !port) return;
    
    // Access the TCP connection structure fields
    // tcp_connection_t has remote_ip and remote_port fields
    tcp_connection_t* tcp_conn = (tcp_connection_t*)conn;
    *ip = tcp_conn->remote_ip;
    *port = tcp_conn->remote_port;
}

// Perform TLS handshake over an existing TCP connection
// conn is a tcp_connection_t* from tcp_connect_with_ptr()
// Returns 0 on success, negative on error
int tls_client_handshake(void* conn) {
    if (!conn) return -1;

    // Step 1: Extract the remote IP and port from the raw TCP connection
    uint32_t remote_ip = 0;
    uint16_t remote_port = 0;
    get_tcp_conn_info(conn, &remote_ip, &remote_port);
    
    if (remote_ip == 0 || remote_port == 0) {
        s_printf("[TLS_CLIENT] Cannot extract remote address from TCP connection\n");
        return -10;
    }

    // Step 2: Find or allocate a TLS client context
    tls_client_ctx_t* ctx = find_ctx(conn);
    if (!ctx) {
        s_printf("[TLS_CLIENT] No free TLS session slots\n");
        return -10;
    }
    
    ctx->remote_ip = remote_ip;
    ctx->remote_port = remote_port;

    // Step 3: Close the raw TCP connection - we'll use a BSD socket instead
    // The BSD socket layer wraps the same TCP stack but provides the
    // k_sendto/k_recvfrom API that tls_connect() expects.
    // We don't close the raw conn yet - keep it as a reference.
    
    s_printf("[TLS_CLIENT] Creating BSD socket for TLS...\n");

    // Step 4: Create a BSD socket
    int sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        s_printf("[TLS_CLIENT] Failed to create BSD socket\n");
        ctx->active = 0;
        return -10;
    }

    // Step 5: Connect via BSD socket to the same remote endpoint
    sockaddr_in_t server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(remote_port);
    server_addr.sin_addr = remote_ip;
    
    if (k_connect(sockfd, &server_addr) < 0) {
        s_printf("[TLS_CLIENT] BSD socket connect failed\n");
        k_close(sockfd);
        ctx->active = 0;
        return -10;
    }

    s_printf("[TLS_CLIENT] BSD socket connected, starting TLS handshake...\n");

    // Step 6: Create a TLS session and perform the handshake
    tls_session_t* session = tls_create_session();
    if (!session) {
        s_printf("[TLS_CLIENT] Failed to create TLS session\n");
        k_close(sockfd);
        ctx->active = 0;
        return -10;
    }
    
    session->socket_fd = sockfd;
    
    // Set hostname for SNI if available
    if (ctx->hostname[0]) {
        tls_set_hostname(session, ctx->hostname);
    }
    
    // Skip strict cert verification for broader compatibility
    // (our CA store is limited)
    tls_set_verify(session, 0);

    // Step 7: Perform the TLS handshake
    int result = tls_connect(session, ctx->hostname[0] ? ctx->hostname : "", remote_port);
    if (result != 0) {
        s_printf("[TLS_CLIENT] TLS handshake failed: ");
        char err_buf[16];
        int_to_str(result, err_buf);
        s_printf(err_buf);
        s_printf(" (");
        s_printf(tls_error_string((tls_error_t)result));
        s_printf(")\n");
        
        tls_destroy_session(session);
        k_close(sockfd);
        ctx->active = 0;
        return result;
    }

    s_printf("[TLS_CLIENT] TLS handshake succeeded!\n");

    // Step 8: Store the successful session
    ctx->socket_fd = sockfd;
    ctx->session = session;

    return 0;  // Success!
}

// Alternative: Perform TLS handshake using an existing BSD socket fd
// This is used by http.c which already creates a socket via k_socket()
// Returns the TLS session on success, NULL on failure
tls_session_t* tls_client_handshake_fd(int sockfd, const char* hostname, uint16_t port) {
    if (sockfd < 0) return NULL;

    s_printf("[TLS_CLIENT] Starting TLS handshake on existing socket fd=");
    char fd_buf[16];
    int_to_str(sockfd, fd_buf);
    s_printf(fd_buf);
    s_printf("\n");

    // Create a TLS session
    tls_session_t* session = tls_create_session();
    if (!session) {
        s_printf("[TLS_CLIENT] Failed to create TLS session\n");
        return NULL;
    }
    
    session->socket_fd = sockfd;
    
    // Set hostname for SNI
    if (hostname) {
        tls_set_hostname(session, hostname);
    }
    
    // Skip strict cert verification
    tls_set_verify(session, 0);

    // Perform the TLS handshake
    int result = tls_connect(session, hostname ? hostname : "", port);
    if (result != 0) {
        s_printf("[TLS_CLIENT] TLS handshake failed: ");
        char err_buf[16];
        int_to_str(result, err_buf);
        s_printf(err_buf);
        s_printf("\n");
        
        // Clear socket_fd before destroying session so tls_destroy_session
        // doesn't close the caller's socket (the caller manages it)
        session->socket_fd = -1;
        tls_destroy_session(session);
        return NULL;
    }

    s_printf("[TLS_CLIENT] TLS handshake succeeded!\n");

    // Also track in our context table
    tls_client_ctx_t* ctx = NULL;
    for (int i = 0; i < MAX_TLS_CLIENT_SESSIONS; i++) {
        if (!g_tls_client_ctx[i].active) {
            ctx = &g_tls_client_ctx[i];
            break;
        }
    }
    if (ctx) {
        memset(ctx, 0, sizeof(tls_client_ctx_t));
        ctx->active = 1;
        ctx->tcp_conn = NULL;  // No raw TCP connection
        ctx->socket_fd = sockfd;
        ctx->session = session;
        if (hostname) {
            strncpy(ctx->hostname, hostname, 255);
            ctx->hostname[255] = 0;
        }
        ctx->remote_port = port;
    }

    return session;
}

// Send data over an established TLS connection
// Returns number of bytes sent, or negative on error
int tls_client_send(void* conn, const char* data, int len) {
    if (!conn || !data || len <= 0) return -1;

    tls_client_ctx_t* ctx = find_existing_ctx(conn);
    if (!ctx || !ctx->session) {
        // No TLS session - fall through to raw TCP
        return tcp_conn_send(conn, data, len);
    }

    int result = tls_write(ctx->session, data, (size_t)len);
    if (result < 0) {
        s_printf("[TLS_CLIENT] send failed: ");
        char buf[16];
        int_to_str(result, buf);
        s_printf(buf);
        s_printf("\n");
    }
    return result;
}

// Send data over an established TLS session (by session pointer)
int tls_client_session_send(tls_session_t* session, const char* data, int len) {
    if (!session || !data || len <= 0) return -1;
    return tls_write(session, data, (size_t)len);
}

// Receive data from an established TLS connection
// Returns number of bytes received, 0 if closed, negative on error
int tls_client_recv(void* conn, char* buf, int len) {
    if (!conn || !buf || len <= 0) return -1;

    tls_client_ctx_t* ctx = find_existing_ctx(conn);
    if (!ctx || !ctx->session) {
        // No TLS session means this isn't a TLS connection
        // Fall through to raw TCP receive
        return tcp_conn_recv(conn, buf, len);
    }

    int result = tls_read(ctx->session, buf, (size_t)len);
    if (result < 0) {
        // TLS read failed - try raw TCP as fallback
        return tcp_conn_recv(conn, buf, len);
    }
    return result;
}

// Receive data from an established TLS session (by session pointer)
int tls_client_session_recv(tls_session_t* session, char* buf, int len) {
    if (!session || !buf || len <= 0) return -1;
    return tls_read(session, buf, (size_t)len);
}

// Set the hostname for SNI (Server Name Indication) - call before handshake
void tls_client_set_hostname(void* conn, const char* hostname) {
    if (!conn || !hostname) return;

    tls_client_ctx_t* ctx = find_ctx(conn);
    if (ctx) {
        strncpy(ctx->hostname, hostname, 255);
        ctx->hostname[255] = 0;
    }
}

// Close and clean up a TLS client session
void tls_client_close(void* conn) {
    if (!conn) return;

    tls_client_ctx_t* ctx = find_existing_ctx(conn);
    if (ctx) {
        if (ctx->session) {
            tls_close(ctx->session);
            tls_destroy_session(ctx->session);
            ctx->session = 0;
        }
        if (ctx->socket_fd >= 0) {
            k_close(ctx->socket_fd);
            ctx->socket_fd = -1;
        }
        ctx->active = 0;
    }
}

// Close and clean up a TLS client session by session pointer
// Note: This does NOT close the underlying socket fd, as the caller
// (browser/http.c) manages the socket lifecycle
void tls_client_session_close(tls_session_t* session) {
    if (!session) return;
    
    // Find and clear the context
    for (int i = 0; i < MAX_TLS_CLIENT_SESSIONS; i++) {
        if (g_tls_client_ctx[i].active && g_tls_client_ctx[i].session == session) {
            g_tls_client_ctx[i].session = 0;
            g_tls_client_ctx[i].socket_fd = -1;
            g_tls_client_ctx[i].active = 0;
            break;
        }
    }
    
    // Clear socket_fd before destroying so tls_destroy_session doesn't close it
    // The caller (browser/http client) is responsible for closing the socket
    session->socket_fd = -1;
    tls_destroy_session(session);
}

// Check if a connection has an active TLS session
int tls_client_is_active(void* conn) {
    tls_client_ctx_t* ctx = find_existing_ctx(conn);
    return (ctx && ctx->session && ctx->session->state == TLS_STATE_ESTABLISHED) ? 1 : 0;
}
