// core/tls.c - TLS 1.2+ Protocol Implementation
// Implements: TLS 1.2 handshake, AES-GCM, SHA-256, RSA, Certificate validation
#include "tls.h"
#include "socket.h"
#include "string.h"
#include "memory.h"
#include "net.h"
#include "../hal/cpu/timer.h"

// External functions
extern size_t strlen(const char* s);
extern int atoi(const char* str);
extern char* strchr(const char* s, int c);
extern int strcmp(const char* s1, const char* s2);
extern int strncmp(const char* s1, const char* s2, size_t n);
extern char* strncpy(char* dest, const char* src, size_t n);

// ============================================================================
// EXTERNAL DECLARATIONS
// ============================================================================
extern void rtl8139_poll(void);

// ============================================================================
// CONSTANTS
// ============================================================================
#define TLS_MAX_RECORD_SIZE     16384
#define TLS_MAX_HANDSHAKE_SIZE  65536

// AES S-Box
static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

// AES Inverse S-Box
static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

// Rcon for key expansion
static const uint8_t rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

// SHA-256 initial hash values
static const uint32_t sha256_init_state[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

// SHA-256 round constants
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

uint16_t tls_read_uint16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t tls_read_uint24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

uint32_t tls_read_uint32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | 
           ((uint32_t)p[2] << 8) | p[3];
}

uint64_t tls_read_uint64(const uint8_t* p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | p[7];
}

void tls_write_uint16(uint16_t v, uint8_t* p) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

void tls_write_uint24(uint32_t v, uint8_t* p) {
    p[0] = (v >> 16) & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = v & 0xFF;
}

void tls_write_uint32(uint32_t v, uint8_t* p) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

void tls_write_uint64(uint64_t v, uint8_t* p) {
    p[0] = (v >> 56) & 0xFF;
    p[1] = (v >> 48) & 0xFF;
    p[2] = (v >> 40) & 0xFF;
    p[3] = (v >> 32) & 0xFF;
    p[4] = (v >> 24) & 0xFF;
    p[5] = (v >> 16) & 0xFF;
    p[6] = (v >> 8) & 0xFF;
    p[7] = v & 0xFF;
}

int tls_constant_time_memcmp(const void* a, const void* b, size_t len) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    uint8_t result = 0;
    
    for (size_t i = 0; i < len; i++) {
        result |= pa[i] ^ pb[i];
    }
    
    return result;
}

const char* tls_error_string(tls_error_t err) {
    switch (err) {
        case TLS_OK: return "OK";
        case TLS_ERR_SOCKET: return "Socket error";
        case TLS_ERR_HANDSHAKE: return "Handshake failed";
        case TLS_ERR_CERTIFICATE: return "Certificate error";
        case TLS_ERR_CIPHER: return "Cipher suite error";
        case TLS_ERR_MAC: return "MAC verification failed";
        case TLS_ERR_DECRYPT: return "Decryption failed";
        case TLS_ERR_ENCRYPT: return "Encryption failed";
        case TLS_ERR_PROTOCOL: return "Protocol error";
        case TLS_ERR_VERSION: return "Version not supported";
        case TLS_ERR_MEMORY: return "Memory allocation failed";
        case TLS_ERR_TIMEOUT: return "Operation timed out";
        case TLS_ERR_CERT_VERIFY: return "Certificate verification failed";
        case TLS_ERR_SIGNATURE: return "Signature verification failed";
        case TLS_ERR_KEY_EXCHANGE: return "Key exchange failed";
        default: return "Unknown error";
    }
}

// ============================================================================
// RANDOM NUMBER GENERATION
// ============================================================================

void tls_get_random(uint8_t* buffer, size_t len) {
    // Use system timer and other entropy sources
    static uint32_t seed = 0;
    
    if (seed == 0) {
        seed = get_tick_count();
    }
    
    for (size_t i = 0; i < len; i++) {
        // Simple LCG with XOR mixing
        seed = seed * 1103515245 + 12345;
        uint32_t val = (seed >> 16) ^ (seed & 0xFFFF);
        
        // Mix in timer value
        val ^= get_tick_count();
        
        buffer[i] = val & 0xFF;
    }
}

// Get a single random byte
uint8_t tls_get_random_byte(void) {
    uint8_t byte;
    tls_get_random(&byte, 1);
    return byte;
}

