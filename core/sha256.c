// core/sha256.c - SHA-256 Hash Function Implementation for CamelOS
// Pure C implementation, no external dependencies, suitable for kernel mode
// Reference: FIPS 180-4

#include "sha256.h"
#include "string.h"

// SHA-256 Constants: first 32 bits of the fractional parts of the cube roots
// of the first 64 primes (2..311)
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// Bitwise rotation and shift operations
#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n)   ((x) >> (n))

// SHA-256 logical functions
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))

void sha256_init(sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bit_count = 0;
    ctx->buffer_len = 0;
    memset(ctx->buffer, 0, 64);
}

// Process a 512-bit (64-byte) block
static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t T1, T2;
    int t;

    // Prepare the message schedule
    for (t = 0; t < 16; t++) {
        W[t] = ((uint32_t)block[t * 4] << 24) |
               ((uint32_t)block[t * 4 + 1] << 16) |
               ((uint32_t)block[t * 4 + 2] << 8) |
               ((uint32_t)block[t * 4 + 3]);
    }
    for (t = 16; t < 64; t++) {
        W[t] = SIG1(W[t - 2]) + W[t - 7] + SIG0(W[t - 15]) + W[t - 16];
    }

    // Initialize working variables
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    // 64 rounds
    for (t = 0; t < 64; t++) {
        T1 = h + EP1(e) + CH(e, f, g) + K[t] + W[t];
        T2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    // Add the compressed chunk to the current hash value
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, uint32_t len) {
    uint32_t i;

    for (i = 0; i < len; i++) {
        ctx->buffer[ctx->buffer_len++] = data[i];
        if (ctx->buffer_len == 64) {
            sha256_transform(ctx->state, ctx->buffer);
            ctx->bit_count += 512;
            ctx->buffer_len = 0;
        }
    }
}

void sha256_final(sha256_ctx_t* ctx, uint8_t hash[SHA256_BLOCK_SIZE]) {
    uint32_t i;
    uint64_t total_bits = ctx->bit_count + (ctx->buffer_len * 8);

    // Pad with a 1 bit
    ctx->buffer[ctx->buffer_len++] = 0x80;

    // If buffer is too full for the length, process and start new block
    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < 64) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        sha256_transform(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
    }

    // Pad with zeros until we have 56 bytes
    while (ctx->buffer_len < 56) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }

    // Append the total length in bits as 64-bit big-endian
    ctx->buffer[56] = (uint8_t)(total_bits >> 56);
    ctx->buffer[57] = (uint8_t)(total_bits >> 48);
    ctx->buffer[58] = (uint8_t)(total_bits >> 40);
    ctx->buffer[59] = (uint8_t)(total_bits >> 32);
    ctx->buffer[60] = (uint8_t)(total_bits >> 24);
    ctx->buffer[61] = (uint8_t)(total_bits >> 16);
    ctx->buffer[62] = (uint8_t)(total_bits >> 8);
    ctx->buffer[63] = (uint8_t)(total_bits);

    sha256_transform(ctx->state, ctx->buffer);

    // Output the hash in big-endian
    for (i = 0; i < 8; i++) {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hash(const uint8_t* data, uint32_t len, uint8_t hash[SHA256_BLOCK_SIZE]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
}

void sha256_string(const char* str, uint8_t hash[SHA256_BLOCK_SIZE]) {
    uint32_t len = 0;
    const char* p = str;
    while (*p++) len++;
    sha256_hash((const uint8_t*)str, len, hash);
}

void sha256_to_hex(const uint8_t hash[SHA256_BLOCK_SIZE], char hex[65]) {
    static const char hex_chars[] = "0123456789abcdef";
    int i;
    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        hex[i * 2]     = hex_chars[(hash[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0x0F];
    }
    hex[64] = 0;
}

void sha256_hash_password(const char* password, char hex[65]) {
    // Salt the password with a CamelOS-specific salt to prevent rainbow table attacks
    // Format: "CamelOS:" + password
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t*)"CamelOS:", 8);
    if (password) {
        uint32_t len = 0;
        const char* p = password;
        while (*p++) len++;
        sha256_update(&ctx, (const uint8_t*)password, len);
    }
    uint8_t hash[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, hash);
    sha256_to_hex(hash, hex);
}

int sha256_verify_password(const char* password, const char* stored_hash_hex) {
    char computed_hash[65];
    sha256_hash_password(password, computed_hash);

    // Constant-time comparison to prevent timing attacks
    int result = 0;
    int i;
    for (i = 0; i < 64; i++) {
        result |= computed_hash[i] ^ stored_hash_hex[i];
    }
    return (result == 0) ? 1 : 0;
}
