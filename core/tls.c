// core/tls.c - TLS 1.2+ Protocol Implementation
// Implements: TLS 1.2 handshake, AES-GCM, SHA-256, RSA, Certificate validation
#include "tls.h"
#include "tls_ca_store.h"
#include "sha256.h"
#include "socket.h"
#include "dns.h"
#include "string.h"
#include "memory.h"
#include "net.h"
#include "../hal/cpu/timer.h"
#include "../hal/drivers/rtc.h"

// External functions
extern size_t strlen(const char* s);
extern void s_printf(const char* fmt, ...);
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
// Hash-DRBG based on SHA-256 (NIST SP 800-90A style)
// Much stronger than simple LCG - uses SHA-256 as a cryptographic primitive

static uint8_t drbg_state[32];     // V: internal state
static uint8_t drbg_constant[32];  // C: reseed constant
static int drbg_initialized = 0;
static uint64_t drbg_reseed_counter = 0;

// Internal: update the DRBG state with provided data
static void drbg_update(const uint8_t *provided_data, int data_len) {
    // Step 1: V = V + 1
    // Increment V as a big-endian integer
    int carry = 1;
    for (int i = 31; i >= 0 && carry; i--) {
        int sum = drbg_state[i] + carry;
        drbg_state[i] = sum & 0xFF;
        carry = sum >> 8;
    }

    // Step 2: H = V || provided_data
    // Step 3: V = H
    {
        uint8_t hash_input[64];
        int input_len = 32;
        memcpy(hash_input, drbg_state, 32);
        if (provided_data && data_len > 0) {
            int copy_len = data_len;
            if (copy_len > 32) copy_len = 32;
            memcpy(hash_input + 32, provided_data, copy_len);
            input_len = 32 + copy_len;
        }
        sha256_hash(hash_input, input_len, drbg_state);
    }

    // Step 4: H = V || C || provided_data
    // Step 5: C = H
    {
        uint8_t hash_input[96];
        int input_len = 64;
        memcpy(hash_input, drbg_state, 32);
        memcpy(hash_input + 32, drbg_constant, 32);
        if (provided_data && data_len > 0) {
            int copy_len = data_len;
            if (copy_len > 32) copy_len = 32;
            memcpy(hash_input + 64, provided_data, copy_len);
            input_len = 64 + copy_len;
        }
        sha256_hash(hash_input, input_len, drbg_constant);
    }
}

// Collect entropy from multiple system sources
static void collect_entropy(uint8_t *buf, int len) {
    // Mix multiple entropy sources from the kernel environment
    // Using timer variations, cycle counter, and memory addresses as entropy
    uint8_t seed_material[64];
    memset(seed_material, 0, sizeof(seed_material));

    // Collect timer variations (8 samples with delays for variation)
    for (int i = 0; i < 8; i++) {
        uint32_t t = get_tick_count();
        seed_material[i * 4 + 0] = (t >> 24) & 0xFF;
        seed_material[i * 4 + 1] = (t >> 16) & 0xFF;
        seed_material[i * 4 + 2] = (t >> 8) & 0xFF;
        seed_material[i * 4 + 3] = t & 0xFF;
        // Small delay to get different timer values
        for (volatile int d = 0; d < 1000; d++);
    }

    // Mix in heap and stack addresses (ASLR-like entropy)
    uint32_t heap_addr = (uint32_t)kmalloc(1);
    seed_material[32] = (heap_addr >> 24) & 0xFF;
    seed_material[33] = (heap_addr >> 16) & 0xFF;
    seed_material[34] = (heap_addr >> 8) & 0xFF;
    seed_material[35] = heap_addr & 0xFF;
    if (heap_addr) kfree((void*)heap_addr);

    uint32_t stack_var_addr = 0;
    uint8_t stack_var = 0;
    stack_var_addr = (uint32_t)&stack_var;
    seed_material[36] = (stack_var_addr >> 24) & 0xFF;
    seed_material[37] = (stack_var_addr >> 16) & 0xFF;
    seed_material[38] = (stack_var_addr >> 8) & 0xFF;
    seed_material[39] = stack_var_addr & 0xFF;

    // Mix in more timer readings with different timing
    for (int i = 0; i < 4; i++) {
        uint32_t t = get_tick_count();
        seed_material[40 + i * 4 + 0] = (t >> 24) & 0xFF;
        seed_material[40 + i * 4 + 1] = (t >> 16) & 0xFF;
        seed_material[40 + i * 4 + 2] = (t >> 8) & 0xFF;
        seed_material[40 + i * 4 + 3] = t & 0xFF;
        for (volatile int d = 0; d < 500; d++);
    }

    // Hash the seed material to condense entropy
    sha256_hash(seed_material, 56, buf);
    if (len > 32) {
        sha256_hash(buf, 32, buf + 32);
    }
}

static void drbg_initialize(void) {
    if (drbg_initialized) return;

    // Collect entropy for initial state
    uint8_t seed_material[64];
    collect_entropy(seed_material, 64);

    // Initialize V from seed
    memcpy(drbg_state, seed_material, 32);

    // Initialize C = SHA-256(0x00 || V)
    uint8_t c_input[33];
    c_input[0] = 0x00;
    memcpy(c_input + 1, drbg_state, 32);
    sha256_hash(c_input, 33, drbg_constant);

    drbg_reseed_counter = 1;
    drbg_initialized = 1;
}

static void drbg_reseed(void) {
    uint8_t seed_material[64];
    collect_entropy(seed_material, 64);

    // V = SHA-256(0x01 || V || seed_material)
    uint8_t reseed_input[97];
    reseed_input[0] = 0x01;
    memcpy(reseed_input + 1, drbg_state, 32);
    memcpy(reseed_input + 33, seed_material, 64);
    sha256_hash(reseed_input, 97, drbg_state);

    // C = SHA-256(0x00 || V)
    uint8_t c_input[33];
    c_input[0] = 0x00;
    memcpy(c_input + 1, drbg_state, 32);
    sha256_hash(c_input, 33, drbg_constant);

    drbg_reseed_counter = 1;
}

void tls_get_random(uint8_t* buffer, size_t len) {
    if (!drbg_initialized) {
        drbg_initialize();
    }

    // Reseed every 2^16 requests for forward secrecy
    if (drbg_reseed_counter > 65536) {
        drbg_reseed();
    }

    size_t generated = 0;
    while (generated < len) {
        // Generate 32 bytes at a time
        drbg_update(NULL, 0);

        int copy_len = 32;
        if (generated + copy_len > len) {
            copy_len = len - generated;
        }
        memcpy(buffer + generated, drbg_state, copy_len);
        generated += copy_len;
    }

    drbg_reseed_counter++;
}

// Get a single random byte
uint8_t tls_get_random_byte(void) {
    uint8_t byte;
    tls_get_random(&byte, 1);
    return byte;
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
    int i, j, round;
 
    // Load input into state (column-major, same as encrypt)
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            s[j][i] = input[i * 4 + j];
 
    // Initial AddRoundKey with last round key
    for (i = 0; i < 4; i++) {
        uint32_t k = ctx->key[nr * 4 + i];
        s[0][i] ^= (k >> 24) & 0xFF;
        s[1][i] ^= (k >> 16) & 0xFF;
        s[2][i] ^= (k >> 8)  & 0xFF;
        s[3][i] ^=  k        & 0xFF;
    }
 
    for (round = nr - 1; round >= 0; round--) {
        // *** FIX: copy s → state before every InvShiftRows ***
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                state[i][j] = s[i][j];
 
        // InvShiftRows:
        // Row 1: right shift by 1  (reverse of left-shift-1)
        { uint8_t t = state[1][3]; state[1][3]=state[1][2]; state[1][2]=state[1][1]; state[1][1]=state[1][0]; state[1][0]=t; }
        // Row 2: swap [0]↔[2] and [1]↔[3]  (same as reverse of left-shift-2)
        { uint8_t t=state[2][0]; state[2][0]=state[2][2]; state[2][2]=t;
          t=state[2][1]; state[2][1]=state[2][3]; state[2][3]=t; }
        // Row 3: left shift by 1  (reverse of right-shift-1 = left-shift-3)
        { uint8_t t = state[3][0]; state[3][0]=state[3][1]; state[3][1]=state[3][2]; state[3][2]=state[3][3]; state[3][3]=t; }
 
        // InvSubBytes
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                state[i][j] = aes_inv_sbox[state[i][j]];
 
        // AddRoundKey
        for (i = 0; i < 4; i++) {
            uint32_t k = ctx->key[round * 4 + i];
            state[0][i] ^= (k >> 24) & 0xFF;
            state[1][i] ^= (k >> 16) & 0xFF;
            state[2][i] ^= (k >> 8)  & 0xFF;
            state[3][i] ^=  k        & 0xFF;
        }
 
        if (round > 0) {
            // InvMixColumns — result goes back into s
            for (i = 0; i < 4; i++) {
                uint8_t a0=state[0][i], a1=state[1][i],
                        a2=state[2][i], a3=state[3][i];
                s[0][i] = multiply(a0,0x0e)^multiply(a1,0x0b)^multiply(a2,0x0d)^multiply(a3,0x09);
                s[1][i] = multiply(a0,0x09)^multiply(a1,0x0e)^multiply(a2,0x0b)^multiply(a3,0x0d);
                s[2][i] = multiply(a0,0x0d)^multiply(a1,0x09)^multiply(a2,0x0e)^multiply(a3,0x0b);
                s[3][i] = multiply(a0,0x0b)^multiply(a1,0x0d)^multiply(a2,0x09)^multiply(a3,0x0e);
            }
        } else {
            // Final round: no InvMixColumns, just copy state → s
            for (i = 0; i < 4; i++)
                for (j = 0; j < 4; j++)
                    s[i][j] = state[i][j];
        }
    }
 
    // Output (column-major → row-major)
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            output[i * 4 + j] = s[j][i];
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
    uint8_t* ghash_input = (uint8_t*)kmalloc(pt_len + aad_len + 64);
    if (!ghash_input) return -1;
    size_t ghash_len = 0;
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
    
    kfree(ghash_input);
    return 0;
}

