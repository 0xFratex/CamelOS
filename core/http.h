// core/http.h
#ifndef HTTP_H
#define HTTP_H

#include "../include/types.h"

#define HTTP_MAX_RESPONSE 65536

typedef struct {
    int status_code;
    char* headers;
    int headers_len;
    char* body;
    int body_len;
    int total_len;
} http_response_t;

// HTTP loading phases
typedef enum {
    HTTP_PHASE_IDLE,
    HTTP_PHASE_DNS,
    HTTP_PHASE_CONNECTING,
    HTTP_PHASE_TLS_HANDSHAKE,
    HTTP_PHASE_SENDING_REQUEST,
    HTTP_PHASE_RECEIVING_HEADERS,
    HTTP_PHASE_RECEIVING_BODY,
    HTTP_PHASE_COMPLETE,
    HTTP_PHASE_ERROR
} http_phase_t;

// Loading state for UI feedback
typedef struct {
    int is_loading;
    http_phase_t phase;
    int bytes_received;
    int total_bytes;
    char status_text[64];
    void (*progress_callback)(int bytes, int total, void* data);
    void* user_data;
} http_loading_state_t;

// Progress callback type for async operations
typedef void (*http_progress_cb)(int bytes_received, int total_bytes, void* user_data);

// HTTP Functions
int http_get(const char* url, char* response, int response_size, const char** headers, int header_count);
int http_get_simple(const char* url, char* response, int response_size);

// Async HTTP with progress callback (allows UI updates during fetch)
int http_get_async(const char* url, char* response, int response_size, 
                   const char** headers, int header_count,
                   http_progress_cb progress_cb, void* user_data);

// Get current loading state for UI feedback
http_loading_state_t* http_get_loading_state(void);

// Cancel current request (if possible)
void http_cancel_request(void);

#endif