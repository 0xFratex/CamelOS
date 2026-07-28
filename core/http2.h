// core/http2.h - HTTP/2 Protocol Implementation
// Based on RFC 7540 (HTTP/2) and RFC 7541 (HPACK)
// Implements: Binary framing, HPACK compression, Stream multiplexing

#ifndef HTTP2_H
#define HTTP2_H

#include "../include/types.h"

// ============================================================================
// HTTP/2 CONSTANTS
// ============================================================================

// HTTP/2 Connection Preface
static const char HTTP2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
static const int HTTP2_PREFACE_LEN = 24;

// HTTP/2 Frame Types
#define HTTP2_FRAME_DATA          0x00
#define HTTP2_FRAME_HEADERS       0x01
#define HTTP2_FRAME_PRIORITY      0x02
#define HTTP2_FRAME_RST_STREAM    0x03
#define HTTP2_FRAME_SETTINGS      0x04
#define HTTP2_FRAME_PUSH_PROMISE  0x05
#define HTTP2_FRAME_PING          0x06
#define HTTP2_FRAME_GOAWAY        0x07
#define HTTP2_FRAME_WINDOW_UPDATE 0x08
#define HTTP2_FRAME_CONTINUATION  0x09

// HTTP/2 Settings Identifiers
#define HTTP2_SETTINGS_HEADER_TABLE_SIZE      0x01
#define HTTP2_SETTINGS_ENABLE_PUSH            0x02
#define HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS 0x03
#define HTTP2_SETTINGS_INITIAL_WINDOW_SIZE    0x04
#define HTTP2_SETTINGS_MAX_FRAME_SIZE         0x05
#define HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE   0x06

// HTTP/2 Settings Default Values
#define HTTP2_DEFAULT_HEADER_TABLE_SIZE   4096
#define HTTP2_DEFAULT_ENABLE_PUSH         1
#define HTTP2_DEFAULT_MAX_CONCURRENT_STREAMS 100
#define HTTP2_DEFAULT_INITIAL_WINDOW_SIZE 65535
#define HTTP2_DEFAULT_MAX_FRAME_SIZE      16384
#define HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE 65536

// HTTP/2 Frame Flags
#define HTTP2_FLAG_NONE         0x00
#define HTTP2_FLAG_END_STREAM   0x01
#define HTTP2_FLAG_END_HEADERS  0x04
#define HTTP2_FLAG_PADDED       0x08
#define HTTP2_FLAG_PRIORITY     0x20
#define HTTP2_FLAG_ACK          0x01

// HTTP/2 Stream States
typedef enum {
    HTTP2_STREAM_IDLE,
    HTTP2_STREAM_RESERVED_LOCAL,
    HTTP2_STREAM_RESERVED_REMOTE,
    HTTP2_STREAM_OPEN,
    HTTP2_STREAM_HALF_CLOSED_LOCAL,
    HTTP2_STREAM_HALF_CLOSED_REMOTE,
    HTTP2_STREAM_CLOSED
} http2_stream_state_t;

// HTTP/2 Error Codes
#define HTTP2_ERROR_NO_ERROR            0x00
#define HTTP2_ERROR_PROTOCOL_ERROR      0x01
#define HTTP2_ERROR_INTERNAL_ERROR      0x02
#define HTTP2_ERROR_FLOW_CONTROL_ERROR  0x03
#define HTTP2_ERROR_SETTINGS_TIMEOUT    0x04
#define HTTP2_ERROR_STREAM_CLOSED       0x05
#define HTTP2_ERROR_FRAME_SIZE_ERROR    0x06
#define HTTP2_ERROR_REFUSED_STREAM      0x07
#define HTTP2_ERROR_CANCEL              0x08
#define HTTP2_ERROR_COMPRESSION_ERROR   0x09
#define HTTP2_ERROR_CONNECT_ERROR       0x0a
#define HTTP2_ERROR_ENHANCE_YOUR_CALM   0x0b
#define HTTP2_ERROR_INADEQUATE_SECURITY 0x0c
#define HTTP2_ERROR_HTTP_1_1_REQUIRED   0x0d