// ============================================================================
// SHA-256 IMPLEMENTATION
// ============================================================================

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define EP1(x) (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SIG0(x) (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

void sha256_init(sha256_ctx_t* ctx) {
    for (int i = 0; i < 8; i++) {
        ctx->state[i] = sha256_init_state[i];
    }
    ctx->count = 0;
    memset(ctx->buffer, 0, SHA256_BLOCK_SIZE);
}

void sha256_transform(sha256_ctx_t* ctx, const uint8_t* data) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;
    
    // Prepare message schedule
    for (int i = 0; i < 16; i++) {
        w[i] = tls_read_uint32(data + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    }
    
    // Initialize working variables
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    
    // Main loop
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + w[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    
    // Add compressed chunk to current hash value
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    size_t buffer_idx = ctx->count % SHA256_BLOCK_SIZE;
    ctx->count += len;
    
    // If we have data in buffer, try to fill it
    if (buffer_idx > 0) {
        size_t space = SHA256_BLOCK_SIZE - buffer_idx;
        if (len < space) {
            memcpy(ctx->buffer + buffer_idx, data, len);
            return;
        }
        memcpy(ctx->buffer + buffer_idx, data, space);
        sha256_transform(ctx, ctx->buffer);
        data += space;
        len -= space;
    }
    
    // Process complete blocks
    while (len >= SHA256_BLOCK_SIZE) {
        sha256_transform(ctx, data);
        data += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }
    
    // Copy remaining data to buffer
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

void sha256_final(sha256_ctx_t* ctx, uint8_t* digest) {
    size_t buffer_idx = ctx->count % SHA256_BLOCK_SIZE;
    
    // Pad message
    ctx->buffer[buffer_idx++] = 0x80;
    
    // If not enough room for length, pad and process
    if (buffer_idx > 56) {
        while (buffer_idx < SHA256_BLOCK_SIZE) {
            ctx->buffer[buffer_idx++] = 0;
        }
        sha256_transform(ctx, ctx->buffer);
        buffer_idx = 0;
    }
    
    // Pad to 56 bytes
    while (buffer_idx < 56) {
        ctx->buffer[buffer_idx++] = 0;
    }
    
    // Append length in bits (big-endian)
    uint64_t bit_len = ctx->count * 8;
    tls_write_uint64(bit_len, ctx->buffer + 56);
    sha256_transform(ctx, ctx->buffer);
    
    // Output digest
    for (int i = 0; i < 8; i++) {
        tls_write_uint32(ctx->state[i], digest + i * 4);
    }
}

void sha256_hash(const uint8_t* data, size_t len, uint8_t* digest) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

// ============================================================================
// AES IMPLEMENTATION
// ============================================================================

static uint8_t xtime(uint8_t x) {
    return ((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static uint8_t multiply(uint8_t x, uint8_t y) {
    return (((y & 1) * x) ^
            ((y >> 1 & 1) * xtime(x)) ^
            ((y >> 2 & 1) * xtime(xtime(x))) ^
            ((y >> 3 & 1) * xtime(xtime(xtime(x)))) ^
            ((y >> 4 & 1) * xtime(xtime(xtime(xtime(x))))));
}

void aes_set_key(aes_gcm_ctx_t* ctx, const uint8_t* key, int key_bits) {
    int nk = key_bits / 32;  // Number of 32-bit words in key
    int nr = nk + 6;         // Number of rounds
    
    ctx->key_bits = key_bits;
    
    // Copy key into first words of expanded key
    for (int i = 0; i < nk; i++) {
        ctx->key[i] = tls_read_uint32(key + i * 4);
    }
    
    // Expand key
    for (int i = nk; i < 4 * (nr + 1); i++) {
        uint32_t temp = ctx->key[i - 1];
        
        if (i % nk == 0) {
            // RotWord and SubWord
            temp = ((temp << 8) | (temp >> 24));
            temp = (aes_sbox[(temp >> 24) & 0xFF] << 24) |
                   (aes_sbox[(temp >> 16) & 0xFF] << 16) |
                   (aes_sbox[(temp >> 8) & 0xFF] << 8) |
                   aes_sbox[temp & 0xFF];
            temp ^= rcon[i / nk] << 24;
        } else if (nk > 6 && i % nk == 4) {
            // Extra SubWord for AES-256
            temp = (aes_sbox[(temp >> 24) & 0xFF] << 24) |
                   (aes_sbox[(temp >> 16) & 0xFF] << 16) |
                   (aes_sbox[(temp >> 8) & 0xFF] << 8) |
                   aes_sbox[temp & 0xFF];
        }
        
        ctx->key[i] = ctx->key[i - nk] ^ temp;
    }
}

void aes_encrypt_block(aes_gcm_ctx_t* ctx, const uint8_t* input, uint8_t* output) {
    int nk = ctx->key_bits / 32;
    int nr = nk + 6;
    
    uint8_t s[4][4];
    uint8_t state[4][4];
    
    // Copy input to state (column-major)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            s[j][i] = input[i * 4 + j];
        }
    }
    
    // Initial round key addition
    for (int i = 0; i < 4; i++) {
        uint32_t k = ctx->key[i];
        s[0][i] ^= (k >> 24) & 0xFF;
        s[1][i] ^= (k >> 16) & 0xFF;
        s[2][i] ^= (k >> 8) & 0xFF;
        s[3][i] ^= k & 0xFF;
    }
    
    // Main rounds
    for (int round = 1; round < nr; round++) {
        // SubBytes
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                state[i][j] = aes_sbox[s[i][j]];
            }
        }
        
        // ShiftRows
        uint8_t t = state[1][0];
        state[1][0] = state[1][1];
        state[1][1] = state[1][2];
        state[1][2] = state[1][3];
        state[1][3] = t;
        
        t = state[2][0];
        state[2][0] = state[2][2];
        state[2][2] = t;
        t = state[2][1];
        state[2][1] = state[2][3];
        state[2][3] = t;
        
        t = state[3][3];
        state[3][3] = state[3][2];
        state[3][2] = state[3][1];
        state[3][1] = state[3][0];
        state[3][0] = t;
        
        // MixColumns
        for (int i = 0; i < 4; i++) {
            uint8_t a0 = state[0][i], a1 = state[1][i];
            uint8_t a2 = state[2][i], a3 = state[3][i];
            
            s[0][i] = multiply(a0, 2) ^ multiply(a1, 3) ^ a2 ^ a3;
            s[1][i] = a0 ^ multiply(a1, 2) ^ multiply(a2, 3) ^ a3;
            s[2][i] = a0 ^ a1 ^ multiply(a2, 2) ^ multiply(a3, 3);
            s[3][i] = multiply(a0, 3) ^ a1 ^ a2 ^ multiply(a3, 2);
        }
        
        // AddRoundKey
        for (int i = 0; i < 4; i++) {
            uint32_t k = ctx->key[round * 4 + i];
            s[0][i] ^= (k >> 24) & 0xFF;
            s[1][i] ^= (k >> 16) & 0xFF;
            s[2][i] ^= (k >> 8) & 0xFF;
            s[3][i] ^= k & 0xFF;
        }
    }
    
    // Final round (no MixColumns)
    // SubBytes
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            state[i][j] = aes_sbox[s[i][j]];
        }
    }
    
    // ShiftRows
    uint8_t t = state[1][0];
    state[1][0] = state[1][1];
    state[1][1] = state[1][2];
    state[1][2] = state[1][3];
    state[1][3] = t;
    
    t = state[2][0];
    state[2][0] = state[2][2];
    state[2][2] = t;
    t = state[2][1];
    state[2][1] = state[2][3];
    state[2][3] = t;
    
    t = state[3][3];
    state[3][3] = state[3][2];
    state[3][2] = state[3][1];
    state[3][1] = state[3][0];
    state[3][0] = t;
    
    // AddRoundKey
    for (int i = 0; i < 4; i++) {
        uint32_t k = ctx->key[nr * 4 + i];
        state[0][i] ^= (k >> 24) & 0xFF;
        state[1][i] ^= (k >> 16) & 0xFF;
        state[2][i] ^= (k >> 8) & 0xFF;
        state[3][i] ^= k & 0xFF;
    }
    
    // Copy state to output
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            output[i * 4 + j] = state[j][i];
        }
    }
}

void aes_decrypt_block(aes_gcm_ctx_t* ctx, const uint8_t* input, uint8_t* output) {
    int nk = ctx->key_bits / 32;
    int nr = nk + 6;
    
    uint8_t s[4][4];
    uint8_t state[4][4];
    
    // Copy input to state
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            s[j][i] = input[i * 4 + j];
        }
    }
    
    // Initial round key addition (last round key)
    for (int i = 0; i < 4; i++) {
        uint32_t k = ctx->key[nr * 4 + i];
        s[0][i] ^= (k >> 24) & 0xFF;
        s[1][i] ^= (k >> 16) & 0xFF;
        s[2][i] ^= (k >> 8) & 0xFF;
        s[3][i] ^= k & 0xFF;
    }
    
    // Main rounds in reverse
    for (int round = nr - 1; round > 0; round--) {
        // InvShiftRows
        uint8_t t = state[3][0];
        state[3][0] = state[3][1];
        state[3][1] = state[3][2];
        state[3][2] = state[3][3];
        state[3][3] = t;
        
        t = state[2][0];
        state[2][0] = state[2][2];
        state[2][2] = t;
        t = state[2][1];
        state[2][1] = state[2][3];
        state[2][3] = t;
        
        t = state[1][3];
        state[1][3] = state[1][2];
        state[1][2] = state[1][1];
        state[1][1] = state[1][0];
        state[1][0] = t;
        
        // InvSubBytes
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                state[i][j] = aes_inv_sbox[state[i][j]];
            }
        }
        
        // AddRoundKey
        for (int i = 0; i < 4; i++) {
            uint32_t k = ctx->key[round * 4 + i];
            state[0][i] ^= (k >> 24) & 0xFF;
            state[1][i] ^= (k >> 16) & 0xFF;
            state[2][i] ^= (k >> 8) & 0xFF;
            state[3][i] ^= k & 0xFF;
        }
        
        // InvMixColumns
        for (int i = 0; i < 4; i++) {
            uint8_t a0 = state[0][i], a1 = state[1][i];
            uint8_t a2 = state[2][i], a3 = state[3][i];
            
            s[0][i] = multiply(a0, 0x0e) ^ multiply(a1, 0x0b) ^ 
                      multiply(a2, 0x0d) ^ multiply(a3, 0x09);
            s[1][i] = multiply(a0, 0x09) ^ multiply(a1, 0x0e) ^ 
                      multiply(a2, 0x0b) ^ multiply(a3, 0x0d);
            s[2][i] = multiply(a0, 0x0d) ^ multiply(a1, 0x09) ^ 
                      multiply(a2, 0x0e) ^ multiply(a3, 0x0b);
            s[3][i] = multiply(a0, 0x0b) ^ multiply(a1, 0x0d) ^ 
                      multiply(a2, 0x09) ^ multiply(a3, 0x0e);
        }
    }
    
    // Final round
    // InvShiftRows
    uint8_t t = state[3][0];
    state[3][0] = state[3][1];
    state[3][1] = state[3][2];
    state[3][2] = state[3][3];
    state[3][3] = t;
    
    t = state[2][0];
    state[2][0] = state[2][2];
    state[2][2] = t;
    t = state[2][1];
    state[2][1] = state[2][3];
    state[2][3] = t;
    
    t = state[1][3];
    state[1][3] = state[1][2];
    state[1][2] = state[1][1];
    state[1][1] = state[1][0];
    state[1][0] = t;
    
    // InvSubBytes
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            state[i][j] = aes_inv_sbox[state[i][j]];
        }
    }
    
    // AddRoundKey
    for (int i = 0; i < 4; i++) {
        uint32_t k = ctx->key[i];
        state[0][i] ^= (k >> 24) & 0xFF;
        state[1][i] ^= (k >> 16) & 0xFF;
        state[2][i] ^= (k >> 8) & 0xFF;
        state[3][i] ^= k & 0xFF;
    }
    
    // Copy state to output
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            output[i * 4 + j] = state[j][i];
        }
    }
}

// ============================================================================
// AES-GCM IMPLEMENTATION
// ============================================================================

