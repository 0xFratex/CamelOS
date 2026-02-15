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

// Progress callback type for async operations
typedef void (*http_progress_cb)(int bytes_received, int total_bytes, void* user_data);

// HTTP Functions
int http_get(const char* url, char* response, int response_size, const char** headers, int header_count);
int http_get_simple(const char* url, char* response, int response_size);

// Async HTTP with progress callback (allows UI updates during fetch)
int http_get_async(const char* url, char* response, int response_size, 
                   const char** headers, int header_count,
                   http_progress_cb progress_cb, void* user_data);

#endif