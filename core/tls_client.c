// core/tls_client.c - TLS Client Wrapper for CamelOS
// Provides the tls_client_* API used by the browser and download manager.
// Bridges the browser's void* conn (TCP connection) model to the
// tls_session_t based TLS implementation.
//
// NOTE: Full TLS is not yet supported over the RTL8139 TCP stack.
// The handshake function safely returns an error so the browser can
// fall back to HTTP. When a proper socket layer is available, the
// tls_connect() path can be re-enabled.

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

    s_printf("[TLS_CLIENT] TLS handshake requested but not yet supported over RTL8139 TCP\n");
    s_printf("[TLS_CLIENT] Browser should fall back to HTTP\n");

    // Full TLS is not yet integrated with the RTL8139 polling-based TCP stack.
    // The tls_connect() function uses BSD socket recv()/send() which don't
    // work with our tcp_connection_t model. Return error to let the browser
    // gracefully fall back to HTTP.
    return -10;  // TLS_NOT_SUPPORTED
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