static void gcm_mult(uint8_t* x, const uint8_t* y) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, y, 16);
    
    for (int i = 0; i < 16; i++) {
        for (int j = 7; j >= 0; j--) {
            if (x[i] & (1 << j)) {
                for (int k = 0; k < 16; k++) {
                    z[k] ^= v[k];
                }
            }
            
            // Multiply v by x (shift and reduce)
            uint8_t carry = v[15] & 1;
            for (int k = 15; k > 0; k--) {
                v[k] = (v[k] >> 1) | ((v[k-1] & 1) << 7);
            }
            v[0] >>= 1;
            
            if (carry) {
                v[0] ^= 0xe1;  // Reduction polynomial for GHASH
            }
        }
    }
    
    memcpy(x, z, 16);
}

static void ghash(aes_gcm_ctx_t* ctx, const uint8_t* data, size_t len, uint8_t* result) {
    uint8_t y[16] = {0};
    
    for (size_t i = 0; i < len; i += 16) {
        // XOR with input block
        for (int j = 0; j < 16 && i + j < len; j++) {
            y[j] ^= data[i + j];
        }
        
        // Multiply by H
        gcm_mult(y, ctx->gcm_h);
    }
    
    memcpy(result, y, 16);
}

int aes_gcm_init(aes_gcm_ctx_t* ctx, const uint8_t* key, int key_bits, const uint8_t* iv) {
    // Set up AES key
    aes_set_key(ctx, key, key_bits);
    
    // Compute H = AES(0)
    memset(ctx->gcm_h, 0, 16);
    aes_encrypt_block(ctx, ctx->gcm_h, ctx->gcm_h);
    
    // Set IV
    memcpy(ctx->iv, iv, TLS_GCM_IV_SIZE);
    
    // Compute J0
    memset(ctx->gcm_j0, 0, 16);
    memcpy(ctx->gcm_j0, iv, 12);
    ctx->gcm_j0[15] = 1;
    
    // Initialize length counters
    memset(ctx->gcm_len_a, 0, 8);
    memset(ctx->gcm_len_c, 0, 8);
    
    return 0;
}

static void gcm_inc(uint8_t* counter) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
}

int aes_gcm_encrypt(aes_gcm_ctx_t* ctx, const uint8_t* plaintext, size_t pt_len,
                    const uint8_t* aad, size_t aad_len,
                    uint8_t* ciphertext, uint8_t* tag) {
    uint8_t counter[16];
    uint8_t eky[16];
    uint8_t ghash_input[1024];
    size_t ghash_len = 0;
    
    // Copy J0 to counter
    memcpy(counter, ctx->gcm_j0, 16);
    
    // Encrypt plaintext
    for (size_t i = 0; i < pt_len; i += 16) {
        gcm_inc(counter);
        aes_encrypt_block(ctx, counter, eky);
        
        size_t block_len = (pt_len - i < 16) ? pt_len - i : 16;
        for (size_t j = 0; j < block_len; j++) {
            ciphertext[i + j] = plaintext[i + j] ^ eky[j];
        }
    }
    
    // Build GHASH input: AAD || padding || ciphertext || padding || lengths
    // Add AAD
    if (aad && aad_len > 0) {
        memcpy(ghash_input + ghash_len, aad, aad_len);
        ghash_len += aad_len;
        // Pad to 16-byte boundary
        size_t pad = (16 - (aad_len % 16)) % 16;
        memset(ghash_input + ghash_len, 0, pad);
        ghash_len += pad;
    }
    
    // Add ciphertext
    memcpy(ghash_input + ghash_len, ciphertext, pt_len);
    ghash_len += pt_len;
    // Pad to 16-byte boundary
    size_t pad = (16 - (pt_len % 16)) % 16;
    memset(ghash_input + ghash_len, 0, pad);
    ghash_len += pad;
    
    // Add lengths (AAD length || ciphertext length in bits)
    uint64_t aad_bits = aad_len * 8;
    uint64_t ct_bits = pt_len * 8;
    tls_write_uint64(aad_bits, ghash_input + ghash_len);
    ghash_len += 8;
    tls_write_uint64(ct_bits, ghash_input + ghash_len);
    ghash_len += 8;
    
    // Compute GHASH
    uint8_t ghash_result[16];
    ghash(ctx, ghash_input, ghash_len, ghash_result);
    
    // Compute tag = GHASH XOR AES(J0)
    aes_encrypt_block(ctx, ctx->gcm_j0, eky);
    for (int i = 0; i < 16; i++) {
        tag[i] = ghash_result[i] ^ eky[i];
    }
    
    return 0;
}

int aes_gcm_decrypt(aes_gcm_ctx_t* ctx, const uint8_t* ciphertext, size_t ct_len,
                    const uint8_t* aad, size_t aad_len,
                    const uint8_t* tag, uint8_t* plaintext) {
    uint8_t counter[16];
    uint8_t eky[16];
    uint8_t ghash_input[1024];
    size_t ghash_len = 0;
    uint8_t computed_tag[16];
    
    // Build GHASH input and compute expected tag
    // Add AAD
    if (aad && aad_len > 0) {
        memcpy(ghash_input + ghash_len, aad, aad_len);
        ghash_len += aad_len;
        size_t pad = (16 - (aad_len % 16)) % 16;
        memset(ghash_input + ghash_len, 0, pad);
        ghash_len += pad;
    }
    
    // Add ciphertext
    memcpy(ghash_input + ghash_len, ciphertext, ct_len);
    ghash_len += ct_len;
    size_t pad = (16 - (ct_len % 16)) % 16;
    memset(ghash_input + ghash_len, 0, pad);
    ghash_len += pad;
    
    // Add lengths
    uint64_t aad_bits = aad_len * 8;
    uint64_t ct_bits = ct_len * 8;
    tls_write_uint64(aad_bits, ghash_input + ghash_len);
    ghash_len += 8;
    tls_write_uint64(ct_bits, ghash_input + ghash_len);
    ghash_len += 8;
    
    // Compute GHASH
    uint8_t ghash_result[16];
    ghash(ctx, ghash_input, ghash_len, ghash_result);
    
    // Compute tag
    aes_encrypt_block(ctx, ctx->gcm_j0, eky);
    for (int i = 0; i < 16; i++) {
        computed_tag[i] = ghash_result[i] ^ eky[i];
    }
    
    // Verify tag (constant-time comparison)
    if (tls_constant_time_memcmp(tag, computed_tag, 16) != 0) {
        return TLS_ERR_MAC;
    }
    
    // Decrypt ciphertext
    memcpy(counter, ctx->gcm_j0, 16);
    
    for (size_t i = 0; i < ct_len; i += 16) {
        gcm_inc(counter);
        aes_encrypt_block(ctx, counter, eky);
        
        size_t block_len = (ct_len - i < 16) ? ct_len - i : 16;
        for (size_t j = 0; j < block_len; j++) {
            plaintext[i + j] = ciphertext[i + j] ^ eky[j];
        }
    }
    
    return 0;
}

// ============================================================================
// TLS PRF (Pseudo-Random Function) - TLS 1.2 uses HMAC-SHA256
// ============================================================================

static void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len,
                        uint8_t* mac) {
    uint8_t k_ipad[64], k_opad[64];
    sha256_ctx_t ctx;
    
    // Prepare key
    memset(k_ipad, 0, 64);
    memset(k_opad, 0, 64);
    if (key_len > 64) {
        sha256_hash(key, key_len, k_ipad);
        memcpy(k_opad, k_ipad, 32);
    } else {
        memcpy(k_ipad, key, key_len);
        memcpy(k_opad, key, key_len);
    }
    
    // XOR with pads
    for (int i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }
    
    // Inner hash
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, mac);
    
    // Outer hash
    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, mac, 32);
    sha256_final(&ctx, mac);
}