// HTTP/2 Limits — REDUCED to keep http2_connection_t allocatable
// Original values (256 streams × 64 headers × 8KB each = 134 MB) made
// kmalloc(sizeof(http2_connection_t)) fail immediately.
#define HTTP2_MAX_FRAME_SIZE      16777215  // 2^24 - 1 (protocol limit, unchanged)
#define HTTP2_MAX_STREAMS         8         // was 256 — we only need 1-2 at a time
#define HTTP2_MAX_HEADERS         16        // was 64  — typical response has ~10
#define HTTP2_HEADER_MAX_LEN      256       // was 4096 — header names/values rarely exceed this
#define HTTP2_MAX_WINDOW          2147483647  // 2^31 - 1

// Read cache size: one TLS record is at most ~16KB + overhead.
// 32KB gives headroom for reassembly.
#define HTTP2_READ_CACHE_SIZE     32768

// ============================================================================
// HPACK STATIC TABLE (RFC 7541 Appendix A)
// ============================================================================

typedef struct {
    const char* name;
    const char* value;
} hpack_static_entry_t;

// Static table entries (first 61 entries)
static const hpack_static_entry_t hpack_static_table[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

#define HPACK_STATIC_TABLE_SIZE 61

// ============================================================================
// HPACK DYNAMIC TABLE
// ============================================================================

typedef struct hpack_dynamic_entry {
    char* name;
    char* value;
    size_t size;  // name.len + value.len + 32
    struct hpack_dynamic_entry* next;
    struct hpack_dynamic_entry* prev;
} hpack_dynamic_entry_t;

typedef struct {
    hpack_dynamic_entry_t* head;  // Most recent entry
    hpack_dynamic_entry_t* tail;  // Oldest entry
    size_t max_size;
    size_t current_size;
} hpack_dynamic_table_t;

// ============================================================================
// HPACK HEADER
// ============================================================================

typedef struct {
    char name[HTTP2_HEADER_MAX_LEN];
    char value[HTTP2_HEADER_MAX_LEN];
    int is_indexed;      // From static or dynamic table
    int is_pseudo;       // Pseudo-header (starts with :)
} http2_header_t;

// ============================================================================
// HTTP/2 FRAME STRUCTURES
// ============================================================================

// HTTP/2 Frame Header (9 bytes)
typedef struct __attribute__((packed)) {
    uint32_t length : 24;    // Length of frame payload
    uint8_t type;            // Frame type
    uint8_t flags;           // Frame flags
    uint32_t stream_id : 31; // Stream identifier
    uint8_t reserved : 1;    // Reserved bit
} http2_frame_header_t;

// HTTP/2 Settings Frame
typedef struct __attribute__((packed)) {
    uint16_t identifier;
    uint32_t value;
} http2_setting_t;

// HTTP/2 Stream
typedef struct {
    uint32_t id;
    http2_stream_state_t state;
    uint32_t send_window;
    uint32_t recv_window;
    http2_header_t headers[HTTP2_MAX_HEADERS];
    int header_count;
    uint8_t* data;
    size_t data_len;
    size_t data_capacity;
    int end_stream_received;
    int end_stream_sent;
} http2_stream_t;

// ============================================================================
// HTTP/2 CONNECTION
// ============================================================================

typedef struct {
    // Socket
    int socket_fd;
    
    // TLS session (if using HTTPS)
    void* tls_session;
    int use_tls;
    
    // Server info
    char host[256];
    uint16_t port;
    
    // Connection state
    int established;
    int goaway_received;
    int goaway_sent;
    uint32_t last_stream_id;
    
    // Streams
    http2_stream_t streams[HTTP2_MAX_STREAMS];
    int stream_count;
    uint32_t next_stream_id;  // Next client-initiated stream ID
    
    // Settings
    uint32_t settings_header_table_size;
    uint32_t settings_enable_push;
    uint32_t settings_max_concurrent_streams;
    uint32_t settings_initial_window_size;
    uint32_t settings_max_frame_size;
    uint32_t settings_max_header_list_size;
    
    // Peer settings
    uint32_t peer_header_table_size;
    uint32_t peer_max_frame_size;
    uint32_t peer_initial_window_size;
    
    // Connection-level flow control
    uint32_t send_window;
    uint32_t recv_window;
    
    // HPACK encoder/decoder tables
    hpack_dynamic_table_t encoder_table;
    hpack_dynamic_table_t decoder_table;
    
    // ---- READ CACHE ----
    // Buffers partial TLS records so http2_receive_frame can read
    // exactly 9 bytes (frame header) then exactly N bytes (payload)
    // even when both arrive in a single TLS record.
    // Replaces the old recv_buffer[16MB] that made the struct un-allocatable.
    uint8_t read_cache[HTTP2_READ_CACHE_SIZE];
    size_t  read_cache_len;      // Bytes currently valid in read_cache
    size_t  read_cache_pos;      // Next unread byte in read_cache
    
    // Error info
    uint32_t last_error;
    char error_msg[128];

    int owns_tls;       // 1 if http2_connect() created the TLS session
    int owns_socket;    // 1 if http2_connect() created the socket
    int tls_version;    // 12 or 13 (discriminator for the void* tls_session)
    
} http2_connection_t;

// ============================================================================
// HTTP/2 PUBLIC API
// ============================================================================

// Create HTTP/2 connection
http2_connection_t* http2_connect(const char* host, uint16_t port, int use_tls);

// Close HTTP/2 connection
void http2_close(http2_connection_t* conn);

// Make HTTP/2 request
int http2_request(http2_connection_t* conn, const char* method, const char* path,
                  const http2_header_t* headers, int header_count,
                  const uint8_t* body, size_t body_len,
                  uint8_t* response, size_t* response_len,
                  http2_header_t* response_headers, int* response_header_count);

// GET request
int http2_get(http2_connection_t* conn, const char* path,
              http2_header_t* request_headers, int request_header_count,
              uint8_t* response, size_t* response_len,
              http2_header_t* response_headers, int* response_header_count);

// POST request
int http2_post(http2_connection_t* conn, const char* path,
               http2_header_t* request_headers, int request_header_count,
               const uint8_t* body, size_t body_len,
               uint8_t* response, size_t* response_len,
               http2_header_t* response_headers, int* response_header_count);

http2_connection_t* http2_connect_fd(int socket_fd, void* tls_session, int use_tls,
                                      const char* host, uint16_t port);

// Initialize an HTTP/2 connection over an existing TLS session
http2_connection_t* http2_init_over_tls(void* tls_session, int socket_fd,
                                         const char* host, uint16_t port);

// Send WINDOW_UPDATE frame
int http2_send_window_update(http2_connection_t* conn, uint32_t stream_id, uint32_t increment);

// ============================================================================
// HPACK API
// ============================================================================

void hpack_init_table(hpack_dynamic_table_t* table, size_t max_size);
void hpack_free_table(hpack_dynamic_table_t* table);
int hpack_add_entry(hpack_dynamic_table_t* table, const char* name, const char* value);

int hpack_encode(hpack_dynamic_table_t* table, const http2_header_t* headers, int count,
                 uint8_t* output, size_t output_max);

int hpack_decode(hpack_dynamic_table_t* table, const uint8_t* input, size_t input_len,
                 http2_header_t* headers, int max_headers, int* header_count);

// ============================================================================
// HTTP/2 FRAME FUNCTIONS
// ============================================================================

int http2_send_settings(http2_connection_t* conn);
int http2_send_settings_ack(http2_connection_t* conn);
int http2_send_ping(http2_connection_t* conn, const uint8_t* data);
int http2_send_ping_ack(http2_connection_t* conn, const uint8_t* data);

int http2_send_headers(http2_connection_t* conn, uint32_t stream_id,
                       const http2_header_t* headers, int header_count, int end_stream);

int http2_send_data(http2_connection_t* conn, uint32_t stream_id,
                    const uint8_t* data, size_t len, int end_stream);

int http2_send_rst_stream(http2_connection_t* conn, uint32_t stream_id, uint32_t error_code);
int http2_send_goaway(http2_connection_t* conn, uint32_t last_stream_id, uint32_t error_code);
int http2_receive_frame(http2_connection_t* conn);

// ============================================================================
// INTEGER ENCODING (HPACK)
// ============================================================================

int hpack_encode_int(uint8_t* output, uint64_t value, int prefix_bits);
int hpack_decode_int(const uint8_t* input, size_t input_len, uint64_t* value, int prefix_bits, size_t* consumed);

// ============================================================================
// STRING ENCODING (HPACK)
// ============================================================================

int hpack_encode_string(uint8_t* output, const char* str, size_t max_len);
int hpack_decode_string(const uint8_t* input, size_t input_len, char* str, size_t max_len, size_t* consumed);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

http2_stream_t* http2_get_stream(http2_connection_t* conn, uint32_t stream_id);
http2_stream_t* http2_create_stream(http2_connection_t* conn);
void http2_close_stream(http2_connection_t* conn, uint32_t stream_id);
int http2_process_frame(http2_connection_t* conn, const uint8_t* frame, size_t len);

#endif // HTTP2_H