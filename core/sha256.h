// core/sha256.h - SHA-256 Hash Function for CamelOS
// Used for encrypted password storage and data integrity verification
#ifndef SHA256_H
#define SHA256_H

#include "../include/types.h"

#define SHA256_BLOCK_SIZE 32  // 256 bits = 32 bytes

// SHA-256 context for streaming hash operations
typedef struct {
    uint32_t state[8];       // Intermediate hash state (H0..H7)
    uint64_t bit_count;      // Total bits processed
    uint8_t  buffer[64];     // 512-bit message block buffer
    uint32_t buffer_len;     // Current bytes in buffer
} sha256_ctx_t;

// Initialize SHA-256 context
void sha256_init(sha256_ctx_t* ctx);

// Update SHA-256 context with data
void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, uint32_t len);

// Finalize SHA-256 hash and get result
void sha256_final(sha256_ctx_t* ctx, uint8_t hash[SHA256_BLOCK_SIZE]);

// Convenience: Hash a buffer in one call
void sha256_hash(const uint8_t* data, uint32_t len, uint8_t hash[SHA256_BLOCK_SIZE]);

// Convenience: Hash a null-terminated string
void sha256_string(const char* str, uint8_t hash[SHA256_BLOCK_SIZE]);

// Convert SHA-256 hash to hex string (64 chars + null terminator)
void sha256_to_hex(const uint8_t hash[SHA256_BLOCK_SIZE], char hex[65]);

// Verify a password against a stored hash
// Returns 1 if password matches, 0 if not
int sha256_verify_password(const char* password, const char* stored_hash_hex);

// Hash a password and return hex string
void sha256_hash_password(const char* password, char hex[65]);

#endif // SHA256_H