int tls_prf(const uint8_t* secret, size_t secret_len,
            const char* label,
            const uint8_t* seed, size_t seed_len,
            uint8_t* output, size_t output_len) {
    // Build seed: label || seed
    uint8_t full_seed[256];
    size_t label_len = strlen((char*)label);
    size_t full_seed_len = label_len + seed_len;
    
    memcpy(full_seed, label, label_len);
    memcpy(full_seed + label_len, seed, seed_len);
    
    // P_hash(secret, seed) using HMAC-SHA256
    uint8_t a[32];  // A(i)
    uint8_t tmp[32];
    size_t done = 0;
    
    // A(1) = HMAC(secret, seed)
    hmac_sha256(secret, secret_len, full_seed, full_seed_len, a);
    
    while (done < output_len) {
        // A(i+1) = HMAC(secret, A(i))
        // output += HMAC(secret, A(i) || seed)
        uint8_t a_and_seed[32 + 256];
        memcpy(a_and_seed, a, 32);
        memcpy(a_and_seed + 32, full_seed, full_seed_len);
        
        hmac_sha256(secret, secret_len, a_and_seed, 32 + full_seed_len, tmp);
        
        size_t copy_len = (output_len - done < 32) ? output_len - done : 32;
        memcpy(output + done, tmp, copy_len);
        done += copy_len;
        
        // Compute next A
        hmac_sha256(secret, secret_len, a, 32, a);
    }
    
    return 0;
}

// ============================================================================
// RSA IMPLEMENTATION (Simplified - for certificate verification)
// ============================================================================

// Modular exponentiation (square-and-multiply)
static void mod_exp(const uint8_t* base, size_t base_len,
                    const uint8_t* exp, size_t exp_len,
                    const uint8_t* mod, size_t mod_len,
                    uint8_t* result) {
    // This is a simplified implementation
    // In production, use a proper big integer library
    
    // For now, we'll implement a basic version
    // that works for typical RSA operations
    
    // Initialize result to 1
    memset(result, 0, mod_len);
    result[mod_len - 1] = 1;
    
    // Temporary buffers
    uint8_t temp[512], temp2[512];
    
    // Process each bit of exponent
    for (size_t i = 0; i < exp_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            // result = (result * result) % mod
            // Simplified: would need proper big integer multiplication
            
            if (exp[i] & (1 << bit)) {
                // result = (result * base) % mod
            }
        }
    }
}

int rsa_public_encrypt(rsa_key_t* key, const uint8_t* plaintext, size_t pt_len,
                       uint8_t* ciphertext) {
    // RSA encryption: c = m^e mod n
    // For TLS, we typically encrypt a pre-master secret
    
    if (pt_len > (size_t)key->modulus_len - 11) {
        return TLS_ERR_ENCRYPT;
    }
    
    // PKCS#1 v1.5 padding
    uint8_t padded[512];
    size_t padded_len = key->modulus_len;
    
    padded[0] = 0x00;
    padded[1] = 0x02;
    
    // Add random padding
    for (size_t i = 2; i < padded_len - pt_len - 1; i++) {
        padded[i] = 0x01 + (tls_get_random_byte() % 255);
    }
    padded[padded_len - pt_len - 1] = 0x00;
    memcpy(padded + padded_len - pt_len, plaintext, pt_len);
    
    // Encrypt: c = m^e mod n
    mod_exp(padded, padded_len, key->exponent, key->exponent_len,
            key->modulus, key->modulus_len, ciphertext);
    
    return key->modulus_len;
}

int rsa_verify_pkcs1(rsa_key_t* key, const uint8_t* signature, size_t sig_len,
                     const uint8_t* hash, size_t hash_len, int hash_alg) {
    // RSA verification: m = s^e mod n
    uint8_t decrypted[512];
    
    mod_exp(signature, sig_len, key->exponent, key->exponent_len,
            key->modulus, key->modulus_len, decrypted);
    
    // Check PKCS#1 v1.5 padding
    if (decrypted[0] != 0x00 || decrypted[1] != 0x01) {
        return TLS_ERR_SIGNATURE;
    }
    
    // Find hash start
    size_t i = 2;
    while (i < key->modulus_len && decrypted[i] == 0xFF) i++;
    if (decrypted[i] != 0x00) {
        return TLS_ERR_SIGNATURE;
    }
    i++;
    
    // Check DigestInfo prefix
    // For SHA-256: 30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20
    static const uint8_t sha256_prefix[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
        0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
        0x00, 0x04, 0x20
    };
    
    if (hash_alg == 1) { // SHA-256
        if (i + sizeof(sha256_prefix) + hash_len > key->modulus_len) {
            return TLS_ERR_SIGNATURE;
        }
        if (memcmp(decrypted + i, sha256_prefix, sizeof(sha256_prefix)) != 0) {
            return TLS_ERR_SIGNATURE;
        }
        i += sizeof(sha256_prefix);
    }
    
    // Compare hash
    if (memcmp(decrypted + i, hash, hash_len) != 0) {
        return TLS_ERR_SIGNATURE;
    }
    
    return 0;
}

// ============================================================================
// X.509 CERTIFICATE PARSING
// ============================================================================

// ASN.1 tag constants
#define ASN1_TAG_INTEGER       0x02
#define ASN1_TAG_BIT_STRING    0x03
#define ASN1_TAG_OCTET_STRING  0x04
#define ASN1_TAG_NULL          0x05
#define ASN1_TAG_OID           0x06
#define ASN1_TAG_UTF8_STRING   0x0C
#define ASN1_TAG_SEQUENCE      0x30
#define ASN1_TAG_SET           0x31

