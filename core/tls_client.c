// core/tls_client.c - TLS Client Wrapper for CamelOS
// Provides the tls_client_* API used by the browser and download manager.
// Bridges the browser's void* conn (TCP connection) model to the
// tls_session_t based TLS implementation.

#include "tls.h"
#include "tls13.h"
#include "tcp.h"
#include "string.h"
#include "memory.h"
#include "../common/serial.h"

// Maximum concurrent TLS sessions (browser + downloads)
#define MAX_TLS_CLIENT_SESSIONS 4

// Stored TLS session context associated with a TCP connection
typedef struct {
    int active;
    void* tcp_conn;          // The underlying TCP connection
    tls_session_t* session;  // The TLS session state
    char hostname[256];      // Server hostname for SNI
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

// Perform TLS handshake over an existing TCP connection
// conn is a tcp_connection_t* from tcp_connect_with_ptr()
// Returns 0 on success, negative on error
int tls_client_handshake(void* conn) {
    if (!conn) return -1;

    tls_client_ctx_t* ctx = find_ctx(conn);
    if (!ctx) {
        s_printf("[TLS_CLIENT] No free session slots\n");
        return -2;
    }

    // Create a new TLS session
    tls_session_t* session = tls_create_session();
    if (!session) {
        s_printf("[TLS_CLIENT] Failed to create TLS session\n");
        ctx->active = 0;
        return -3;
    }

    // Disable strict certificate verification for now (self-signed certs)
    tls_set_verify(session, 0);

    ctx->session = session;

    // Extract the remote IP and port from the TCP connection
    // For now, use a placeholder hostname - the browser sets it before calling
    if (ctx->hostname[0]) {
        tls_set_hostname(session, ctx->hostname);
    }

    // Perform the TLS handshake using the TCP connection's underlying socket
    // The TLS layer needs a connected socket; we use the TCP connection's fd
    tcp_connection_t* tcp = (tcp_connection_t*)conn;
    session->socket_fd = tcp->local_port; // Use as identifier

    // Perform the full TLS connect/handshake
    // tls_connect() handles the full handshake sequence
    int result = tls_connect(session, ctx->hostname[0] ? ctx->hostname : "server",
                             tcp->remote_port);

    if (result != 0) {
        s_printf("[TLS_CLIENT] Handshake failed: ");
        char buf[16];
        int_to_str(result, buf);
        s_printf(buf);
        s_printf("\n");
        tls_destroy_session(session);
        ctx->session = 0;
        ctx->active = 0;
        return result;
    }

    s_printf("[TLS_CLIENT] TLS handshake successful\n");
    return 0;
}

// Send data over an established TLS connection
// Returns number of bytes sent, or negative on error
int tls_client_send(void* conn, const char* data, int len) {
    if (!conn || !data || len <= 0) return -1;

    tls_client_ctx_t* ctx = find_existing_ctx(conn);
    if (!ctx || !ctx->session) {
        s_printf("[TLS_CLIENT] send: no TLS session for connection\n");
        return -2;
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
        // This handles cases where the session state is inconsistent
        return tcp_conn_recv(conn, buf, len);
    }
    return result;
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
        ctx->active = 0;
    }
}