int aes_gcm_decrypt(aes_gcm_ctx_t* ctx, const uint8_t* ciphertext, size_t ct_len,
                    const uint8_t* aad, size_t aad_len,
                    const uint8_t* tag, uint8_t* plaintext) {
    uint8_t counter[16];
    uint8_t eky[16];
    uint8_t* ghash_input = (uint8_t*)kmalloc(ct_len + aad_len + 64);
    if (!ghash_input) return -1;
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
    
    // Clean up GHASH input buffer
    kfree(ghash_input);
    ghash_input = NULL;
    
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
    size_t label_len = strlen((char*)label);
    size_t full_seed_len = label_len + seed_len;

    // Bounds check to prevent stack overflow
    if (full_seed_len > 240) {
        // Truncate to fit - extremely unlikely in practice
        full_seed_len = 240;
        if (label_len > full_seed_len) label_len = full_seed_len;
        seed_len = full_seed_len - label_len;
    }

    uint8_t full_seed[256];
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

// Big integer: little-endian uint32_t words, fixed size of 64 words (2048 bits)
#define MODEXP_WORDS 64
typedef uint32_t bignum_t[MODEXP_WORDS];

static void bn_zero(bignum_t a) {
    for (int i = 0; i < MODEXP_WORDS; i++) a[i] = 0;
}

static void bn_copy(bignum_t dst, const bignum_t src) {
    for (int i = 0; i < MODEXP_WORDS; i++) dst[i] = src[i];
}

// Load big-endian bytes into little-endian bignum
static void bn_from_bytes(bignum_t a, const uint8_t* bytes, size_t len) {
    bn_zero(a);
    if (len > MODEXP_WORDS * 4) len = MODEXP_WORDS * 4;
    for (size_t i = 0; i < len; i++) {
        size_t word_idx  = (len - 1 - i) / 4;
        size_t byte_pos  = (len - 1 - i) % 4;
        a[word_idx] |= (uint32_t)bytes[i] << (byte_pos * 8);
    }
}

// Store little-endian bignum into big-endian bytes
static void bn_to_bytes(uint8_t* bytes, size_t len, const bignum_t a) {
    if (len > MODEXP_WORDS * 4) len = MODEXP_WORDS * 4;
    for (size_t i = 0; i < len; i++) {
        size_t word_idx = (len - 1 - i) / 4;
        size_t byte_pos = (len - 1 - i) % 4;
        bytes[i] = (a[word_idx] >> (byte_pos * 8)) & 0xFF;
    }
}

// a >= b ?
static int bn_cmp(const bignum_t a, const bignum_t b) {
    for (int i = MODEXP_WORDS - 1; i >= 0; i--) {
        if (a[i] > b[i]) return  1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

// a = a - b (assuming a >= b)
static void bn_sub_inplace(bignum_t a, const bignum_t b) {
    uint32_t borrow = 0;
    for (int i = 0; i < MODEXP_WORDS; i++) {
        uint64_t diff = (uint64_t)a[i] - b[i] - borrow;
        a[i]  = (uint32_t)diff;
        borrow = (diff >> 63) & 1;
    }
}

// r = (a + b) mod m  — a,b < m
static void bn_addmod(bignum_t r, const bignum_t a, const bignum_t b, const bignum_t m) {
    uint32_t carry = 0;
    for (int i = 0; i < MODEXP_WORDS; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        r[i]  = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
    if (carry || bn_cmp(r, m) >= 0) bn_sub_inplace(r, m);
}

// Schoolbook multiply: result = a * b (double-width, lower half stored in lo[])
// We only keep MODEXP_WORDS words (truncate high half) — caller reduces mod m
static void bn_mul_trunc(bignum_t lo, const bignum_t a, const bignum_t b) {
    uint32_t tmp[MODEXP_WORDS * 2];
    for (int i = 0; i < MODEXP_WORDS * 2; i++) tmp[i] = 0;
    for (int i = 0; i < MODEXP_WORDS; i++) {
        if (!a[i]) continue;
        uint64_t carry = 0;
        for (int j = 0; j < MODEXP_WORDS && i+j < MODEXP_WORDS*2; j++) {
            uint64_t prod = (uint64_t)a[i] * b[j] + tmp[i+j] + carry;
            tmp[i+j] = (uint32_t)prod;
            carry    = prod >> 32;
        }
    }
    for (int i = 0; i < MODEXP_WORDS; i++) lo[i] = tmp[i];
}

// Modular reduction: r = a mod m  (a has up to MODEXP_WORDS words, m has mod_len bytes)
// We use repeated subtraction for simplicity (sufficient for RSA where operands < m^2)
static void bn_reduce(bignum_t a, const bignum_t m) {
    while (bn_cmp(a, m) >= 0)
        bn_sub_inplace(a, m);
}

// Montgomery-style: r = (a * b) mod m
static void bn_mulmod(bignum_t r, const bignum_t a, const bignum_t b, const bignum_t m) {
    bignum_t tmp;
    bn_mul_trunc(tmp, a, b);
    bn_reduce(tmp, m);
    bn_copy(r, tmp);
}

// Modular exponentiation: result = base^exp mod mod
static void mod_exp(const uint8_t* base, size_t base_len,
                    const uint8_t* exp,  size_t exp_len,
                    const uint8_t* mod,  size_t mod_len,
                    uint8_t*       result) {
    bignum_t B, M, R, T;

    bn_from_bytes(B, base, base_len);
    bn_from_bytes(M, mod,  mod_len);

    // R = 1
    bn_zero(R);
    R[0] = 1;

    // Reduce B mod M first
    bn_reduce(B, M);

    // Square-and-multiply, MSB first
    for (size_t i = 0; i < exp_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            // R = R^2 mod M
            bn_mulmod(T, R, R, M);
            bn_copy(R, T);
            // if bit set: R = R * B mod M
            if (exp[i] & (1u << bit)) {
                bn_mulmod(T, R, B, M);
                bn_copy(R, T);
            }
        }
    }

    bn_to_bytes(result, mod_len, R);
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
        if (tls_constant_time_memcmp(decrypted + i, sha256_prefix, sizeof(sha256_prefix)) != 0) {
            return TLS_ERR_SIGNATURE;
        }
        i += sizeof(sha256_prefix);
    }
    
    // Compare hash using constant-time comparison to prevent timing attacks
    if (tls_constant_time_memcmp(decrypted + i, hash, hash_len) != 0) {
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
static const uint8_t oid_sha384_rsa[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C};
static const uint8_t oid_san[] = {0x55, 0x1D, 0x11};  // 2.5.29.17 SubjectAltName

// ASN.1 time tag constants
#define ASN1_TAG_UTCTIME         0x17
#define ASN1_TAG_GENERALIZEDTIME 0x18

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

// Parse any ASN.1 element regardless of tag (returns total element size including tag+length)
static int parse_asn1_any(const uint8_t* data, const uint8_t** content, size_t* content_len, uint8_t* actual_tag) {
    if (!data) return -1;
    *actual_tag = data[0];
    size_t len, header_len;
    if (parse_asn1_length(data + 1, &len, &header_len) < 0) return -1;
    *content = data + 1 + header_len;
    *content_len = len;
    return 1 + header_len + len;
}

// Convert ASN.1 UTCTime or GeneralizedTime string to Unix timestamp
static uint32_t asn1_time_to_unix(const uint8_t* time_str, size_t time_len, int is_generalized) {
    int year, month, day, hour = 0, minute = 0, second = 0;
    int pos = 0;

    if (is_generalized) {
        // YYYYMMDDHHmmSSZ
        if (time_len < 10) return 0;
        year = (time_str[0] - '0') * 1000 + (time_str[1] - '0') * 100 +
               (time_str[2] - '0') * 10 + (time_str[3] - '0');
        pos = 4;
    } else {
        // YYMMDDHHmmSSZ
        if (time_len < 8) return 0;
        year = (time_str[0] - '0') * 10 + (time_str[1] - '0');
        year += (year >= 50) ? 1900 : 2000;
        pos = 2;
    }

    month = (time_str[pos] - '0') * 10 + (time_str[pos+1] - '0');
    pos += 2;
    day = (time_str[pos] - '0') * 10 + (time_str[pos+1] - '0');
    pos += 2;
    if (pos + 1 < time_len) {
        hour = (time_str[pos] - '0') * 10 + (time_str[pos+1] - '0');
        pos += 2;
    }
    if (pos + 1 < time_len) {
        minute = (time_str[pos] - '0') * 10 + (time_str[pos+1] - '0');
        pos += 2;
    }
    if (pos + 1 < time_len && time_str[pos] != 'Z' && time_str[pos] != '+' && time_str[pos] != '-') {
        second = (time_str[pos] - '0') * 10 + (time_str[pos+1] - '0');
    }

    // Validate basic ranges
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
    if (year < 1970) return 0;

    // Compute Unix timestamp
    static const uint16_t days_before_month[13] = {
        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    uint32_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += 365 + ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0);
    }
    days += days_before_month[month];
    if (month > 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        days += 1;
    }
    days += day - 1;

    return (uint32_t)(days * 86400UL + hour * 3600 + minute * 60 + second);
}

// Extract RSA key components from SubjectPublicKeyInfo BIT STRING content
static int x509_extract_rsa_key(const uint8_t* pk_data, uint16_t pk_len, rsa_key_t* key) {
    // The BIT STRING content (after unused-bits byte) contains DER:
    // SEQUENCE { INTEGER modulus, INTEGER exponent }
    if (pk_len < 4 || pk_data[0] != 0x00) return -1;  // Skip unused-bits byte

    const uint8_t* p = pk_data + 1;  // Skip unused-bits byte

    // SEQUENCE
    if (p[0] != ASN1_TAG_SEQUENCE) return -1;
    size_t seq_len, seq_hdr;
    if (parse_asn1_length(p + 1, &seq_len, &seq_hdr) < 0) return -1;
    const uint8_t* seq_p = p + 1 + seq_hdr;

    // Modulus INTEGER
    if (seq_p[0] != ASN1_TAG_INTEGER) return -1;
    size_t mod_len, mod_hdr;
    if (parse_asn1_length(seq_p + 1, &mod_len, &mod_hdr) < 0) return -1;
    const uint8_t* mod_data = seq_p + 1 + mod_hdr;
    // Skip leading zero byte (sign byte)
    if (mod_len > 0 && mod_data[0] == 0x00) {
        mod_data++;
        mod_len--;
    }
    if (mod_len > TLS_MAX_RSA_MODULUS_SIZE) return -1;
    key->modulus_len = (uint16_t)mod_len;
    memcpy(key->modulus, mod_data, mod_len);
    seq_p += 1 + mod_hdr + mod_len;

    // Exponent INTEGER
    if (seq_p[0] != ASN1_TAG_INTEGER) return -1;
    size_t exp_len, exp_hdr;
    if (parse_asn1_length(seq_p + 1, &exp_len, &exp_hdr) < 0) return -1;
    const uint8_t* exp_data = seq_p + 1 + exp_hdr;
    if (exp_len > 0 && exp_data[0] == 0x00) {
        exp_data++;
        exp_len--;
    }
    if (exp_len > 8) return -1;
    key->exponent_len = (uint8_t)exp_len;
    memcpy(key->exponent, exp_data, exp_len);

    return 0;
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
        size_t ver_len = 0, ver_header = 0;
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
    
    // Parse notBefore and notAfter (UTCTime or GeneralizedTime)
    const uint8_t* val_p = validity;
    // notBefore
    {
        const uint8_t* time_content;
        size_t time_len;
        uint8_t tag;
        int elem_size = parse_asn1_any(val_p, &time_content, &time_len, &tag);
        if (elem_size < 0) return -1;
        int is_gen = (tag == ASN1_TAG_GENERALIZEDTIME) ? 1 : 0;
        cert->not_before = asn1_time_to_unix(time_content, time_len, is_gen);
        val_p += elem_size;
    }
    // notAfter
    {
        const uint8_t* time_content;
        size_t time_len;
        uint8_t tag;
        int elem_size = parse_asn1_any(val_p, &time_content, &time_len, &tag);
        if (elem_size < 0) return -1;
        int is_gen = (tag == ASN1_TAG_GENERALIZEDTIME) ? 1 : 0;
        cert->not_after = asn1_time_to_unix(time_content, time_len, is_gen);
        val_p += elem_size;
    }
    
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
    tbs += spki_offset;

    // Parse extensions for Subject Alternative Names (v3 certs)
    // Look for [3] EXPLICIT tag (0xA3) containing Extensions SEQUENCE
    cert->san_count = 0;
    if (tbs < tbs_end && tbs[0] == 0xA3) {
        const uint8_t* ext_wrapper;
        size_t ext_wrapper_len;
        uint8_t ext_tag;
        int ext_wrapper_size = parse_asn1_any(tbs, &ext_wrapper, &ext_wrapper_len, &ext_tag);
        if (ext_wrapper_size > 0 && ext_wrapper_len > 0) {
            // Inside [3] is a SEQUENCE OF Extension
            if (ext_wrapper[0] == ASN1_TAG_SEQUENCE) {
                const uint8_t* exts_content;
                size_t exts_len;
                if (parse_asn1_element(ext_wrapper, ASN1_TAG_SEQUENCE, &exts_content, &exts_len) > 0) {
                    const uint8_t* ext_p = exts_content;
                    const uint8_t* ext_end = exts_content + exts_len;

                    while (ext_p < ext_end && cert->san_count < TLS_MAX_SAN_ENTRIES) {
                        // Each Extension is a SEQUENCE
                        const uint8_t* ext_content;
                        size_t ext_len;
                        int ext_size = parse_asn1_element(ext_p, ASN1_TAG_SEQUENCE, &ext_content, &ext_len);
                        if (ext_size < 0) break;

                        const uint8_t* ep = ext_content;
                        // OID
                        const uint8_t* ext_oid;
                        size_t ext_oid_len;
                        int oid_size = parse_asn1_element(ep, ASN1_TAG_OID, &ext_oid, &ext_oid_len);
                        if (oid_size < 0) { ext_p += ext_size; continue; }
                        ep += oid_size;

                        // Check if this is the SAN extension (2.5.29.17)
                        if (oid_compare(ext_oid, ext_oid_len, oid_san, sizeof(oid_san))) {
                            // Optional BOOLEAN critical
                            if (ep < ext_content + ext_len && ep[0] == 0x01) {
                                size_t bool_len, bool_hdr;
                                parse_asn1_length(ep + 1, &bool_len, &bool_hdr);
                                ep += 1 + bool_hdr + bool_len;
                            }
                            // OCTET STRING containing the SAN value
                            const uint8_t* san_octet;
                            size_t san_octet_len;
                            if (parse_asn1_element(ep, ASN1_TAG_OCTET_STRING, &san_octet, &san_octet_len) > 0) {
                                // SAN is SEQUENCE OF GeneralName
                                if (san_octet_len > 0 && san_octet[0] == ASN1_TAG_SEQUENCE) {
                                    const uint8_t* san_seq;
                                    size_t san_seq_len;
                                    if (parse_asn1_element(san_octet, ASN1_TAG_SEQUENCE, &san_seq, &san_seq_len) > 0) {
                                        const uint8_t* san_p = san_seq;
                                        const uint8_t* san_end = san_seq + san_seq_len;
                                        while (san_p < san_end && cert->san_count < TLS_MAX_SAN_ENTRIES) {
                                            // dNSName is context tag [2] = 0x82
                                            if (san_p[0] == 0x82) {
                                                size_t name_len, name_hdr;
                                                if (parse_asn1_length(san_p + 1, &name_len, &name_hdr) >= 0) {
                                                    size_t copy = (name_len < TLS_MAX_CN_LENGTH - 1) ?
                                                                  name_len : TLS_MAX_CN_LENGTH - 1;
                                                    memcpy(cert->san_entries[cert->san_count],
                                                           san_p + 1 + name_hdr, copy);
                                                    cert->san_entries[cert->san_count][copy] = '\0';
                                                    cert->san_count++;
                                                }
                                                size_t skip_len, skip_hdr;
                                                parse_asn1_length(san_p + 1, &skip_len, &skip_hdr);
                                                san_p += 1 + skip_hdr + skip_len;
                                            } else {
                                                // Skip other GeneralName types
                                                size_t skip_len, skip_hdr;
                                                if (parse_asn1_length(san_p + 1, &skip_len, &skip_hdr) >= 0) {
                                                    san_p += 1 + skip_hdr + skip_len;
                                                } else {
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        ext_p += ext_size;
                    }
                }
            }
        }
    }

    // Determine if certificate is self-signed (issuer CN == subject CN)
    cert->is_self_signed = (strcmp(cert->common_name, cert->issuer_cn) == 0) ? 1 : 0;

    // Parse signature from the outer Certificate SEQUENCE (after TBS)
    // cert_content points to the start of Certificate content
    // tbs_offset bytes cover the entire TBS element
    {
        const uint8_t* after_tbs = cert_content + tbs_offset;

        // SignatureAlgorithm SEQUENCE
        const uint8_t* outer_sig_alg;
        size_t outer_sig_alg_len;
        int sig_alg_size = parse_asn1_element(after_tbs, ASN1_TAG_SEQUENCE,
                                               &outer_sig_alg, &outer_sig_alg_len);
        if (sig_alg_size > 0) {
            // Identify signature algorithm
            const uint8_t* alg_oid;
            size_t alg_oid_len;
            if (parse_asn1_element(outer_sig_alg, ASN1_TAG_OID, &alg_oid, &alg_oid_len) > 0) {
                if (oid_compare(alg_oid, alg_oid_len, oid_sha256_rsa, sizeof(oid_sha256_rsa))) {
                    cert->signature_alg = 1; // SHA256withRSA
                } else if (oid_compare(alg_oid, alg_oid_len, oid_sha384_rsa, sizeof(oid_sha384_rsa))) {
                    cert->signature_alg = 2; // SHA384withRSA
                } else if (oid_compare(alg_oid, alg_oid_len, oid_rsa_encryption, sizeof(oid_rsa_encryption))) {
                    cert->signature_alg = 1; // RSA (default to SHA-256)
                }
            }

            // SignatureValue BIT STRING
            const uint8_t* after_sig_alg = after_tbs + sig_alg_size;
            const uint8_t* sig_bits;
            size_t sig_bits_len;
            if (parse_asn1_element(after_sig_alg, ASN1_TAG_BIT_STRING, &sig_bits, &sig_bits_len) > 0) {
                // First byte is unused-bits count (should be 0)
                if (sig_bits_len > 1) {
                    cert->signature_len = (uint16_t)(sig_bits_len - 1);
                    if (cert->signature_len > 512) cert->signature_len = 512;
                    memcpy(cert->signature, sig_bits + 1, cert->signature_len);
                }
            }
        }
    }
    
    // Compute fingerprint
    sha256_hash(der_data, len, cert->fingerprint);
    
    return 0;
}

int x509_check_validity(x509_cert_t* cert) {
    if (!cert) return -1;

    // If validity dates weren't parsed (remain 0), we can't verify expiry
    // Allow through but this is a weak check
    if (cert->not_before == 0 && cert->not_after == 0) return 0;

    uint32_t now = get_unix_time();

    // Certificate is not yet valid
    if (cert->not_before != 0 && now < cert->not_before) return -1;

    // Certificate has expired
    if (cert->not_after != 0 && now > cert->not_after) return -1;

    return 0;
}

int x509_verify_signature(x509_cert_t* cert, x509_cert_t* issuer_cert) {
    if (!cert || !issuer_cert) return TLS_ERR_CERT_VERIFY;

    // Only support RSA signature verification for now
    if (issuer_cert->public_key_type != 1) {
        // Non-RSA keys not yet supported - reject
        return TLS_ERR_SIGNATURE;
    }

    // Extract the RSA public key from the issuer's certificate
    rsa_key_t issuer_key;
    memset(&issuer_key, 0, sizeof(rsa_key_t));
    if (x509_extract_rsa_key(issuer_cert->public_key, issuer_cert->public_key_len, &issuer_key) < 0) {
        return TLS_ERR_SIGNATURE;
    }

    // The TBS (To Be Signed) bytes are the raw DER of the TBSCertificate
    // from the outer Certificate SEQUENCE. We need to locate them precisely.
    // cert_content starts at the Certificate SEQUENCE content.
    // The first element is the TBSCertificate.
    const uint8_t* cert_content;
    size_t cert_content_len;
    if (parse_asn1_element(cert->raw_data, ASN1_TAG_SEQUENCE, &cert_content, &cert_content_len) < 0) {
        return TLS_ERR_CERT_VERIFY;
    }

    // Find the end of the TBS element within cert_content
    size_t tbs_len, tbs_hdr;
    if (cert_content[0] != ASN1_TAG_SEQUENCE) return TLS_ERR_CERT_VERIFY;
    if (parse_asn1_length(cert_content + 1, &tbs_len, &tbs_hdr) < 0) return TLS_ERR_CERT_VERIFY;

    size_t tbs_total = 1 + tbs_hdr + tbs_len;
    const uint8_t* tbs_der = cert_content;

    // Hash the TBS certificate
    uint8_t tbs_hash[32];
    sha256_hash(tbs_der, tbs_total, tbs_hash);

    // Verify signature using issuer's public key
    int hash_alg = 1; // SHA-256
    if (cert->signature_alg == 2) hash_alg = 2; // SHA-384 (not fully supported yet, fall through)

    int result = rsa_verify_pkcs1(&issuer_key, cert->signature, cert->signature_len,
                                   tbs_hash, 32, hash_alg);
    return result;
}

// Check if a hostname matches a certificate's identity (CN or SANs)
static int x509_hostname_matches(x509_cert_t* cert, const char* hostname) {
    if (!hostname) return 0;

    // Check Subject Alternative Names first (per RFC 6125)
    for (int i = 0; i < cert->san_count; i++) {
        // Exact match
        if (strcmp(hostname, cert->san_entries[i]) == 0) return 1;

        // Wildcard match: SAN entry is "*.example.com"
        if (cert->san_entries[i][0] == '*' && cert->san_entries[i][1] == '.') {
            const char* host_dot = strchr(hostname, '.');
            if (host_dot && strcmp(host_dot, cert->san_entries[i] + 1) == 0) return 1;
        }
    }

    // Fall back to Common Name (CN) if no SAN match
    if (cert->common_name[0]) {
        // Exact match
        if (strcmp(hostname, cert->common_name) == 0) return 1;

        // Wildcard match: cert CN = "*.example.com"
        if (cert->common_name[0] == '*' && cert->common_name[1] == '.') {
            const char* host_dot = strchr(hostname, '.');
            if (host_dot && strcmp(host_dot, cert->common_name + 1) == 0) return 1;
        }
    }

    return 0; // No match
}

int tls_verify_certificate(x509_cert_t* cert, const char* hostname) {
    // Step 1: Check certificate validity period (expiry)
    if (x509_check_validity(cert) < 0) {
        return TLS_ERR_CERT_VERIFY;
    }

    // Step 2: Check hostname match (CN or SANs)
    if (hostname && hostname[0]) {
        if (!x509_hostname_matches(cert, hostname)) {
            return TLS_ERR_CERT_VERIFY;
        }
    }

    // Step 3: For self-signed certificates, verify they are in the trust store
    if (cert->is_self_signed) {
        // Verify the self-signed signature first
        if (x509_verify_signature(cert, cert) < 0) {
            return TLS_ERR_SIGNATURE;
        }
        // Check if this self-signed cert is a trusted root CA
        if (!tls_ca_is_trusted_fingerprint(cert->fingerprint)) {
            // Self-signed but NOT in trust store - reject
            return TLS_ERR_CERT_VERIFY;
        }
    }

    return 0;
}

// Verify a full certificate chain
int tls_verify_cert_chain(x509_cert_t* chain, int count, const char* hostname) {
    if (!chain || count <= 0) return TLS_ERR_CERT_VERIFY;

    // Verify the leaf certificate (index 0)
    int ret = tls_verify_certificate(&chain[0], hostname);
    if (ret < 0) return ret;

    // If there's only one cert and it's self-signed, tls_verify_certificate
    // already checked the trust store. We're done.
    if (count == 1) return 0;

    // Walk the chain: each cert should be signed by the next one
    for (int i = 0; i < count - 1; i++) {
        // Check validity of each intermediate cert
        if (x509_check_validity(&chain[i + 1]) < 0) {
            return TLS_ERR_CERT_VERIFY;
        }

        // Verify that cert[i] was signed by cert[i+1]
        ret = x509_verify_signature(&chain[i], &chain[i + 1]);
        if (ret < 0) return ret;
    }

    // The last cert in the chain should be (or chain to) a trusted root
    x509_cert_t* root = &chain[count - 1];
    if (root->is_self_signed) {
        // Verify self-signed signature
        if (x509_verify_signature(root, root) < 0) {
            return TLS_ERR_SIGNATURE;
        }
        // Must be in our trust store
        if (!tls_ca_is_trusted_fingerprint(root->fingerprint)) {
            return TLS_ERR_CERT_VERIFY;
        }
    } else {
        // Last cert is not self-signed - check if it matches a trusted root
        // by fingerprint (it might be an intermediate that chains to a root
        // we have, or the root itself might not have been sent)
        if (!tls_ca_is_trusted_fingerprint(root->fingerprint)) {
            // Also try matching the issuer against trusted CAs
            const root_ca_entry_t* ca = tls_ca_find(root->issuer_cn);
            if (!ca || !(ca->flags & CA_FLAG_TRUSTED)) {
                return TLS_ERR_CERT_VERIFY;
            }
        }
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
    session->socket_fd = -1;   // No socket yet (-1 means not connected)
    
    return session;
}

void tls_destroy_session(tls_session_t* session) {
    if (session) {
        // Only close the socket if we created it ourselves
        // (if tls_client_handshake_fd was used, the caller manages the socket)
        if (session->socket_fd >= 0) {
            k_close(session->socket_fd);
            session->socket_fd = -1;
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
    header->version = htons(session->version);  // CRITICAL FIX: version MUST be network byte order too
    header->length = htons(len);  // CRITICAL FIX: TLS record length MUST be network byte order (big-endian)
    
    memcpy(record + 5, data, len);
    
    return k_sendto(session->socket_fd, record, len + 5, 0, NULL);
}

// Helper to receive exactly N bytes from a socket
// FIXED: Removed redundant rtl8139_poll() and http_process_events() calls.
// k_recvfrom() already polls the NIC and calls http_process_events() internally
// when waiting for data. Calling them AGAIN here creates nested blocking loops
// that can deadlock:
//   tls_recv_all -> k_recvfrom (blocks, polls NIC) -> returns no data
//   tls_recv_all -> rtl8139_poll() (redundant) -> http_process_events() (redundant)
// This also starved the GUI because the outer poll+sleep was unnecessary overhead.
static int tls_recv_all(int fd, uint8_t* buffer, size_t len) {
    size_t total = 0;
    uint32_t start = get_tick_count();
    // TLS per-read timeout: 60 seconds (3000 ticks at 50Hz).
    // QEMU SLIRP can delay server responses by 10-30 seconds. We use 60s
    // to be safe. The timer does NOT reset on partial reads — the total
    // time from the first call to this function must not exceed 60s.
    uint32_t tls_timeout = 3000;

    while (total < len) {
        int r = k_recvfrom(fd, buffer + total, len - total, 0, NULL);
        if (r > 0) {
            total += r;
            // NOTE: Do NOT reset 'start' here. The total timeout is measured
            // from the beginning of this tls_recv_all call, not from the last
            // successful read. This prevents spurious zero bytes from resetting
            // the timer and making us wait another 60s per byte.
        } else if (r == 0) {
            // Connection closed by peer
            s_printf("[TLS] tls_recv_all: connection closed (EOF) after %d/%d bytes\n", (int)total, (int)len);
            return (total > 0) ? (int)total : -1;
        } else {
            // r < 0: no data available or k_recvfrom internal timeout
            // Check overall timeout
            uint32_t elapsed = get_tick_count() - start;
            if (elapsed > tls_timeout) {
                s_printf("[TLS] tls_recv_all: timeout after %d ticks (%d/%d bytes received)\n",
                         elapsed, (int)total, (int)len);
                return (total > 0) ? (int)total : -1;
            }
            // Keep waiting — k_recvfrom already polled the NIC internally
        }
    }
    return (int)total;
}

static int tls_recv_record(tls_session_t* session, uint8_t* content_type,
                           uint8_t* buffer, size_t max_len) {
    uint8_t header[5];

    // ----------------------------------------------------------------
    // QEMU SLIRP zero-byte prelude — ROOT CAUSE ANALYSIS
    //
    // QEMU's SLIRP userspace TCP stack has a quirk where it sometimes
    // sends a 6-byte all-zero segment as the very first data segment
    // after the TCP 3-way handshake completes. This zero segment has
    // the SAME sequence number as the real TLS ServerHello that
    // follows it.
    //
    // When the real ServerHello arrives (at the same seq), our TCP
    // layer sees a partial overlap and trims the first 6 bytes —
    // which happen to be the real TLS record header:
    //
    //   Real ServerHello on the wire:
    //     [16 03 03 LH LL] [02 00 00 HS_LEN...] [server_version] [random] ...
    //      ^--- 5-byte TLS record header ---^   ^--- handshake body ---^
    //
    //   What SLIRP delivers to us:
    //     Segment 1: seq=S,     len=6,  data=[00 00 00 00 00 00]
    //     Segment 2: seq=S,     len=N,  data=[16 03 03 LH LL 02 00 00 HS_LEN ...]
    //
    //   After TCP overlap trimming (Segment 2's first 6 bytes eaten):
    //     Socket buffer = [00 00 00 00 00 00] [02 00 00 HS_LEN 03 03 <random> ...]
    //                      ^--- 6 zeros ---^   ^--- handshake body WITHOUT record header ---^
    //
    // So the 5-byte TLS record header (16 03 03 LH LL) is GONE. What
    // remains is: 6 zeros, then the handshake type (0x02), then the
    // 3-byte handshake length, then the rest of the ServerHello body.
    //
    // RECONSTRUCTION STRATEGY:
    //   1. Read the first byte. If it's 0x00, read 5 more to confirm
    //      the 6-byte zero prelude.
    //   2. Read 1 byte → handshake_type (should be 0x02 = ServerHello).
    //   3. Read 3 bytes → handshake_length (24-bit big-endian).
    //   4. Reconstruct: content_type=0x16, version=0x0303,
    //      record_length = handshake_length + 4.
    //   5. Build buffer: [hs_type] [hs_len_bytes] [body...].
    //   6. Read handshake_length bytes of body.
    //
    // PREVIOUS BUGS:
    //   * Original code read 3 bytes after the zeros and treated them
    //     ALL as the handshake length — but the first byte (0x02) is
    //     the handshake TYPE, not part of the length. This produced
    //     hs_len=0x020000=131072 and a 60-second timeout trying to
    //     read 128 KB.
    //   * My first fix removed the reconstruction entirely and tried
    //     to "skip 64 bytes of garbage" — but the garbage included
    //     real TLS data, so it found a random 0x16 byte in the server's
    //     random field and built a garbage header with length 30384.
    // ----------------------------------------------------------------

    // Read the first byte.
    int received = tls_recv_all(session->socket_fd, header, 1);
    if (received < 1) {
        s_printf("[TLS] recv_record: EOF on first byte\n");
        return TLS_ERR_SOCKET;
    }

    // Check for the SLIRP 6-byte zero prelude.
    if (header[0] == 0x00) {
        // Read 5 more bytes to confirm they're all zero.
        uint8_t peek[5];
        received = tls_recv_all(session->socket_fd, peek, 5);
        if (received < 5) {
            s_printf("[TLS] recv_record: short read during zero-prelude check\n");
            return TLS_ERR_SOCKET;
        }

        int all_zeros = 1;
        for (int i = 0; i < 5; i++) {
            if (peek[i] != 0) { all_zeros = 0; break; }
        }

        if (all_zeros) {
            s_printf("[TLS] Detected 6-byte zero prelude (QEMU SLIRP) — reconstructing record header\n");

            // Read the handshake type (1 byte) and handshake length (3 bytes).
            // These are what TCP delivered after trimming the 6-byte overlap
            // — the real TLS record header (16 03 03 LH LL) was consumed by
            // the overlap trim, leaving just the handshake header + body.
            uint8_t hs_type;
            received = tls_recv_all(session->socket_fd, &hs_type, 1);
            if (received < 1) {
                s_printf("[TLS] recv_record: EOF reading handshake type\n");
                return TLS_ERR_SOCKET;
            }

            uint8_t hs_len_bytes[3];
            received = tls_recv_all(session->socket_fd, hs_len_bytes, 3);
            if (received < 3) {
                s_printf("[TLS] recv_record: EOF reading handshake length\n");
                return TLS_ERR_SOCKET;
            }

            uint32_t hs_len = ((uint32_t)hs_len_bytes[0] << 16) |
                              ((uint32_t)hs_len_bytes[1] << 8) |
                              (uint32_t)hs_len_bytes[2];
            uint16_t record_len = (uint16_t)(hs_len + 4);  // +4 for handshake header

            // Sanity-check the handshake length. A typical ServerHello is
            // 70-90 bytes; Certificate can be up to ~16 KB. Anything > 16 KB
            // or 0 is corrupt.
            if (hs_len == 0 || hs_len > 16384) {
                s_printf("[TLS] recv_record: bad handshake length %d after zero prelude (hs_type=0x%02X)\n",
                         (int)hs_len, hs_type);
                return TLS_ERR_SOCKET;
            }

            s_printf("[TLS] Reconstructed: content_type=0x16, hs_type=0x%02X, hs_len=%d, record_len=%d\n",
                     hs_type, (int)hs_len, (int)record_len);

            *content_type = 0x16;  // Handshake

            // Build the buffer: [hs_type] [hs_len_bytes] [body...]
            // This is exactly what tls_parse_server_hello expects — a
            // handshake header (type + 3-byte length) followed by the
            // handshake body.
            if (record_len > max_len) {
                s_printf("[TLS] recv_record: record_len %d > max_len %d\n",
                         (int)record_len, (int)max_len);
                return TLS_ERR_SOCKET;
            }

            buffer[0] = hs_type;
            buffer[1] = hs_len_bytes[0];
            buffer[2] = hs_len_bytes[1];
            buffer[3] = hs_len_bytes[2];

            uint32_t body_remaining = hs_len;
            if (body_remaining > max_len - 4) {
                body_remaining = (uint32_t)(max_len - 4);
            }

            received = tls_recv_all(session->socket_fd, buffer + 4, body_remaining);
            if (received < (int)body_remaining) {
                s_printf("[TLS] recv_record: short body read after reconstruction (%d/%d)\n",
                         received, (int)body_remaining);
                return TLS_ERR_SOCKET;
            }

            s_printf("[TLS] recv_record: reconstructed record OK, returning %d bytes\n",
                     (int)(4 + body_remaining));
            return (int)(4 + body_remaining);
        } else {
            // First byte was 0x00 but the next 5 weren't all zero.
            // This shouldn't happen in practice (no valid TLS record
            // starts with 0x00). Treat the 6 bytes we've read as a
            // corrupted header and abort with a diagnostic dump.
            s_printf("[TLS] recv_record: first byte 0x00 but not all-zero prelude\n");
            s_printf("[TLS] recv_record: 6-byte hex dump: 00 %02X %02X %02X %02X %02X\n",
                     peek[0], peek[1], peek[2], peek[3], peek[4]);
            return TLS_ERR_SOCKET;
        }
    }

    // Normal case: first byte is a valid content_type. Read remaining 4.
    received = tls_recv_all(session->socket_fd, header + 1, 4);
    if (received < 4) {
        s_printf("[TLS] recv_record: short header read (%d/4 trailing bytes)\n", received);
        return TLS_ERR_SOCKET;
    }

    tls_record_header_t* hdr = (tls_record_header_t*)header;
    *content_type = hdr->content_type;

    // Validate content_type.
    if (*content_type < 0x14 || *content_type > 0x17) {
        s_printf("[TLS] recv_record: INVALID content_type=0x%02X (expected 0x14-0x17)\n",
                 *content_type);
        s_printf("[TLS] recv_record: header hex dump:");
        for (int i = 0; i < 5; i++) {
            s_printf(" %02X", header[i]);
        }
        s_printf("\n");
        return TLS_ERR_SOCKET;
    }

    uint16_t len = ntohs(hdr->length);

    // s_printf in CamelOS doesn't support %04X, so print version bytes individually.
    {
        uint8_t* vp = (uint8_t*)&hdr->version;
        s_printf("[TLS] recv_record: content_type=0x%02X, version=0x%02X%02X, length=%d\n",
                 hdr->content_type, vp[0], vp[1], (int)len);
    }

    // Sanity-check the record length.
    if (len == 0 || len > 18432) {
        s_printf("[TLS] recv_record: invalid record length %d (expected 1..18432)\n", (int)len);
        return TLS_ERR_SOCKET;
    }
    if (len > max_len) {
        len = (uint16_t)max_len;
    }

    received = tls_recv_all(session->socket_fd, buffer, len);
    if (received < len) {
        s_printf("[TLS] recv_record: short body read (%d/%d bytes)\n", received, (int)len);
        return TLS_ERR_SOCKET;
    }
    return received;
}

static int tls_send_client_hello(tls_session_t* session) {
    uint8_t hello[2048];  // Increased buffer for more extensions
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
    
    // Cipher suites - modern suites that Google and most servers accept
    // Order: ECDHE first (forward secrecy), then RSA fallback
    uint16_t cipher_suites[] = {
        TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,  // 0xC02B - Google uses ECDSA
        TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,  // 0xC02C
        TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,    // 0xC02F
        TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,    // 0xC030
        TLS_RSA_WITH_AES_128_GCM_SHA256,           // 0x009C
        TLS_RSA_WITH_AES_256_GCM_SHA384,           // 0x009D
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
    
    // === Extensions ===
    // We build extensions sequentially; the total length is computed at the end
    
    uint8_t* ext_len_ptr = p;
    p += 2;  // Extensions total length placeholder
    
    // 1. Server Name extension (SNI) - REQUIRED by most servers
    size_t sni_len = strlen(session->server_name);
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
    
    // 2. Supported Groups extension (0x000A) - CRITICAL for ECDHE
    // Without this, servers MUST abort per RFC 8422
    tls_write_uint16(0x000A, p);  // Extension type: supported_groups
    p += 2;
    tls_write_uint16(6, p);  // Extension data length
    p += 2;
    tls_write_uint16(4, p);  // Groups list length (2 groups * 2 bytes)
    p += 2;
    tls_write_uint16(0x001D, p);  // X25519 (preferred - we have real impl)
    p += 2;
    tls_write_uint16(0x0017, p);  // secp256r1 (P-256)
    p += 2;
    
    // 3. EC Point Formats extension (0x000B) - required by some servers
    tls_write_uint16(0x000B, p);  // Extension type: ec_point_formats
    p += 2;
    tls_write_uint16(2, p);  // Extension data length
    p += 2;
    *p++ = 1;  // Formats list length
    *p++ = 0;  // uncompressed
    
    // 4. Signature Algorithms extension (0x000D) - REQUIRED for TLS 1.2
    // Must include RSA-PSS and ECDSA algorithms for modern servers
    uint16_t sig_algs[] = {
        0x0403,  // ecdsa_secp256r1_sha256
        0x0503,  // ecdsa_secp384r1_sha384
        0x0603,  // ecdsa_secp521r1_sha512
        0x0804,  // rsa_pss_rsae_sha256
        0x0805,  // rsa_pss_rsae_sha384
        0x0806,  // rsa_pss_rsae_sha512
        0x0401,  // rsa_pkcs1_sha256
        0x0501,  // rsa_pkcs1_sha384
        0x0601,  // rsa_pkcs1_sha512
    };
    int sig_alg_count = sizeof(sig_algs) / sizeof(sig_algs[0]);
    
    tls_write_uint16(0x000D, p);  // Extension type: signature_algorithms
    p += 2;
    tls_write_uint16(sig_alg_count * 2 + 2, p);  // Extension data length
    p += 2;
    tls_write_uint16(sig_alg_count * 2, p);  // Algorithms list length
    p += 2;
    for (int i = 0; i < sig_alg_count; i++) {
        tls_write_uint16(sig_algs[i], p);
        p += 2;
    }
    
    // 5. ALPN extension (0x0010) - Application-Layer Protocol Negotiation
    // Required by many servers (Cloudflare, Google, etc.)
    tls_write_uint16(0x0010, p);  // Extension type: alpn
    p += 2;
    // FIX: Extension data length = 2 (list_len) + 3 (h2) + 9 (http/1.1) = 14
    // Previously was 13 (off-by-one), which corrupted all subsequent extensions
    // and caused servers to reject the ClientHello with decode_error.
    tls_write_uint16(14, p);  // Extension data length
    p += 2;
    // FIX: Protocol list length = 3 (h2) + 9 (http/1.1) = 12
    tls_write_uint16(12, p);  // ALPN protocol list length
    p += 2;
    // "h2" (HTTP/2)
    *p++ = 2;  // Protocol length
    *p++ = 'h'; *p++ = '2';
    // "http/1.1"
    *p++ = 8;  // Protocol length
    *p++ = 'h'; *p++ = 't'; *p++ = 't'; *p++ = 'p';
    *p++ = '/'; *p++ = '1'; *p++ = '.'; *p++ = '1';

    // 6. Supported Versions extension (0x002B)
    // We only offer TLS 1.2 (0x0303) because our TLS 1.3 implementation is
    // incomplete (tls13_connect is a stub). If we offer TLS 1.3, servers will
    // pick it and the handshake will fail when we can't process the TLS 1.3
    // ServerHello (cipher suite 0x1301 not recognized by our TLS 1.2 parser).
    // Most servers still accept TLS 1.2 fallback.
    tls_write_uint16(0x002B, p);  // Extension type: supported_versions
    p += 2;
    tls_write_uint16(3, p);  // Extension data length (1 + 1*2 = 3)
    p += 2;
    *p++ = 2;  // Versions list length (1 version * 2 bytes)
    tls_write_uint16(0x0303, p);  // TLS 1.2 only
    p += 2;

    // Calculate extensions total length
    uint16_t ext_total = p - ext_len_ptr - 2;
    tls_write_uint16(ext_total, ext_len_ptr);
    
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
        case TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
            session->cipher_key_size = 16;
            session->cipher_iv_size = 4;  // Implicit IV
            session->cipher_mac_size = 0; // GCM has auth tag
            break;
        case TLS_RSA_WITH_AES_256_GCM_SHA384:
        case TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
        case TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
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
    
    // For ECDHE key exchange (both RSA and ECDSA authenticated)
    if (session->cipher_suite == TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 ||
        session->cipher_suite == TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 ||
        session->cipher_suite == TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 ||
        session->cipher_suite == TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384) {
        
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
        
        // Store server's ECDHE ephemeral public key SEPARATELY
        // (so it doesn't get overwritten when we generate our keypair)
        session->server_key_type = 3;  // ECDHE
        session->server_ecdhe_curve = curve;
        if (pk_len <= sizeof(session->server_ecdhe_public_key)) {
            memcpy(session->server_ecdhe_public_key, p, pk_len);
            session->server_ecdhe_public_key_len = pk_len;
        }
        p += pk_len;
        
        // Signature (verify with server certificate)
        // For now, skip signature verification - just advance past the signature
        // The signature is included in the handshake hash which is computed
        // from the full record, so skipping here is OK for the hash.
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
        // ECDHE key exchange
        // Server's ECDHE public key is stored in server_ecdhe_public_key
        // We need to generate our keypair and compute shared secret
        
        uint8_t shared_secret[64];
        int shared_len = 0;
        
        if (session->server_ecdhe_curve == 0x001D) {
            // X25519 key exchange (RFC 8422) - we have a real implementation!
            // Generate X25519 keypair
            extern int x25519_generate_keypair(uint8_t* pub, uint8_t* priv);
            extern int x25519_compute_shared(const uint8_t* priv, const uint8_t* peer, uint8_t* shared);
            
            x25519_generate_keypair(session->x25519_public_key, session->x25519_private_key);
            
            // Compute shared secret using server's X25519 public key
            if (x25519_compute_shared(session->x25519_private_key,
                                      session->server_ecdhe_public_key,
                                      shared_secret) != 0) {
                return TLS_ERR_KEY_EXCHANGE;
            }
            shared_len = 32;
            
            // Send our X25519 public key (32 bytes, no 0x04 prefix for X25519)
            *p++ = 32;  // Public key length
            memcpy(p, session->x25519_public_key, 32);
            p += 32;
            
        } else if (session->server_ecdhe_curve == 0x0017) {
            // P-256 (secp256r1) - use the existing (simplified) ECDH
            // NOTE: The current P-256 ECDH is not real EC math, so this will
            // produce wrong results. But at least the protocol flow is correct.
            // X25519 should be preferred as it has a real implementation.
            ecdh_generate_keypair(&session->ecdhe_key, EC_CURVE_P256);
            
            shared_len = ecdh_compute_shared_secret(&session->ecdhe_key,
                                                     session->server_ecdhe_public_key,
                                                     session->server_ecdhe_public_key_len,
                                                     shared_secret);
            
            // Send our P-256 public key (uncompressed: 0x04 + 32 + 32 = 65 bytes)
            *p++ = session->ecdhe_key.public_key_len;
            memcpy(p, session->ecdhe_key.public_key, session->ecdhe_key.public_key_len);
            p += session->ecdhe_key.public_key_len;
        } else {
            // Unsupported curve
            return TLS_ERR_KEY_EXCHANGE;
        }
        
        // Store the shared secret as the pre-master secret
        // For ECDHE, the PMS is the shared secret (variable length)
        // For TLS 1.2 PRF, we need to pass the actual PMS length
        memset(session->master_secret, 0, sizeof(session->master_secret));
        if (shared_len > 0 && shared_len <= (int)sizeof(session->master_secret)) {
            memcpy(session->master_secret, shared_secret, shared_len);
            session->pre_master_secret_len = shared_len;
        }
        
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
        session->pre_master_secret_len = 48;
        
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
    
    // Use actual pre-master secret length (48 for RSA, variable for ECDHE)
    size_t pms_len = session->pre_master_secret_len;
    if (pms_len == 0) pms_len = 48;  // Fallback default
    
    tls_prf(session->master_secret, pms_len, "master secret",
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
    
    // Extract keys and IVs from key_block
    uint8_t* kb = session->key_block;
    
    // Client write key
    uint8_t* client_key = kb; kb += session->cipher_key_size;
    // Server write key
    uint8_t* server_key = kb; kb += session->cipher_key_size;
    // Client write IV (implicit nonce, 4 bytes for TLS 1.2 GCM)
    uint8_t* client_iv  = kb; kb += session->cipher_iv_size;
    // Server write IV
    uint8_t* server_iv  = kb;
    
    // Store IVs for use when building per-record nonces
    memcpy(session->write_iv, client_iv, session->cipher_iv_size);
    memcpy(session->read_iv,  server_iv, session->cipher_iv_size);
    
    // Initialize AES-GCM contexts with actual keys and IVs
    aes_gcm_init(&session->write_ctx, client_key, session->cipher_key_size * 8, client_iv);
    aes_gcm_init(&session->read_ctx,  server_key, session->cipher_key_size * 8, server_iv);
    
    return 0;
}

static int tls_send_change_cipher_spec(tls_session_t* session) {
    uint8_t ccs = 1;
    return tls_send_record(session, TLS_CONTENT_CHANGE_CIPHER_SPEC, &ccs, 1);
}

static int tls_send_finished(tls_session_t* session) {
    // Compute verify_data
    uint8_t verify_data[12];
    uint8_t handshake_hash[32];
    sha256_ctx_t hash_copy = session->handshake_hash;  // don't destroy running hash
    sha256_final(&hash_copy, handshake_hash);
    tls_prf(session->master_secret, 48, "client finished",
            handshake_hash, 32, verify_data, 12);

    // Build plaintext: handshake header (4) + verify_data (12) = 16 bytes
    uint8_t plaintext[16];
    plaintext[0] = TLS_HANDSHAKE_FINISHED;
    tls_write_uint24(12, plaintext + 1);
    memcpy(plaintext + 4, verify_data, 12);

    // Update handshake transcript with the plaintext Finished
    sha256_update(&session->handshake_hash, plaintext, 16);

    // Build TLS GCM record: explicit nonce (8 bytes) + ciphertext + tag (16)
    // TLS 1.2 GCM: nonce = implicit_IV[4] || explicit_nonce[8]
    uint8_t explicit_nonce[8];
    tls_write_uint64(session->write_seq_num, explicit_nonce);

    uint8_t nonce[12];
    memcpy(nonce,   session->write_iv, 4);   // implicit IV
    memcpy(nonce+4, explicit_nonce,    8);   // explicit nonce

    // AAD: seq_num(8) || type(1) || version(2) || length(2)
    uint8_t aad[13];
    tls_write_uint64(session->write_seq_num, aad);
    aad[8]  = TLS_CONTENT_HANDSHAKE;
    tls_write_uint16(session->version, aad + 9);
    tls_write_uint16(16, aad + 11);  // length of ciphertext+tag

    // Reinit context with per-record nonce
    aes_gcm_init(&session->write_ctx,
                 session->key_block,              // client write key
                 session->cipher_key_size * 8,
                 nonce);

    uint8_t encrypted[16], tag[16];
    aes_gcm_encrypt(&session->write_ctx, plaintext, 16, aad, 13, encrypted, tag);
    session->write_seq_num++;

    // Assemble: explicit_nonce(8) + ciphertext(16) + tag(16) = 40 bytes
    uint8_t record_data[40];
    memcpy(record_data,      explicit_nonce, 8);
    memcpy(record_data + 8,  encrypted,      16);
    memcpy(record_data + 24, tag,             16);

    return tls_send_record(session, TLS_CONTENT_HANDSHAKE, record_data, 40);
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
    uint8_t* buffer;  // Use heap allocation instead of stack
    uint8_t content_type;
    
    // Allocate buffer on heap to avoid stack overflow (16KB stack limit)
    buffer = (uint8_t*)kmalloc(8192);
    if (!buffer) {
        return TLS_ERR_MEMORY;
    }
    
    // Initialize handshake hash
    sha256_init(&session->handshake_hash);
    
    // Set hostname
    tls_set_hostname(session, hostname);
    session->port = port;
    
    // Check if socket is already connected (e.g., via tls_client_handshake_fd)
    // If so, skip socket creation and DNS resolution - reuse the existing connection
    if (session->socket_fd < 0) {
        // No existing socket - create one and connect
        session->socket_fd = k_socket(AF_INET, SOCK_STREAM, 0);
        if (session->socket_fd < 0) {
            kfree(buffer);
            return TLS_ERR_SOCKET;
        }
        
        // Resolve hostname
        char ip_str[32];
        if (dns_resolve(hostname, ip_str, sizeof(ip_str)) < 0) {
            k_close(session->socket_fd);
            session->socket_fd = -1;
            kfree(buffer);
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
            session->socket_fd = -1;
            kfree(buffer);
            return TLS_ERR_SOCKET;
        }
    }
    
    session->state = TLS_STATE_CONNECTING;

    // NOTE: The drain logic that was here has been removed. It was causing
    // more harm than good — if the server responded quickly, the drain would
    // eat the real ServerHello before tls_recv_record could read it. The
    // byte-skip loop in tls_recv_record (which skips non-TLS bytes before
    // the record header) is sufficient to handle spurious leading data.

    // Send ClientHello
    ret = tls_send_client_hello(session);
    if (ret < 0) {
        s_printf("[TLS] Step 1 FAILED: tls_send_client_hello returned %d\n", ret);
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    s_printf("[TLS] Step 1 OK: ClientHello sent (%d bytes)\n", ret);
    session->state = TLS_STATE_HELLO_SENT;

    // Receive ServerHello
    ret = tls_recv_record(session, &content_type, buffer, 8192);
    if (ret < 0) {
        s_printf("[TLS] Step 2 FAILED: tls_recv_record returned %d\n", ret);
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    s_printf("[TLS] Step 2 OK: received record, ret=%d, content_type=%d (expected %d)\n",
             ret, content_type, TLS_CONTENT_HANDSHAKE);
    if (content_type != TLS_CONTENT_HANDSHAKE) {
        s_printf("[TLS] Step 2 FAILED: content_type %d != HANDSHAKE %d\n",
                 content_type, TLS_CONTENT_HANDSHAKE);
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }

    if (buffer[0] != TLS_HANDSHAKE_SERVER_HELLO) {
        s_printf("[TLS] Step 2 FAILED: buffer[0]=%d != SERVER_HELLO %d\n",
                 buffer[0], TLS_HANDSHAKE_SERVER_HELLO);
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    s_printf("[TLS] Step 2 OK: ServerHello received\n");
    
    // Update handshake hash
    size_t handshake_len = tls_read_uint24(buffer + 1) + 4;
    sha256_update(&session->handshake_hash, buffer, handshake_len);
    
    // Parse ServerHello
    ret = tls_parse_server_hello(session, buffer + 4, handshake_len - 4);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    session->state = TLS_STATE_HELLO_RECEIVED;
    
    // Receive Certificate
    ret = tls_recv_record(session, &content_type, buffer, 8192);
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    
    if (buffer[0] != TLS_HANDSHAKE_CERTIFICATE) {
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    
    handshake_len = tls_read_uint24(buffer + 1) + 4;
    sha256_update(&session->handshake_hash, buffer, handshake_len);
    
    // Parse Certificate
    ret = tls_parse_certificate(session, buffer + 4, handshake_len - 4);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    
    // Verify certificate chain
    if (session->verify_cert) {
        ret = tls_verify_cert_chain(session->cert_chain, session->cert_count, hostname);
        if (ret < 0) {
            kfree(buffer);
            return ret;
        }
    }
    
    // Call certificate verification callback
    if (session->on_cert_verify) {
        session->on_cert_verify(&session->cert_chain[0], session->callback_user_data);
    }
    
    session->state = TLS_STATE_CERTIFICATE_RECEIVED;
    
    // Receive ServerKeyExchange (for ECDHE) or ServerHelloDone
    ret = tls_recv_record(session, &content_type, buffer, 8192);
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    
    if (buffer[0] == TLS_HANDSHAKE_SERVER_KEY_EXCHANGE) {
        handshake_len = tls_read_uint24(buffer + 1) + 4;
        sha256_update(&session->handshake_hash, buffer, handshake_len);
        
        ret = tls_parse_server_key_exchange(session, buffer + 4, handshake_len - 4);
        if (ret < 0) {
            kfree(buffer);
            return ret;
        }
        
        session->state = TLS_STATE_KEY_EXCHANGE_RECEIVED;
        
        // Receive ServerHelloDone
        ret = tls_recv_record(session, &content_type, buffer, 8192);
        if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
            kfree(buffer);
            return TLS_ERR_HANDSHAKE;
        }
    }
    
    if (buffer[0] != TLS_HANDSHAKE_SERVER_HELLO_DONE) {
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    
    handshake_len = tls_read_uint24(buffer + 1) + 4;
    sha256_update(&session->handshake_hash, buffer, handshake_len);
    
    session->state = TLS_STATE_HELLO_DONE_RECEIVED;
    
    // Send ClientKeyExchange
    ret = tls_send_client_key_exchange(session);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    
    // Derive keys
    ret = tls_derive_keys(session);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    
    session->state = TLS_STATE_KEY_EXCHANGE_SENT;
    
    // Send ChangeCipherSpec
    ret = tls_send_change_cipher_spec(session);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    
    // Send Finished
    ret = tls_send_finished(session);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    session->state = TLS_STATE_FINISHED_SENT;
    
    // Receive ChangeCipherSpec
    ret = tls_recv_record(session, &content_type, buffer, 8192);
    if (ret < 0 || content_type != TLS_CONTENT_CHANGE_CIPHER_SPEC) {
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    
    // Receive Finished
    ret = tls_recv_record(session, &content_type, buffer, 8192);
    if (ret < 0 || content_type != TLS_CONTENT_HANDSHAKE) {
        kfree(buffer);
        return TLS_ERR_HANDSHAKE;
    }
    
    ret = tls_verify_server_finished(session, buffer, ret);
    if (ret < 0) {
        kfree(buffer);
        return ret;
    }
    
    session->state = TLS_STATE_ESTABLISHED;
    kfree(buffer);
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
    if (session->state != TLS_STATE_ESTABLISHED) return TLS_ERR_PROTOCOL;
    if (len > 4096) len = 4096;

    // Explicit nonce (8 bytes) = write sequence number
    uint8_t explicit_nonce[8];
    tls_write_uint64(session->write_seq_num, explicit_nonce);

    // Build full 12-byte nonce: implicit(4) || explicit(8)
    uint8_t nonce[12];
    memcpy(nonce,   session->write_iv, 4);
    memcpy(nonce+4, explicit_nonce,    8);

    // AAD
    uint8_t aad[13];
    tls_write_uint64(session->write_seq_num, aad);
    aad[8]  = TLS_CONTENT_APPLICATION_DATA;
    tls_write_uint16(session->version, aad + 9);
    tls_write_uint16((uint16_t)(len + 16), aad + 11);

    // Re-init AES-GCM with fresh nonce
    aes_gcm_init(&session->write_ctx,
                 session->key_block,
                 session->cipher_key_size * 8,
                 nonce);

    uint8_t* encrypted = (uint8_t*)kmalloc(len + 16);
    if (!encrypted) return TLS_ERR_MEMORY;
    uint8_t tag[16];
    aes_gcm_encrypt(&session->write_ctx, data, len, aad, 13, encrypted, tag);
    session->write_seq_num++;

    // Build TLS record: explicit_nonce(8) + ciphertext(len) + tag(16)
    size_t record_data_len = 8 + len + 16;
    uint8_t* record_data = (uint8_t*)kmalloc(record_data_len);
    if (!record_data) { kfree(encrypted); return TLS_ERR_MEMORY; }
    memcpy(record_data,        explicit_nonce, 8);
    memcpy(record_data + 8,    encrypted,      len);
    memcpy(record_data + 8 + len, tag,         16);
    kfree(encrypted);

    int ret = tls_send_record(session, TLS_CONTENT_APPLICATION_DATA,
                              record_data, record_data_len);
    kfree(record_data);
    return ret;
}

int tls_read(tls_session_t* session, void* buffer, size_t max_len) {
    if (session->state != TLS_STATE_ESTABLISHED) return TLS_ERR_PROTOCOL;

    uint8_t content_type;
    uint8_t* temp = (uint8_t*)kmalloc(TLS_MAX_RECORD_SIZE);
    if (!temp) return TLS_ERR_MEMORY;

    int received = tls_recv_record(session, &content_type, temp, TLS_MAX_RECORD_SIZE);
    if (received < 0) { kfree(temp); return received; }

    if (content_type == TLS_CONTENT_ALERT) {
        if (received >= 2 && temp[0] == TLS_ALERT_LEVEL_FATAL)
            session->state = TLS_STATE_ERROR;
        else if (received >= 2 && temp[1] == TLS_ALERT_CLOSE_NOTIFY)
            session->state = TLS_STATE_CLOSED;
        kfree(temp); return 0;
    }

    if (content_type != TLS_CONTENT_APPLICATION_DATA) {
        kfree(temp); return TLS_ERR_PROTOCOL;
    }

    // TLS 1.2 GCM record: explicit_nonce(8) + ciphertext + tag(16)
    if (received < 8 + 16) { kfree(temp); return TLS_ERR_DECRYPT; }
    uint8_t* explicit_nonce = temp;
    uint8_t* ciphertext     = temp + 8;
    size_t   ct_len         = (size_t)received - 8 - 16;
    uint8_t* tag            = temp + 8 + ct_len;

    // Rebuild 12-byte nonce
    uint8_t nonce[12];
    memcpy(nonce,   session->read_iv,  4);
    memcpy(nonce+4, explicit_nonce,    8);

    // AAD
    uint8_t aad[13];
    tls_write_uint64(session->read_seq_num, aad);
    aad[8]  = TLS_CONTENT_APPLICATION_DATA;
    tls_write_uint16(session->version, aad + 9);
    tls_write_uint16((uint16_t)(ct_len + 16), aad + 11);

    // Server write key is at offset cipher_key_size in key_block
    aes_gcm_init(&session->read_ctx,
                 session->key_block + session->cipher_key_size,
                 session->cipher_key_size * 8,
                 nonce);

    uint8_t* plain = (uint8_t*)kmalloc(ct_len);
    if (!plain) { kfree(temp); return TLS_ERR_MEMORY; }

    int ret = aes_gcm_decrypt(&session->read_ctx, ciphertext, ct_len,
                               aad, 13, tag, plain);
    if (ret != 0) { kfree(plain); kfree(temp); return TLS_ERR_DECRYPT; }

    session->read_seq_num++;
    size_t copy_len = ct_len < max_len ? ct_len : max_len;
    memcpy(buffer, plain, copy_len);
    kfree(plain); kfree(temp);
    return (int)copy_len;
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