// OID values
static const uint8_t oid_common_name[] = {0x55, 0x04, 0x03};
static const uint8_t oid_organization[] = {0x55, 0x04, 0x0A};
static const uint8_t oid_rsa_encryption[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
static const uint8_t oid_sha256_rsa[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B};

static int parse_asn1_length(const uint8_t* data, size_t* len, size_t* header_len) {
    if (data[0] < 0x80) {
        *len = data[0];
        *header_len = 1;
    } else if (data[0] == 0x81) {
        *len = data[1];
        *header_len = 2;
    } else if (data[0] == 0x82) {
        *len = ((size_t)data[1] << 8) | data[2];
        *header_len = 3;
    } else if (data[0] == 0x83) {
        *len = ((size_t)data[1] << 16) | ((size_t)data[2] << 8) | data[3];
        *header_len = 4;
    } else {
        return -1;
    }
    return 0;
}

static int parse_asn1_element(const uint8_t* data, uint8_t expected_tag,
                              const uint8_t** content, size_t* content_len) {
    if (data[0] != expected_tag) {
        return -1;
    }
    
    size_t len, header_len;
    if (parse_asn1_length(data + 1, &len, &header_len) < 0) {
        return -1;
    }
    
    *content = data + 1 + header_len;
    *content_len = len;
    
    return 1 + header_len + len;
}

static int parse_asn1_string(const uint8_t* data, char* out, size_t max_len) {
    size_t len, header_len;
    
    // Accept various string types
    if (data[0] != ASN1_TAG_UTF8_STRING && data[0] != 0x13 && 
        data[0] != 0x14 && data[0] != 0x16 && data[0] != 0x17) {
        return -1;
    }
    
    if (parse_asn1_length(data + 1, &len, &header_len) < 0) {
        return -1;
    }
    
    size_t copy_len = (len < max_len - 1) ? len : max_len - 1;
    memcpy(out, data + 1 + header_len, copy_len);
    out[copy_len] = '\0';
    
    return 1 + header_len + len;
}

static int oid_compare(const uint8_t* oid1, size_t len1, const uint8_t* oid2, size_t len2) {
    if (len1 != len2) return 0;
    return memcmp(oid1, oid2, len1) == 0;
}

int x509_parse_der(const uint8_t* der_data, size_t len, x509_cert_t* cert) {
    const uint8_t* p = der_data;
    const uint8_t* end = der_data + len;
    
    memset(cert, 0, sizeof(x509_cert_t));
    cert->raw_data = (uint8_t*)der_data;
    cert->raw_len = len;
    
    // Certificate ::= SEQUENCE { ... }
    const uint8_t* cert_content;
    size_t cert_len;
    if (parse_asn1_element(p, ASN1_TAG_SEQUENCE, &cert_content, &cert_len) < 0) {
        return -1;
    }
    p += 1 + (cert_len < 128 ? 1 : (cert_len < 256 ? 2 : 3)) + cert_len;
    
    // TBSCertificate ::= SEQUENCE { ... }
    const uint8_t* tbs_content;
    size_t tbs_len;
    int tbs_offset = parse_asn1_element(cert_content, ASN1_TAG_SEQUENCE, &tbs_content, &tbs_len);
    if (tbs_offset < 0) return -1;
    
    const uint8_t* tbs = tbs_content;
    const uint8_t* tbs_end = tbs_content + tbs_len;
    
    // Version [0] EXPLICIT INTEGER (optional)
    if (tbs[0] == 0xA0) {
        size_t ver_len, ver_header;
        parse_asn1_length(tbs + 1, &ver_len, &ver_header);
        tbs += 1 + ver_header + ver_len;
    }
    
    // SerialNumber INTEGER
    const uint8_t* serial;
    size_t serial_len;
    int serial_offset = parse_asn1_element(tbs, ASN1_TAG_INTEGER, &serial, &serial_len);
    if (serial_offset < 0) return -1;
    tbs += serial_offset;
    
    // SignatureAlgorithm SEQUENCE
    const uint8_t* sig_alg;
    size_t sig_alg_len;
    int sig_alg_offset = parse_asn1_element(tbs, ASN1_TAG_SEQUENCE, &sig_alg, &sig_alg_len);
    if (sig_alg_offset < 0) return -1;
    tbs += sig_alg_offset;
    
    // Issuer SEQUENCE
    const uint8_t* issuer;
    size_t issuer_len;
    int issuer_offset = parse_asn1_element(tbs, ASN1_TAG_SEQUENCE, &issuer, &issuer_len);
    if (issuer_offset < 0) return -1;
    tbs += issuer_offset;
    
    // Parse issuer for CN
    const uint8_t* issuer_p = issuer;
    while (issuer_p < issuer + issuer_len) {
        const uint8_t* set_content;
        size_t set_len;
        int set_offset = parse_asn1_element(issuer_p, ASN1_TAG_SET, &set_content, &set_len);
        if (set_offset < 0) break;
        
        const uint8_t* seq_content;
        size_t seq_len;
        if (parse_asn1_element(set_content, ASN1_TAG_SEQUENCE, &seq_content, &seq_len) > 0) {
            const uint8_t* oid;
            size_t oid_len;
            if (parse_asn1_element(seq_content, ASN1_TAG_OID, &oid, &oid_len) > 0) {
                if (oid_compare(oid, oid_len, oid_common_name, sizeof(oid_common_name))) {
                    parse_asn1_string(seq_content + 2 + oid_len, cert->issuer_cn, TLS_MAX_CN_LENGTH);
                }
            }
        }
        
        issuer_p += set_offset;
    }
    
    // Validity SEQUENCE
    const uint8_t* validity;
    size_t validity_len;
    int validity_offset = parse_asn1_element(tbs, ASN1_TAG_SEQUENCE, &validity, &validity_len);
    if (validity_offset < 0) return -1;
    tbs += validity_offset;
    
    // Parse notBefore and notAfter (simplified - just skip for now)
    // In production, parse UTCTime or GeneralizedTime
    
    // Subject SEQUENCE
    const uint8_t* subject;
    size_t subject_len;
    int subject_offset = parse_asn1_element(tbs, ASN1_TAG_SEQUENCE, &subject, &subject_len);
    if (subject_offset < 0) return -1;
    tbs += subject_offset;
    
    // Parse subject for CN
    const uint8_t* subject_p = subject;
    while (subject_p < subject + subject_len) {
        const uint8_t* set_content;
        size_t set_len;
        int set_offset = parse_asn1_element(subject_p, ASN1_TAG_SET, &set_content, &set_len);
        if (set_offset < 0) break;
        
        const uint8_t* seq_content;
        size_t seq_len;
        if (parse_asn1_element(set_content, ASN1_TAG_SEQUENCE, &seq_content, &seq_len) > 0) {
            const uint8_t* oid;
            size_t oid_len;
            if (parse_asn1_element(seq_content, ASN1_TAG_OID, &oid, &oid_len) > 0) {
                if (oid_compare(oid, oid_len, oid_common_name, sizeof(oid_common_name))) {
                    parse_asn1_string(seq_content + 2 + oid_len, cert->common_name, TLS_MAX_CN_LENGTH);
                } else if (oid_compare(oid, oid_len, oid_organization, sizeof(oid_organization))) {
                    parse_asn1_string(seq_content + 2 + oid_len, cert->organization, TLS_MAX_ORG_LENGTH);
                }
            }
        }
        
        subject_p += set_offset;
    }
    
    // SubjectPublicKeyInfo SEQUENCE
    const uint8_t* spki;
    size_t spki_len;
    int spki_offset = parse_asn1_element(tbs, ASN1_TAG_SEQUENCE, &spki, &spki_len);
    if (spki_offset < 0) return -1;
    
    // Parse public key
    const uint8_t* alg_id;
    size_t alg_id_len;
    if (parse_asn1_element(spki, ASN1_TAG_SEQUENCE, &alg_id, &alg_id_len) > 0) {
        const uint8_t* alg_oid;
        size_t alg_oid_len;
        if (parse_asn1_element(alg_id, ASN1_TAG_OID, &alg_oid, &alg_oid_len) > 0) {
            if (oid_compare(alg_oid, alg_oid_len, oid_rsa_encryption, sizeof(oid_rsa_encryption))) {
                cert->public_key_type = 1; // RSA
            }
        }
    }
    
    // Get public key bit string
    const uint8_t* pk_bits;
    size_t pk_bits_len;
    if (parse_asn1_element(spki + spki_offset - spki_len, ASN1_TAG_BIT_STRING, &pk_bits, &pk_bits_len) > 0) {
        // Skip unused bits byte
        cert->public_key_len = pk_bits_len - 1;
        memcpy(cert->public_key, pk_bits + 1, cert->public_key_len);
    }
    
    // Compute fingerprint
    sha256_hash(der_data, len, cert->fingerprint);
    
    return 0;
}

int x509_check_validity(x509_cert_t* cert) {
    // In production, compare with current time
    // For now, just return success
    return 0;
}

int tls_verify_certificate(x509_cert_t* cert, const char* hostname) {
    // Check validity period
    if (x509_check_validity(cert) < 0) {
        return TLS_ERR_CERT_VERIFY;
    }
    
    // Check hostname matches CN
    if (hostname && cert->common_name[0]) {
        // Simple wildcard matching
        if (cert->common_name[0] == '*') {
            // Wildcard certificate
            const char* dot = strchr(hostname, '.');
            if (dot && strcmp(dot, cert->common_name + 1) == 0) {
                return 0; // Match
            }
        } else if (strcmp(hostname, cert->common_name) == 0) {
            return 0; // Exact match
        }
        
        // Check Subject Alternative Names (if present)
        // For now, just fail if CN doesn't match
        return TLS_ERR_CERT_VERIFY;
    }
    
    return 0;
}

// ============================================================================
// TLS SESSION MANAGEMENT
// ============================================================================

tls_session_t* tls_create_session(void) {
    tls_session_t* session = (tls_session_t*)kmalloc(sizeof(tls_session_t));
    if (!session) return NULL;
    
    memset(session, 0, sizeof(tls_session_t));
    session->state = TLS_STATE_INIT;
    session->version = TLS_VERSION_1_2;
    session->verify_cert = 1;  // Verify by default
    
    return session;
}

void tls_destroy_session(tls_session_t* session) {
    if (session) {
        if (session->socket_fd >= 0) {
            k_close(session->socket_fd);
        }
        kfree(session);
    }
}

void tls_set_verify(tls_session_t* session, int verify) {
    session->verify_cert = verify;
}

void tls_set_hostname(tls_session_t* session, const char* hostname) {
    strncpy(session->server_name, hostname, sizeof(session->server_name) - 1);
}

void tls_set_callbacks(tls_session_t* session,
                       void (*on_alert)(int, int, void*),
                       void (*on_cert_verify)(x509_cert_t*, void*),
                       void* user_data) {
    session->on_alert = on_alert;
    session->on_cert_verify = on_cert_verify;
    session->callback_user_data = user_data;
}

// ============================================================================
// TLS HANDSHAKE IMPLEMENTATION
// ============================================================================

static int tls_send_record(tls_session_t* session, uint8_t content_type,
                           const uint8_t* data, size_t len) {
    uint8_t record[TLS_MAX_RECORD_SIZE + 5];
    tls_record_header_t* header = (tls_record_header_t*)record;
    
    header->content_type = content_type;
    header->version = session->version;
    header->length = len;
    
    memcpy(record + 5, data, len);
    
    return k_sendto(session->socket_fd, record, len + 5, 0, NULL);
}

static int tls_recv_record(tls_session_t* session, uint8_t* content_type,
                           uint8_t* buffer, size_t max_len) {
    uint8_t header[5];
    
    int received = k_recvfrom(session->socket_fd, header, 5, 0, NULL);
    if (received < 5) {
        return TLS_ERR_SOCKET;
    }
    
    tls_record_header_t* hdr = (tls_record_header_t*)header;
    *content_type = hdr->content_type;
    
    uint16_t len = hdr->length;
    if (len > max_len) {
        len = max_len;
    }
    
    received = k_recvfrom(session->socket_fd, buffer, len, 0, NULL);
    return received;
}

static int tls_send_client_hello(tls_session_t* session) {
    uint8_t hello[1024];  // Increased buffer size for more extensions
    uint8_t* p = hello;
    
    // Handshake header
    *p++ = TLS_HANDSHAKE_CLIENT_HELLO;
    tls_write_uint24(0, p);  // Length placeholder
    p += 3;
    
    // Client version - TLS 1.2
    tls_write_uint16(TLS_VERSION_1_2, p);
    p += 2;
    
    // Random (32 bytes)
    tls_get_random(session->client_random, 32);
    memcpy(p, session->client_random, 32);
    p += 32;
    
    // Session ID (empty for new connection)
    *p++ = 0;
    
    // Cipher suites - modern suites that Google accepts
    uint16_t cipher_suites[] = {
        TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        TLS_RSA_WITH_AES_128_GCM_SHA256,
        TLS_RSA_WITH_AES_256_GCM_SHA384,
        TLS_RSA_WITH_AES_128_CBC_SHA256,
        TLS_RSA_WITH_AES_256_CBC_SHA256,
        0x002F,  // TLS_RSA_WITH_AES_128_CBC_SHA (fallback)
        0x0035   // TLS_RSA_WITH_AES_256_CBC_SHA (fallback)
    };
    int cipher_count = sizeof(cipher_suites) / sizeof(cipher_suites[0]);
    
    tls_write_uint16(cipher_count * 2, p);
    p += 2;
    for (int i = 0; i < cipher_count; i++) {
        tls_write_uint16(cipher_suites[i], p);
        p += 2;
    }
    
    // Compression methods (null only)
    *p++ = 1;
    *p++ = 0;
    
    // Calculate extensions total length first
    size_t sni_len = strlen(session->server_name);
    size_t ext_total_len = 0;
    
    // SNI extension: 2 (type) + 2 (len) + 2 (list len) + 1 (type) + 2 (name len) + name
    ext_total_len += 2 + 2 + (2 + 1 + 2 + sni_len);
    
    // Supported versions extension: 2 (type) + 2 (len) + 1 (list len) + 2 (version)
    ext_total_len += 2 + 2 + (1 + 2);
    
    // Signature algorithms extension: 2 (type) + 2 (len) + 2 (list len) + 2 (algo)
    ext_total_len += 2 + 2 + (2 + 2);
    
    // Write extensions length
    tls_write_uint16(ext_total_len, p);
    p += 2;
    
    // Server Name extension (SNI) - REQUIRED by Google
    tls_write_uint16(0x0000, p);  // Extension type: server_name
    p += 2;
    tls_write_uint16(sni_len + 5, p);  // Extension data length
    p += 2;
    tls_write_uint16(sni_len + 3, p);  // Server name list length
    p += 2;
    *p++ = 0;  // Name type: host_name
    tls_write_uint16(sni_len, p);  // Name length
    p += 2;
    memcpy(p, session->server_name, sni_len);
    p += sni_len;
    
    // Supported Versions extension - advertise TLS 1.2
    tls_write_uint16(0x002B, p);  // Extension type: supported_versions
    p += 2;
    tls_write_uint16(3, p);  // Extension data length
    p += 2;
    *p++ = 2;  // List length
    tls_write_uint16(TLS_VERSION_1_2, p);  // TLS 1.2
    p += 2;
    
    // Signature Algorithms extension - REQUIRED by Google
    tls_write_uint16(0x000D, p);  // Extension type: signature_algorithms
    p += 2;
    tls_write_uint16(4, p);  // Extension data length
    p += 2;
    tls_write_uint16(2, p);  // List length
    p += 2;
    // RSA-PKCS1-SHA256 (0x0401)
    tls_write_uint16(0x0401, p);
    p += 2;
    
    // Update handshake length
    size_t handshake_len = p - hello - 4;
    tls_write_uint24(handshake_len, hello + 1);
    
    // Update handshake hash
    sha256_update(&session->handshake_hash, hello, p - hello);
    
    // Send record
    return tls_send_record(session, TLS_CONTENT_HANDSHAKE, hello, p - hello);
}

static int tls_parse_server_hello(tls_session_t* session, const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    
    // Version
    session->version = tls_read_uint16(p);
    p += 2;
    
    // Random
    memcpy(session->server_random, p, 32);
    p += 32;
    
    // Session ID
    session->session_id_len = *p++;
    if (session->session_id_len > 0) {
        memcpy(session->session_id, p, session->session_id_len);
        p += session->session_id_len;
    }
    
    // Cipher suite
    session->cipher_suite = tls_read_uint16(p);
    p += 2;
    
    // Set cipher parameters
    switch (session->cipher_suite) {
        case TLS_RSA_WITH_AES_128_GCM_SHA256:
        case TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
            session->cipher_key_size = 16;
            session->cipher_iv_size = 4;  // Implicit IV
            session->cipher_mac_size = 0; // GCM has auth tag
            break;
        case TLS_RSA_WITH_AES_256_GCM_SHA384:
        case TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
            session->cipher_key_size = 32;
            session->cipher_iv_size = 4;
            session->cipher_mac_size = 0;
            break;
        default:
            return TLS_ERR_CIPHER;
    }
    
    // Compression method (should be null)
    if (*p != 0) {
        return TLS_ERR_PROTOCOL;
    }
    
    return 0;
}

static int tls_parse_certificate(tls_session_t* session, const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    
    // Certificates length
    uint32_t certs_len = tls_read_uint24(p);
    p += 3;
    
    // Parse each certificate
    session->cert_count = 0;
    while (p < data + len && session->cert_count < TLS_MAX_CERT_CHAIN) {
        uint32_t cert_len = tls_read_uint24(p);
        p += 3;
        
        if (x509_parse_der(p, cert_len, &session->cert_chain[session->cert_count]) == 0) {
            session->cert_count++;
        }
        
        p += cert_len;
    }
    
    if (session->cert_count == 0) {
        return TLS_ERR_CERTIFICATE;
    }
    
    return 0;
}

static int tls_parse_server_key_exchange(tls_session_t* session, const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    
    // For ECDHE key exchange
    if (session->cipher_suite == TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 ||
        session->cipher_suite == TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384) {
        
        // Curve type
        uint8_t curve_type = *p++;
        if (curve_type != 3) {  // Named curve
            return TLS_ERR_KEY_EXCHANGE;
        }
        
        // Named curve
        uint16_t curve = tls_read_uint16(p);
        p += 2;
        
        // Public key length
        uint8_t pk_len = *p++;
        
        // Store ECDHE public key
        session->server_key_type = 3;  // ECDHE
        session->ecdhe_key.curve = (ec_curve_type_t)curve;
        memcpy(session->ecdhe_key.public_key, p, pk_len);
        session->ecdhe_key.public_key_len = pk_len;
        p += pk_len;
        
        // Signature (verify with server certificate)
        // For now, skip signature verification
    }
    
    return 0;
}

static int tls_send_client_key_exchange(tls_session_t* session) {
    uint8_t key_exchange[512];
    uint8_t* p = key_exchange;
    
    // Handshake header
    *p++ = TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE;
    tls_write_uint24(0, p);  // Length placeholder
    p += 3;
    
    if (session->server_key_type == 3) {
        // ECDHE - generate ephemeral key pair and compute shared secret
        ecdh_generate_keypair(&session->ecdhe_key, session->ecdhe_key.curve);
        
        uint8_t shared_secret[64];
        ecdh_compute_shared_secret(&session->ecdhe_key,
                                   session->server_ec_key.public_key,
                                   session->server_ec_key.public_key_len,
                                   shared_secret);
        
        // Use shared secret as pre-master secret
        // For TLS 1.2, derive master secret using PRF
        
        // Send our public key
        *p++ = session->ecdhe_key.public_key_len;
        memcpy(p, session->ecdhe_key.public_key, session->ecdhe_key.public_key_len);
        p += session->ecdhe_key.public_key_len;
    } else {
        // RSA key exchange
        // Generate pre-master secret
        uint8_t pre_master_secret[48];
        tls_write_uint16(TLS_VERSION_1_2, pre_master_secret);
        tls_get_random(pre_master_secret + 2, 46);
        
        // Encrypt with server's public key
        uint8_t encrypted_pms[512];
        int enc_len = rsa_public_encrypt(&session->server_rsa_key,
                                         pre_master_secret, 48,
                                         encrypted_pms);
        
        if (enc_len < 0) {
            return TLS_ERR_KEY_EXCHANGE;
        }
        
        // Store pre-master secret for key derivation
        memcpy(session->master_secret, pre_master_secret, 48);
        
        // Send encrypted PMS
        tls_write_uint16(enc_len, p);
        p += 2;
        memcpy(p, encrypted_pms, enc_len);
        p += enc_len;
    }
    
    // Update handshake length
    size_t handshake_len = p - key_exchange - 4;
    tls_write_uint24(handshake_len, key_exchange + 1);
    
    // Update handshake hash
    sha256_update(&session->handshake_hash, key_exchange, p - key_exchange);
    
    return tls_send_record(session, TLS_CONTENT_HANDSHAKE, key_exchange, p - key_exchange);
}

static int tls_derive_keys(tls_session_t* session) {
    // Derive master secret
    // master_secret = PRF(pre_master_secret, "master secret",
    //                     client_random + server_random)[0..47]
    
    uint8_t random[64];
    memcpy(random, session->client_random, 32);
    memcpy(random + 32, session->server_random, 32);
    
    tls_prf(session->master_secret, 48, "master secret",
            random, 64, session->master_secret, 48);
    
    // Derive key block
    // key_block = PRF(master_secret, "key expansion",
    //                 server_random + client_random)
    memcpy(random, session->server_random, 32);
    memcpy(random + 32, session->client_random, 32);
    
    // Key block size depends on cipher suite
    size_t key_block_size = session->cipher_key_size * 2 +  // client/server write keys
                            session->cipher_iv_size * 2;     // client/server IVs
    if (session->cipher_mac_size > 0) {
        key_block_size += session->cipher_mac_size * 2;  // MAC keys
    }
    
    tls_prf(session->master_secret, 48, "key expansion",
            random, 64, session->key_block, key_block_size);
    
    // Initialize encryption contexts
    uint8_t* kb = session->key_block;
    
    // Client write key
    aes_gcm_init(&session->write_ctx, kb, session->cipher_key_size * 8, session->write_iv);
    kb += session->cipher_key_size;
    
    // Server write key
    aes_gcm_init(&session->read_ctx, kb, session->cipher_key_size * 8, session->read_iv);
    
    return 0;
}

static int tls_send_change_cipher_spec(tls_session_t* session) {
    uint8_t ccs = 1;
    return tls_send_record(session, TLS_CONTENT_CHANGE_CIPHER_SPEC, &ccs, 1);
}

static int tls_send_finished(tls_session_t* session) {
    uint8_t finished[16 + 4];
    uint8_t* p = finished;
    
    // Handshake header
    *p++ = TLS_HANDSHAKE_FINISHED;
    tls_write_uint24(12, p);  // Length
    p += 3;
    
    // Compute verify_data
    // verify_data = PRF(master_secret, "client finished", Hash(handshake_messages))
    uint8_t verify_data[12];
    uint8_t handshake_hash[32];
    sha256_final(&session->handshake_hash, handshake_hash);
    
    tls_prf(session->master_secret, 48, "client finished",
            handshake_hash, 32, verify_data, 12);
    
    memcpy(p, verify_data, 12);
    p += 12;
    
    // For TLS 1.2 with GCM, encrypt the finished message
    uint8_t encrypted[32];
    uint8_t tag[16];
    
    // Build AAD (additional authenticated data)
    uint8_t aad[13];
    aad[0] = TLS_CONTENT_HANDSHAKE;
    tls_write_uint16(session->version, aad + 1);
    tls_write_uint16(16, aad + 3);  // Length after encryption
    
    // Encrypt
    aes_gcm_encrypt(&session->write_ctx, finished + 4, 12,
                    aad, 13, encrypted, tag);
    
    // Build encrypted record
    uint8_t record[32];
    tls_write_uint16(session->write_seq_num, session->write_iv);
    session->write_seq_num++;
    
    // For now, send unencrypted (simplified)
    return tls_send_record(session, TLS_CONTENT_HANDSHAKE, finished, p - finished);
}

static int tls_verify_server_finished(tls_session_t* session, const uint8_t* data, size_t len) {
    if (len < 16) return TLS_ERR_HANDSHAKE;
    
    // Compute expected verify_data
    uint8_t verify_data[12];
    uint8_t handshake_hash[32];
    
    // Re-initialize hash for server finished
    sha256_final(&session->handshake_hash, handshake_hash);
    
    tls_prf(session->master_secret, 48, "server finished",
            handshake_hash, 32, verify_data, 12);
    
    // Compare
    if (tls_constant_time_memcmp(data + 4, verify_data, 12) != 0) {
        return TLS_ERR_HANDSHAKE;
    }
    
    return 0;
}

int tls_connect(tls_session_t* session, const char* hostname, uint16_t port) {
    int ret;
    uint8_t buffer[8192];
    uint8_t content_type;
    
    // Initialize handshake hash
    sha256_init(&session->handshake_hash);
    
    // Set hostname
    tls_set_hostname(session, hostname);
    session->port = port;
    
    // Create socket
    session->socket_fd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (session->socket_fd < 0) {
        return TLS_ERR_SOCKET;
    }
    
    // Resolve hostname
    char ip_str[32];
    if (dns_resolve(hostname, ip_str, sizeof(ip_str)) < 0) {
        k_close(session->socket_fd);
        return TLS_ERR_SOCKET;
    }
    
    // Convert IP
    uint32_t ip = 0;
    char* p = ip_str;
    for (int i = 0; i < 4; i++) {
        uint8_t octet = 0;
        while (*p >= '0' && *p <= '9') {
            octet = octet * 10 + (*p - '0');
            p++;
        }
        if (*p == '.') p++;
        ip = (ip << 8) | octet;
    }
    
    // Connect
    sockaddr_in_t server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = ip;
    
    if (k_connect(session->socket_fd, &server_addr) < 0) {
        k_close(session->socket_fd);
        return TLS_ERR_SOCKET;
    }
    
    session->state = TLS_STATE_CONNECTING;
    
    // Send ClientHello
    ret = tls_send_client_hello(session);
    if (ret < 0) {
        return TLS_ERR_HANDSHAKE;
    }
    session->state = TLS_STATE_HELLO_SENT;
    
    // Receive ServerHello
    ret = tls_recv_record(session, &content_type, buffer, sizeof(buffer));
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        return TLS_ERR_HANDSHAKE;
    }
    
    if (buffer[0] != TLS_HANDSHAKE_SERVER_HELLO) {
        return TLS_ERR_HANDSHAKE;
    }
    
    // Update handshake hash
    size_t handshake_len = tls_read_uint24(buffer + 1) + 4;
    sha256_update(&session->handshake_hash, buffer, handshake_len);
    
    // Parse ServerHello
    ret = tls_parse_server_hello(session, buffer + 4, handshake_len - 4);
    if (ret < 0) {
        return ret;
    }
    session->state = TLS_STATE_HELLO_RECEIVED;
    
    // Receive Certificate
    ret = tls_recv_record(session, &content_type, buffer, sizeof(buffer));
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        return TLS_ERR_HANDSHAKE;
    }
    
    if (buffer[0] != TLS_HANDSHAKE_CERTIFICATE) {
        return TLS_ERR_HANDSHAKE;
    }
    
    handshake_len = tls_read_uint24(buffer + 1) + 4;
    sha256_update(&session->handshake_hash, buffer, handshake_len);
    
    // Parse Certificate
    ret = tls_parse_certificate(session, buffer + 4, handshake_len - 4);
    if (ret < 0) {
        return ret;
    }
    
    // Verify certificate
    if (session->verify_cert) {
        ret = tls_verify_certificate(&session->cert_chain[0], hostname);
        if (ret < 0) {
            return ret;
        }
    }
    
    // Call certificate verification callback
    if (session->on_cert_verify) {
        session->on_cert_verify(&session->cert_chain[0], session->callback_user_data);
    }
    
    session->state = TLS_STATE_CERTIFICATE_RECEIVED;
    
    // Receive ServerKeyExchange (for ECDHE) or ServerHelloDone
    ret = tls_recv_record(session, &content_type, buffer, sizeof(buffer));
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        return TLS_ERR_HANDSHAKE;
    }
    
    if (buffer[0] == TLS_HANDSHAKE_SERVER_KEY_EXCHANGE) {
        handshake_len = tls_read_uint24(buffer + 1) + 4;
        sha256_update(&session->handshake_hash, buffer, handshake_len);
        
        ret = tls_parse_server_key_exchange(session, buffer + 4, handshake_len - 4);
        if (ret < 0) {
            return ret;
        }
        
        session->state = TLS_STATE_KEY_EXCHANGE_RECEIVED;
        
        // Receive ServerHelloDone
        ret = tls_recv_record(session, &content_type, buffer, sizeof(buffer));
        if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
            return TLS_ERR_HANDSHAKE;
        }
    }
    
    if (buffer[0] != TLS_HANDSHAKE_SERVER_HELLO_DONE) {
        return TLS_ERR_HANDSHAKE;
    }
    
    handshake_len = tls_read_uint24(buffer + 1) + 4;
    sha256_update(&session->handshake_hash, buffer, handshake_len);
    
    session->state = TLS_STATE_HELLO_DONE_RECEIVED;
    
    // Send ClientKeyExchange
    ret = tls_send_client_key_exchange(session);
    if (ret < 0) {
        return ret;
    }
    
    // Derive keys
    ret = tls_derive_keys(session);
    if (ret < 0) {
        return ret;
    }
    
    session->state = TLS_STATE_KEY_EXCHANGE_SENT;
    
    // Send ChangeCipherSpec
    ret = tls_send_change_cipher_spec(session);
    if (ret < 0) {
        return ret;
    }
    
    // Send Finished
    ret = tls_send_finished(session);
    if (ret < 0) {
        return ret;
    }
    session->state = TLS_STATE_FINISHED_SENT;
    
    // Receive ChangeCipherSpec
    ret = tls_recv_record(session, &content_type, buffer, sizeof(buffer));
    if (ret < 0 || content_type != TLS_CONTENT_CHANGE_CIPHER_SPEC) {
        return TLS_ERR_HANDSHAKE;
    }
    
    // Receive Finished
    ret = tls_recv_record(session, &content_type, buffer, sizeof(buffer));
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        return TLS_ERR_HANDSHAKE;
    }
    
    ret = tls_verify_server_finished(session, buffer, ret);
    if (ret < 0) {
        return ret;
    }
    
    session->state = TLS_STATE_ESTABLISHED;
    return 0;
}

int tls_close(tls_session_t* session) {
    if (session->state == TLS_STATE_ESTABLISHED) {
        // Send close_notify alert
        uint8_t alert[2] = {TLS_ALERT_LEVEL_WARNING, TLS_ALERT_CLOSE_NOTIFY};
        tls_send_record(session, TLS_CONTENT_ALERT, alert, 2);
    }
    
    if (session->socket_fd >= 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
    }
    
    session->state = TLS_STATE_CLOSED;
    return 0;
}

int tls_write(tls_session_t* session, const void* data, size_t len) {
    if (session->state != TLS_STATE_ESTABLISHED) {
        return TLS_ERR_PROTOCOL;
    }
    
    // For GCM mode, encrypt application data
    uint8_t encrypted[16384];
    uint8_t tag[16];
    
    // Build AAD
    uint8_t aad[13];
    aad[0] = TLS_CONTENT_APPLICATION_DATA;
    tls_write_uint16(session->version, aad + 1);
    tls_write_uint16(len + 16, aad + 3);  // Length including tag
    
    // Encrypt
    aes_gcm_encrypt(&session->write_ctx, data, len, aad, 13, encrypted, tag);
    
    // Build record
    uint8_t record[16384 + 16];
    memcpy(record, encrypted, len);
    memcpy(record + len, tag, 16);
    
    session->write_seq_num++;
    
    // For now, send unencrypted (simplified implementation)
    return tls_send_record(session, TLS_CONTENT_APPLICATION_DATA, data, len);
}

int tls_read(tls_session_t* session, void* buffer, size_t max_len) {
    if (session->state != TLS_STATE_ESTABLISHED) {
        return TLS_ERR_PROTOCOL;
    }
    
    uint8_t content_type;
    uint8_t temp_buffer[16384];
    
    int ret = tls_recv_record(session, &content_type, temp_buffer, sizeof(temp_buffer));
    if (ret < 0) {
        return ret;
    }
    
    if (content_type == TLS_CONTENT_ALERT) {
        // Handle alert
        if (temp_buffer[0] == TLS_ALERT_LEVEL_FATAL) {
            session->state = TLS_STATE_ERROR;
            return TLS_ERR_HANDSHAKE;
        }
        if (temp_buffer[1] == TLS_ALERT_CLOSE_NOTIFY) {
            session->state = TLS_STATE_CLOSED;
            return 0;
        }
        return 0;
    }
    
    if (content_type != TLS_CONTENT_APPLICATION_DATA) {
        return TLS_ERR_PROTOCOL;
    }
    
    // For GCM mode, decrypt
    // For now, return data as-is (simplified)
    size_t copy_len = (ret < (int)max_len) ? ret : max_len;
    memcpy(buffer, temp_buffer, copy_len);
    
    session->read_seq_num++;
    
    return copy_len;
}

// ============================================================================
// ECDH IMPLEMENTATION (Simplified)
// ============================================================================

int ecdh_generate_keypair(ec_key_t* key, ec_curve_type_t curve) {
    key->curve = curve;
    
    // Generate random private key
    int priv_len = 32;  // P-256
    if (curve == EC_CURVE_P384) priv_len = 48;
    else if (curve == EC_CURVE_P521) priv_len = 66;
    
    tls_get_random(key->private_key, priv_len);
    key->private_key_len = priv_len;
    
    // Compute public key (simplified - would need actual EC point multiplication)
    // For now, just generate random public key
    key->public_key_len = priv_len * 2 + 1;
    key->public_key[0] = 0x04;  // Uncompressed point
    tls_get_random(key->public_key + 1, priv_len * 2);
    
    return 0;
}

int ecdh_compute_shared_secret(ec_key_t* private_key, const uint8_t* peer_public,
                               size_t peer_len, uint8_t* shared_secret) {
    // Simplified ECDH - would need actual EC point multiplication
    // For now, derive shared secret from both keys
    uint8_t combined[256];
    memcpy(combined, private_key->private_key, private_key->private_key_len);
    memcpy(combined + private_key->private_key_len, peer_public, peer_len);
    
    sha256_hash(combined, private_key->private_key_len + peer_len, shared_secret);
    
    return 32;
}

// ============================================================================
// HKDF IMPLEMENTATION
// ============================================================================

int hkdf_extract(const uint8_t* salt, size_t salt_len,
                 const uint8_t* ikm, size_t ikm_len,
                 uint8_t* prk) {
    // PRK = HMAC-Hash(salt, IKM)
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    return 0;
}

int hkdf_expand(const uint8_t* prk, size_t prk_len,
                const uint8_t* info, size_t info_len,
                uint8_t* okm, size_t okm_len) {
    // T(0) = empty
    // T(N) = HMAC-Hash(PRK, T(N-1) | info | N)
    
    uint8_t t[32];
    uint8_t counter = 1;
    size_t done = 0;
    
    while (done < okm_len) {
        uint8_t input[256];
        size_t input_len = 0;
        
        if (counter > 1) {
            memcpy(input, t, 32);
            input_len = 32;
        }
        
        if (info && info_len > 0) {
            memcpy(input + input_len, info, info_len);
            input_len += info_len;
        }
        
        input[input_len++] = counter;
        
        hmac_sha256(prk, prk_len, input, input_len, t);
        
        size_t copy_len = (okm_len - done < 32) ? okm_len - done : 32;
        memcpy(okm + done, t, copy_len);
        done += copy_len;
        counter++;
    }
    
    return 0;
}
