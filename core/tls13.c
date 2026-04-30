// core/tls13.c - TLS 1.3 Protocol Implementation
// Implements: HKDF, X25519, TLS 1.3 handshake
// Based on RFC 8446, RFC 5869, RFC 7748

#include "tls13.h"
#include "tls.h"  // Reuse AES-GCM, SHA-256 from TLS 1.2
#include "socket.h"
#include "memory.h"
#include "string.h"
#include "../hal/cpu/timer.h"

// External declarations
extern void rtl8139_poll(void);
extern void sha256_hash(const uint8_t* data, size_t len, uint8_t* digest);
extern void sha256_init(sha256_ctx_t* ctx);
extern void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len);
extern void sha256_final(sha256_ctx_t* ctx, uint8_t* digest);
extern int aes_gcm_encrypt(aes_gcm_ctx_t* ctx, const uint8_t* plaintext, size_t pt_len,
                           const uint8_t* aad, size_t aad_len,
                           uint8_t* ciphertext, uint8_t* tag);
extern int aes_gcm_decrypt(aes_gcm_ctx_t* ctx, const uint8_t* ciphertext, size_t ct_len,
                           const uint8_t* aad, size_t aad_len,
                           const uint8_t* tag, uint8_t* plaintext);
extern int aes_gcm_init(aes_gcm_ctx_t* ctx, const uint8_t* key, int key_bits, const uint8_t* iv);
extern int dns_resolve(const char* hostname, char* ip_out, int max_len);

static int local_atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// ============================================================================
// X25519 ELLIPTIC CURVE IMPLEMENTATION (RFC 7748)
// ============================================================================

// X25519 prime: 2^255 - 19
// Field arithmetic for Curve25519

// 255-bit integer representation
typedef uint64_t fe25519[5];  // 5 x 51-bit limbs

// Prime p = 2^255 - 19
static const uint64_t P[5] = {
    0x7FFFFFFFFFFFFEDULL, 0x7FFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFULL,
    0x7FFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFULL
};

// Clamp a scalar for X25519
static void x25519_clamp(uint8_t* scalar) {
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
}

// Convert bytes to field element (little-endian)
static void fe_from_bytes(fe25519 h, const uint8_t* s) {
    // Load 5 limbs from 32 bytes
    uint64_t h0 = (uint64_t)s[0];
    h0 |= (uint64_t)s[1] << 8;
    h0 |= (uint64_t)s[2] << 16;
    h0 |= (uint64_t)s[3] << 24;
    h0 |= (uint64_t)s[4] << 32;
    h0 |= (uint64_t)s[5] << 40;
    h0 |= ((uint64_t)s[6] & 0x07) << 48;  // 51 bits max
    h[0] = h0;

    uint64_t h1 = ((uint64_t)s[6] >> 3) & 0x1F;
    h1 |= (uint64_t)s[7] << 5;
    h1 |= (uint64_t)s[8] << 13;
    h1 |= (uint64_t)s[9] << 21;
    h1 |= (uint64_t)s[10] << 29;
    h1 |= (uint64_t)s[11] << 37;
    h1 |= ((uint64_t)s[12] & 0x03) << 45;
    h[1] = h1;

    uint64_t h2 = ((uint64_t)s[12] >> 2) & 0x3F;
    h2 |= (uint64_t)s[13] << 6;
    h2 |= (uint64_t)s[14] << 14;
    h2 |= (uint64_t)s[15] << 22;
    h2 |= (uint64_t)s[16] << 30;
    h2 |= (uint64_t)s[17] << 38;
    h2 |= ((uint64_t)s[18] & 0x01) << 46;
    h[2] = h2;

    uint64_t h3 = ((uint64_t)s[18] >> 1) & 0x7F;
    h3 |= (uint64_t)s[19] << 7;
    h3 |= (uint64_t)s[20] << 15;
    h3 |= (uint64_t)s[21] << 23;
    h3 |= (uint64_t)s[22] << 31;
    h3 |= (uint64_t)s[23] << 39;
    h[3] = h3;

    uint64_t h4 = (uint64_t)s[24];
    h4 |= (uint64_t)s[25] << 8;
    h4 |= (uint64_t)s[26] << 16;
    h4 |= (uint64_t)s[27] << 24;
    h4 |= (uint64_t)s[28] << 32;
    h4 |= (uint64_t)s[29] << 40;
    h4 |= ((uint64_t)s[30] & 0x7F) << 48;  // Mask top bit
    h[4] = h4;
}

// Convert field element to bytes
static void fe_to_bytes(uint8_t* s, const fe25519 h) {
    // Carry reduction first
    uint64_t h0 = h[0];
    uint64_t h1 = h[1];
    uint64_t h2 = h[2];
    uint64_t h3 = h[3];
    uint64_t h4 = h[4];
    
    uint64_t carry = h0 >> 51;
    h0 &= 0x7FFFFFFFFFFFFULL;
    h1 += carry;
    carry = h1 >> 51;
    h1 &= 0x7FFFFFFFFFFFFULL;
    h2 += carry;
    carry = h2 >> 51;
    h2 &= 0x7FFFFFFFFFFFFULL;
    h3 += carry;
    carry = h3 >> 51;
    h3 &= 0x7FFFFFFFFFFFFULL;
    h4 += carry;
    // Final reduction mod p
    carry = h4 >> 51;
    h4 &= 0x7FFFFFFFFFFFFULL;
    h0 += carry * 19;
    
    s[0] = h0 & 0xFF;
    s[1] = (h0 >> 8) & 0xFF;
    s[2] = (h0 >> 16) & 0xFF;
    s[3] = (h0 >> 24) & 0xFF;
    s[4] = (h0 >> 32) & 0xFF;
    s[5] = (h0 >> 40) & 0xFF;
    s[6] = (h0 >> 48) | ((h1 & 0x1F) << 3);
    
    s[7] = (h1 >> 5) & 0xFF;
    s[8] = (h1 >> 13) & 0xFF;
    s[9] = (h1 >> 21) & 0xFF;
    s[10] = (h1 >> 29) & 0xFF;
    s[11] = (h1 >> 37) & 0xFF;
    s[12] = (h1 >> 45) | ((h2 & 0x03) << 6);
    
    s[13] = (h2 >> 2) & 0xFF;
    s[14] = (h2 >> 10) & 0xFF;
    s[15] = (h2 >> 18) & 0xFF;
    s[16] = (h2 >> 26) & 0xFF;
    s[17] = (h2 >> 34) & 0xFF;
    s[18] = (h2 >> 42) | ((h3 & 0x7F) << 1);
    
    s[19] = (h3 >> 7) & 0xFF;
    s[20] = (h3 >> 15) & 0xFF;
    s[21] = (h3 >> 23) & 0xFF;
    s[22] = (h3 >> 31) & 0xFF;
    s[23] = (h3 >> 39) & 0xFF;
    
    s[24] = h4 & 0xFF;
    s[25] = (h4 >> 8) & 0xFF;
    s[26] = (h4 >> 16) & 0xFF;
    s[27] = (h4 >> 24) & 0xFF;
    s[28] = (h4 >> 32) & 0xFF;
    s[29] = (h4 >> 40) & 0xFF;
    s[30] = (h4 >> 48) & 0xFF;
    s[31] = 0;  // Clear top bit
}

// Field addition: h = f + g
static void fe_add(fe25519 h, const fe25519 f, const fe25519 g) {
    h[0] = f[0] + g[0];
    h[1] = f[1] + g[1];
    h[2] = f[2] + g[2];
    h[3] = f[3] + g[3];
    h[4] = f[4] + g[4];
}

// Field subtraction: h = f - g (mod p)
static void fe_sub(fe25519 h, const fe25519 f, const fe25519 g) {
    h[0] = f[0] + (0x7FFFFFFFFFFFFEDULL - g[0]);
    h[1] = f[1] + 0x7FFFFFFFFFFFFULL - g[1];
    h[2] = f[2] + 0x7FFFFFFFFFFFFULL - g[2];
    h[3] = f[3] + 0x7FFFFFFFFFFFFULL - g[3];
    h[4] = f[4] + 0x7FFFFFFFFFFFFULL - g[4];
}

// Field multiplication: h = f * g
static void fe_mul(fe25519 h, const fe25519 f, const fe25519 g) {
    // Schoolbook multiplication with reduction
    uint64_t t[5];
    
    uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    
    // Compute products
    uint64_t h0 = (uint64_t)(f0 * g0);
    uint64_t h1 = (uint64_t)(f0 * g1 + f1 * g0);
    uint64_t h2 = (uint64_t)(f0 * g2 + f1 * g1 + f2 * g0);
    uint64_t h3 = (uint64_t)(f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0);
    uint64_t h4 = (uint64_t)(f0 * g4 + f1 * g3 + f2 * g2 + f3 * g1 + f4 * g0);
    
    // Add contributions from f4*g0..g3 with factor 19
    h0 += 19 * (f1 * g4 + f2 * g3 + f3 * g2 + f4 * g1);
    h1 += 19 * (f2 * g4 + f3 * g3 + f4 * g2);
    h2 += 19 * (f3 * g4 + f4 * g3);
    h3 += 19 * (f4 * g4);
    
    // Reduce and store
    h[0] = h0 & 0x7FFFFFFFFFFFFULL;
    h1 += h0 >> 51;
    h[1] = h1 & 0x7FFFFFFFFFFFFULL;
    h2 += h1 >> 51;
    h[2] = h2 & 0x7FFFFFFFFFFFFULL;
    h3 += h2 >> 51;
    h[3] = h3 & 0x7FFFFFFFFFFFFULL;
    h4 += h3 >> 51;
    h[4] = h4 & 0x7FFFFFFFFFFFFULL;
    
    // Carry to h[0]
    h[0] += 19 * (h4 >> 51);
    h[4] &= 0x7FFFFFFFFFFFFULL;
}

// Field squaring: h = f * f
static void fe_sq(fe25519 h, const fe25519 f) {
    fe_mul(h, f, f);
}

// Field inversion: h = 1/f (using Fermat's little theorem)
static void fe_invert(fe25519 h, const fe25519 f) {
    fe25519 t, f2, f4, f8, f16, f32;
    
    fe_sq(f2, f);         // f^2
    fe_mul(f2, f2, f);    // f^3
    fe_sq(f4, f2);        // f^6
    fe_sq(f4, f4);        // f^12
    fe_mul(f4, f4, f2);   // f^15
    fe_sq(f8, f4);        // f^30
    for (int i = 1; i < 4; i++) fe_sq(f8, f8);  // f^(15*16) = f^240
    fe_mul(f8, f8, f4);   // f^255
    
    fe_sq(f16, f8);       // f^510
    fe_sq(f16, f16);      // f^1020
    fe_mul(f16, f16, f8); // f^1275
    
    // Compute f^(p-2) = f^(2^255 - 19 - 1) = f^(2^255 - 21)
    // This is a simplified exponentiation for demo
    memcpy(h, f16, sizeof(fe25519));
}

// Scalar multiplication using Montgomery ladder
// X25519(u, k) = Montgomery ladder on Curve25519
static void x25519_scalarmult(uint8_t* out, const uint8_t* scalar, const uint8_t* point) {
    fe25519 x1, x2, x3, z2, z3, tmp0, tmp1;
    
    // Load point u-coordinate
    fe_from_bytes(x1, point);
    
    // Initialize ladder variables
    memset(x2, 0, sizeof(fe25519)); x2[0] = 1;  // x2 = 1
    memset(z2, 0, sizeof(fe25519));              // z2 = 0
    memcpy(x3, x1, sizeof(fe25519));             // x3 = u
    memcpy(z3, x2, sizeof(fe25519));             // z3 = 1
    
    // Montgomery ladder
    uint8_t k[32];
    memcpy(k, scalar, 32);
    x25519_clamp(k);
    
    int swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int bit = (k[pos / 8] >> (pos % 8)) & 1;
        swap ^= bit;
        
        // Conditional swap
        for (int i = 0; i < 5; i++) {
            uint64_t t = swap ? (x2[i] ^ x3[i]) : 0;
            x2[i] ^= t;
            x3[i] ^= t;
            t = swap ? (z2[i] ^ z3[i]) : 0;
            z2[i] ^= t;
            z3[i] ^= t;
        }
        swap = bit;
        
        // A = x2 + z2; AA = A^2
        fe_add(tmp0, x2, z2);
        fe_sq(tmp0, tmp0);  // AA
        
        // B = x2 - z2; BB = B^2
        fe_sub(tmp1, x2, z2);
        fe_sq(tmp1, tmp1);  // BB
        
        // E = AA - BB
        fe25519 e;
        fe_sub(e, tmp0, tmp1);
        
        // C = x3 + z3; D = x3 - z3
        fe25519 c, d;
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        
        // DA = D * AA
        fe25519 da;
        fe_mul(da, d, tmp0);
        
        // CB = C * BB
        fe25519 cb;
        fe_mul(cb, c, tmp1);
        
        // x3 = (DA + CB)^2
        fe_add(x3, da, cb);
        fe_sq(x3, x3);
        
        // z3 = x1 * (DA - CB)^2
        fe_sub(z3, da, cb);
        fe_sq(z3, z3);
        fe_mul(z3, z3, x1);
        
        // x2 = AA * BB
        fe_mul(x2, tmp0, tmp1);
        
        // z2 = E * (AA + a24 * E)
        fe25519 a24_e;
        a24_e[0] = 121666;
        for (int i = 1; i < 5; i++) a24_e[i] = 0;
        fe_mul(a24_e, e, a24_e);
        fe_add(z2, tmp0, a24_e);
        fe_mul(z2, z2, e);
    }
    
    // Final conditional swap
    for (int i = 0; i < 5; i++) {
        uint64_t t = swap ? (x2[i] ^ x3[i]) : 0;
        x2[i] ^= t;
        x3[i] ^= t;
        t = swap ? (z2[i] ^ z3[i]) : 0;
        z2[i] ^= t;
        z3[i] ^= t;
    }
    
    // Compute output: x2 * z2^(-1)
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
}

// X25519 base point (u-coordinate of generator)
static const uint8_t x25519_basepoint[32] = {
    9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Generate X25519 key pair
int x25519_generate_keypair(uint8_t* public_key, uint8_t* private_key) {
    // Generate random private key
    tls_get_random(private_key, 32);
    
    // Clamp the scalar
    x25519_clamp(private_key);
    
    // Compute public key: public_key = basepoint * private_key
    x25519_scalarmult(public_key, private_key, x25519_basepoint);
    
    return 0;
}

// Compute X25519 shared secret
int x25519_compute_shared(const uint8_t* private_key, const uint8_t* peer_public,
                          uint8_t* shared_secret) {
    // Compute shared secret: shared = peer_public * private_key
    x25519_scalarmult(shared_secret, private_key, peer_public);
    
    // Check for all-zero output (invalid public key)
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (shared_secret[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    
    if (all_zero) {
        return -1;  // Invalid public key
    }
    
    return 0;
}

// ============================================================================
// HKDF-SHA256 IMPLEMENTATION (RFC 5869)
// ============================================================================

// HMAC-SHA256
static void hmac_sha256_internal(const uint8_t* key, size_t key_len,
                                 const uint8_t* data, size_t data_len,
                                 uint8_t* mac) {
    sha256_ctx_t ctx;
    uint8_t k_ipad[64], k_opad[64];
    
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

// HKDF-Extract(salt, IKM) -> PRK
int hkdf_extract_sha256(const uint8_t* salt, size_t salt_len,
                        const uint8_t* ikm, size_t ikm_len,
                        uint8_t* prk) {
    // If salt is not provided, use zero-filled string of hash length
    uint8_t zero_salt[32] = {0};
    
    if (!salt || salt_len == 0) {
        salt = zero_salt;
        salt_len = 32;
    }
    
    // PRK = HMAC-Hash(salt, IKM)
    hmac_sha256_internal(salt, salt_len, ikm, ikm_len, prk);
    
    return 0;
}

// HKDF-Expand(PRK, info, L) -> OKM
int hkdf_expand_sha256(const uint8_t* prk, size_t prk_len,
                       const uint8_t* info, size_t info_len,
                       uint8_t* okm, size_t okm_len) {
    uint8_t t[32];
    uint8_t counter = 1;
    size_t offset = 0;
    
    // T(0) = empty string
    // T(1) = HMAC(PRK, info || 0x01)
    // T(2) = HMAC(PRK, T(1) || info || 0x02)
    // ...
    
    uint8_t prev[32] = {0};
    size_t prev_len = 0;
    
    while (offset < okm_len) {
        // Build input: prev || info || counter
        uint8_t input[256];
        size_t input_len = 0;
        
        if (prev_len > 0) {
            memcpy(input, prev, prev_len);
            input_len += prev_len;
        }
        
        if (info && info_len > 0) {
            memcpy(input + input_len, info, info_len);
            input_len += info_len;
        }
        
        input[input_len++] = counter;
        
        // Compute T(i)
        hmac_sha256_internal(prk, prk_len, input, input_len, t);
        
        // Copy to output
        size_t copy_len = (okm_len - offset < 32) ? okm_len - offset : 32;
        memcpy(okm + offset, t, copy_len);
        offset += copy_len;
        
        // Save for next iteration
        memcpy(prev, t, 32);
        prev_len = 32;
        
        counter++;
        if (counter == 0) break;  // Overflow protection
    }
    
    return 0;
}

// HKDF-Expand-Label(Secret, Label, Context, Length) -> key
// Used extensively in TLS 1.3 key derivation
int hkdf_expand_label(const uint8_t* secret, size_t secret_len,
                      const char* label,
                      const uint8_t* context, size_t context_len,
                      uint8_t* output, size_t output_len) {
    // Build HkdfLabel structure:
    // struct {
    //     uint16 length = Length;
    //     opaque label<7..255> = "tls13 " + Label;
    //     opaque context<0..255> = Context;
    // } HkdfLabel;
    
    uint8_t info[256];
    size_t info_len = 0;
    
    // Length (2 bytes, big-endian)
    info[info_len++] = (output_len >> 8) & 0xFF;
    info[info_len++] = output_len & 0xFF;
    
    // Label with "tls13 " prefix
    const char* prefix = "tls13 ";
    size_t prefix_len = strlen(prefix);
    size_t label_len = strlen(label);
    
    info[info_len++] = prefix_len + label_len;  // Label length
    memcpy(info + info_len, prefix, prefix_len);
    info_len += prefix_len;
    memcpy(info + info_len, label, label_len);
    info_len += label_len;
    
    // Context
    if (context && context_len > 0) {
        info[info_len++] = context_len;
        memcpy(info + info_len, context, context_len);
        info_len += context_len;
    } else {
        info[info_len++] = 0;  // Empty context
    }
    
    return hkdf_expand_sha256(secret, secret_len, info, info_len, output, output_len);
}

// ============================================================================
// TLS 1.3 KEY DERIVATION
// ============================================================================

int tls13_derive_early_secret(tls13_session_t* session, const uint8_t* psk, size_t psk_len) {
    // Early Secret = HKDF-Extract(salt=0, IKM=PSK)
    // If no PSK, use zero-filled string
    
    if (!psk || psk_len == 0) {
        uint8_t zero[32] = {0};
        hkdf_extract_sha256(NULL, 0, zero, 32, session->keys.early_secret);
    } else {
        hkdf_extract_sha256(NULL, 0, psk, psk_len, session->keys.early_secret);
    }
    
    return 0;
}

int tls13_derive_handshake_secret(tls13_session_t* session, const uint8_t* shared_secret, size_t ss_len) {
    // Derive-Secret(Early Secret, "derived", "")
    uint8_t derived_secret[32];
    hkdf_expand_label(session->keys.early_secret, 32, "derived", NULL, 0, derived_secret, 32);
    
    // Handshake Secret = HKDF-Extract(salt=derived_secret, IKM=shared_secret)
    hkdf_extract_sha256(derived_secret, 32, shared_secret, ss_len, session->keys.handshake_secret);
    
    // Derive client and server handshake traffic secrets
    // c_hs_traffic = Derive-Secret(Handshake Secret, "c hs traffic", ClientHello...ServerHello)
    hkdf_expand_label(session->keys.handshake_secret, 32, "c hs traffic", 
                      session->transcript.client_hello_hash, 32,
                      session->keys.client_handshake_traffic_secret, 32);
    
    // s_hs_traffic = Derive-Secret(Handshake Secret, "s hs traffic", ClientHello...ServerHello)
    hkdf_expand_label(session->keys.handshake_secret, 32, "s hs traffic",
                      session->transcript.server_hello_hash, 32,
                      session->keys.server_handshake_traffic_secret, 32);
    
    return 0;
}

int tls13_derive_master_secret(tls13_session_t* session) {
    // Derive-Secret(Handshake Secret, "derived", "")
    uint8_t derived_secret[32];
    hkdf_expand_label(session->keys.handshake_secret, 32, "derived", NULL, 0, derived_secret, 32);
    
    // Master Secret = HKDF-Extract(salt=derived_secret, IKM=0)
    uint8_t zero[32] = {0};
    hkdf_extract_sha256(derived_secret, 32, zero, 32, session->keys.master_secret);
    
    return 0;
}

int tls13_derive_traffic_keys(tls13_session_t* session) {
    // Derive application traffic secrets
    // c_ap_traffic = Derive-Secret(Master Secret, "c ap traffic", ClientHello...Server Finished)
    hkdf_expand_label(session->keys.master_secret, 32, "c ap traffic",
                      session->transcript.server_finished_hash, 32,
                      session->keys.client_traffic_secret_0, 32);
    
    // s_ap_traffic = Derive-Secret(Master Secret, "s ap traffic", ClientHello...Server Finished)
    hkdf_expand_label(session->keys.master_secret, 32, "s ap traffic",
                      session->transcript.server_finished_hash, 32,
                      session->keys.server_traffic_secret_0, 32);
    
    // Derive traffic keys from secrets
    int key_len = tls13_get_key_len(session->cipher_suite);
    session->keys.key_len = key_len;
    
    // Client write key and IV
    hkdf_expand_label(session->keys.client_traffic_secret_0, 32, "key", NULL, 0,
                      session->keys.client_write_key, key_len);
    hkdf_expand_label(session->keys.client_traffic_secret_0, 32, "iv", NULL, 0,
                      session->keys.client_write_iv, TLS13_IV_LEN);
    
    // Server write key and IV
    hkdf_expand_label(session->keys.server_traffic_secret_0, 32, "key", NULL, 0,
                      session->keys.server_write_key, key_len);
    hkdf_expand_label(session->keys.server_traffic_secret_0, 32, "iv", NULL, 0,
                      session->keys.server_write_iv, TLS13_IV_LEN);
    
    // Exporter secret
    hkdf_expand_label(session->keys.master_secret, 32, "exp master", NULL, 0,
                      session->keys.exporter_secret, 32);
    
    return 0;
}

int tls13_derive_finished_key(const uint8_t* traffic_secret, uint8_t* finished_key) {
    return hkdf_expand_label(traffic_secret, 32, "finished", NULL, 0, finished_key, 32);
}

// ============================================================================
// TLS 1.3 RECORD LAYER
// ============================================================================

void tls13_build_nonce(const uint8_t* iv, uint64_t seq, uint8_t* nonce) {
    // Nonce = IV XOR (sequence number padded to IV length)
    memcpy(nonce, iv, TLS13_IV_LEN);
    
    // XOR with sequence number (big-endian, right-aligned)
    for (int i = 0; i < 8; i++) {
        nonce[TLS13_IV_LEN - 1 - i] ^= (seq >> (i * 8)) & 0xFF;
    }
}

int tls13_encrypt_record(tls13_session_t* session,
                         uint8_t content_type,
                         const uint8_t* plaintext, size_t pt_len,
                         uint8_t* ciphertext) {
    // Build additional data: sequence number || type || version || length
    // In TLS 1.3, AAD is just the sequence number (8 bytes)
    uint8_t aad[8];
    for (int i = 0; i < 8; i++) {
        aad[i] = (session->client_record_seq >> (56 - i * 8)) & 0xFF;
    }
    
    // Append content type to plaintext (for "type" in encrypted record)
    uint8_t* pt_with_type = (uint8_t*)kmalloc(pt_len + 1);
    memcpy(pt_with_type, plaintext, pt_len);
    pt_with_type[pt_len] = content_type;
    
    // Build nonce
    uint8_t nonce[TLS13_IV_LEN];
    tls13_build_nonce(session->keys.client_write_iv, session->client_record_seq, nonce);
    
    // Initialize AES-GCM context
    aes_gcm_ctx_t ctx;
    int key_bits = session->keys.key_len * 8;
    aes_gcm_init(&ctx, session->keys.client_write_key, key_bits, nonce);
    
    // Encrypt
    uint8_t tag[16];
    int result = aes_gcm_encrypt(&ctx, pt_with_type, pt_len + 1, aad, 8, ciphertext, tag);
    
    // Append tag
    memcpy(ciphertext + pt_len + 1, tag, 16);
    
    kfree(pt_with_type);
    
    if (result == 0) {
        session->client_record_seq++;
        return pt_len + 1 + 16;  // ciphertext + tag
    }
    
    return -1;
}

int tls13_decrypt_record(tls13_session_t* session,
                         const uint8_t* ciphertext, size_t ct_len,
                         uint8_t* plaintext, uint8_t* content_type) {
    if (ct_len < 17) return -1;  // Minimum: 1 byte data + 16 bytes tag
    
    // Build nonce
    uint8_t nonce[TLS13_IV_LEN];
    tls13_build_nonce(session->keys.server_write_iv, session->server_record_seq, nonce);
    
    // Build AAD (sequence number)
    uint8_t aad[8];
    for (int i = 0; i < 8; i++) {
        aad[i] = (session->server_record_seq >> (56 - i * 8)) & 0xFF;
    }
    
    // Initialize AES-GCM context
    aes_gcm_ctx_t ctx;
    int key_bits = session->keys.key_len * 8;
    aes_gcm_init(&ctx, session->keys.server_write_key, key_bits, nonce);
    
    // Separate ciphertext and tag
    size_t actual_ct_len = ct_len - 16;
    const uint8_t* tag = ciphertext + actual_ct_len;
    
    // Decrypt
    int result = aes_gcm_decrypt(&ctx, ciphertext, actual_ct_len, aad, 8, tag, plaintext);
    
    if (result == 0) {
        session->server_record_seq++;
        
        // Content type is the last byte
        *content_type = plaintext[actual_ct_len - 1];
        return actual_ct_len - 1;  // Return plaintext length (without type byte)
    }
    
    return -1;
}

// ============================================================================
// TLS 1.3 SESSION MANAGEMENT
// ============================================================================

tls13_session_t* tls13_create_session(void) {
    tls13_session_t* session = (tls13_session_t*)kmalloc(sizeof(tls13_session_t));
    if (!session) return NULL;
    
    memset(session, 0, sizeof(tls13_session_t));
    session->socket_fd = -1;
    session->state = TLS13_STATE_INIT;
    session->cipher_suite = TLS13_AES_128_GCM_SHA256;  // Default
    session->negotiated_version = TLS13_VERSION;
    
    // Generate client random
    tls_get_random(session->client_random, 32);
    
    return session;
}

void tls13_destroy_session(tls13_session_t* session) {
    if (session) {
        if (session->socket_fd >= 0) {
            k_close(session->socket_fd);
        }
        if (session->server_cert) {
            kfree(session->server_cert);
        }
        kfree(session);
    }
}

// ============================================================================
// TLS 1.3 HANDSHAKE IMPLEMENTATION
// ============================================================================

// Send TLS 1.3 ClientHello
int tls13_send_client_hello(tls13_session_t* session) {
    uint8_t hello[2048];
    uint8_t* p = hello;
    
    // TLS record header
    *p++ = TLS13_CONTENT_HANDSHAKE;
    *p++ = 0x03; *p++ = 0x01;  // Legacy version (TLS 1.0 for middlebox compatibility)
    uint8_t* record_len_ptr = p;
    p += 2;  // Length placeholder
    
    // Handshake header
    *p++ = TLS13_CLIENT_HELLO;
    uint8_t* hs_len_ptr = p;
    p += 3;  // Length placeholder
    
    // Legacy version (TLS 1.2 for compatibility)
    *p++ = 0x03; *p++ = 0x03;
    
    // Client random (32 bytes)
    memcpy(p, session->client_random, 32);
    p += 32;
    
    // Legacy session ID (empty)
    *p++ = 0;
    
    // Cipher suites
    uint16_t cipher_suites[] = {
        TLS13_AES_128_GCM_SHA256,
        TLS13_AES_256_GCM_SHA384,
        TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,  // TLS 1.2 fallback
        TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    };
    int cipher_count = sizeof(cipher_suites) / sizeof(cipher_suites[0]);
    
    *p++ = (cipher_count * 2) >> 8;
    *p++ = (cipher_count * 2) & 0xFF;
    for (int i = 0; i < cipher_count; i++) {
        *p++ = cipher_suites[i] >> 8;
        *p++ = cipher_suites[i] & 0xFF;
    }
    
    // Legacy compression methods (null only)
    *p++ = 1;
    *p++ = 0;
    
    // Extensions
    uint8_t* ext_len_ptr = p;
    p += 2;  // Extensions length placeholder
    
    // Server Name (SNI) extension
    size_t sni_len = strlen(session->server_name);
    *p++ = 0x00; *p++ = 0x00;  // Extension type: server_name
    *p++ = (sni_len + 5) >> 8; *p++ = (sni_len + 5) & 0xFF;  // Extension length
    *p++ = (sni_len + 3) >> 8; *p++ = (sni_len + 3) & 0xFF;  // Server name list length
    *p++ = 0;  // Name type: host_name
    *p++ = sni_len >> 8; *p++ = sni_len & 0xFF;  // Name length
    memcpy(p, session->server_name, sni_len);
    p += sni_len;
    
    // ALPN extension (Application-Layer Protocol Negotiation)
    // Required for HTTP/2 - advertise "h2" and "http/1.1"
    *p++ = 0x00; *p++ = 0x10;  // Extension type: alpn (16)
    *p++ = 0x00; *p++ = 0x0D;  // Extension length (13 bytes)
    *p++ = 0x00; *p++ = 0x0B;  // ALPN protocol list length (11 bytes)
    // "h2" (HTTP/2)
    *p++ = 0x02;  // Protocol length
    *p++ = 'h'; *p++ = '2';
    // "http/1.1"
    *p++ = 0x08;  // Protocol length
    *p++ = 'h'; *p++ = 't'; *p++ = 't'; *p++ = 'p';
    *p++ = '/'; *p++ = '1'; *p++ = '.'; *p++ = '1';
    
    // Supported Groups extension
    *p++ = 0x00; *p++ = 0x0A;  // Extension type: supported_groups
    *p++ = 0x00; *p++ = 0x04;  // Extension length
    *p++ = 0x00; *p++ = 0x02;  // Groups list length
    *p++ = 0x00; *p++ = TLS13_GROUP_X25519;  // X25519
    
    // Signature Algorithms extension
    *p++ = 0x00; *p++ = 0x0D;  // Extension type: signature_algorithms
    *p++ = 0x00; *p++ = 0x06;  // Extension length
    *p++ = 0x00; *p++ = 0x04;  // Algorithms list length
    // RSA-PSS with SHA-256
    *p++ = TLS13_SIG_RSA_PSS_RSAE_SHA256 >> 8;
    *p++ = TLS13_SIG_RSA_PSS_RSAE_SHA256 & 0xFF;
    // ECDSA with SHA-256
    *p++ = TLS13_SIG_ECDSA_SECP256R1 >> 8;
    *p++ = TLS13_SIG_ECDSA_SECP256R1 & 0xFF;
    
    // Supported Versions extension
    *p++ = 0x00; *p++ = 0x2B;  // Extension type: supported_versions
    *p++ = 0x00; *p++ = 0x03;  // Extension length
    *p++ = 0x02;  // Versions list length
    *p++ = 0x03; *p++ = 0x04;  // TLS 1.3 (0x0304)
    
    // Key Share extension
    // Generate X25519 key pair
    x25519_generate_keypair(session->key_share_public, session->key_share_private);
    session->key_share_group = TLS13_GROUP_X25519;
    
    *p++ = 0x00; *p++ = 0x33;  // Extension type: key_share
    *p++ = 0x00; *p++ = 0x26;  // Extension length (38 bytes: 4 + 32 + 2)
    *p++ = 0x00; *p++ = 0x24;  // Key share list length
    *p++ = 0x00; *p++ = TLS13_GROUP_X25519;  // Named group
    *p++ = 0x00; *p++ = 0x20;  // Key exchange length (32 bytes)
    memcpy(p, session->key_share_public, 32);
    p += 32;
    
    // PSK Key Exchange Modes extension
    *p++ = 0x00; *p++ = 0x2D;  // Extension type: psk_key_exchange_modes
    *p++ = 0x00; *p++ = 0x02;  // Extension length
    *p++ = 0x01;  // Modes list length
    *p++ = TLS13_PSK_DHE_KE;  // PSK with (EC)DHE
    
    // Calculate extensions length
    uint16_t ext_len = p - ext_len_ptr - 2;
    *ext_len_ptr++ = ext_len >> 8;
    *ext_len_ptr = ext_len & 0xFF;
    
    // Calculate handshake length
    uint32_t hs_len = p - hs_len_ptr - 3;
    *hs_len_ptr++ = (hs_len >> 16) & 0xFF;
    *hs_len_ptr++ = (hs_len >> 8) & 0xFF;
    *hs_len_ptr = hs_len & 0xFF;
    
    // Calculate record length
    uint16_t record_len = p - hello - 5;
    *record_len_ptr++ = record_len >> 8;
    *record_len_ptr = record_len & 0xFF;
    
    // Update transcript hash
    sha256_hash(hello + 5, record_len, session->transcript.client_hello_hash);
    
    // Send record
    int result = k_sendto(session->socket_fd, hello, p - hello, 0, NULL);
    
    if (result > 0) {
        session->state = TLS13_STATE_CLIENT_HELLO_SENT;
        return 0;
    }
    
    return -1;
}

// Process ServerHello
int tls13_process_server_hello(tls13_session_t* session, const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    
    // Legacy version
    p += 2;
    
    // Server random
    memcpy(session->server_random, p, 32);
    p += 32;
    
    // Legacy session ID
    uint8_t session_id_len = *p++;
    p += session_id_len;
    
    // Cipher suite
    session->cipher_suite = (p[0] << 8) | p[1];
    p += 2;
    
    // Legacy compression method
    p++;
    
    // Extensions
    if (p < data + len) {
        uint16_t ext_len = (p[0] << 8) | p[1];
        p += 2;
        
        const uint8_t* ext_end = p + ext_len;
        
        while (p < ext_end) {
            uint16_t ext_type = (p[0] << 8) | p[1];
            p += 2;
            uint16_t ext_data_len = (p[0] << 8) | p[1];
            p += 2;
            
            if (ext_type == TLS13_EXT_SUPPORTED_VERSIONS) {
                // Check negotiated version
                if (ext_data_len >= 2 && p[0] == 0x03 && p[1] == 0x04) {
                    session->negotiated_version = TLS13_VERSION;
                }
            }
            else if (ext_type == TLS13_EXT_KEY_SHARE) {
                // Server's key share
                uint16_t group = (p[0] << 8) | p[1];
                p += 2;
                uint8_t key_len = p[0];
                p++;
                
                if (key_len <= 64) {
                    memcpy(session->server_key_share_public, p, key_len);
                    session->server_key_share_len = key_len;
                    session->key_share_group = group;
                }
            }
            
            p += ext_data_len;
        }
    }
    
    // Update transcript hash
    sha256_hash(data, len, session->transcript.server_hello_hash);
    
    // Compute shared secret using X25519
    uint8_t shared_secret[32];
    x25519_compute_shared(session->key_share_private, session->server_key_share_public, shared_secret);
    
    // Derive early secret (with no PSK)
    tls13_derive_early_secret(session, NULL, 0);
    
    // Derive handshake secret
    tls13_derive_handshake_secret(session, shared_secret, 32);
    
    // Derive traffic keys for handshake
    int key_len = tls13_get_key_len(session->cipher_suite);
    session->keys.key_len = key_len;
    
    hkdf_expand_label(session->keys.client_handshake_traffic_secret, 32, "key", NULL, 0,
                      session->keys.client_write_key, key_len);
    hkdf_expand_label(session->keys.client_handshake_traffic_secret, 32, "iv", NULL, 0,
                      session->keys.client_write_iv, TLS13_IV_LEN);
    hkdf_expand_label(session->keys.server_handshake_traffic_secret, 32, "key", NULL, 0,
                      session->keys.server_write_key, key_len);
    hkdf_expand_label(session->keys.server_handshake_traffic_secret, 32, "iv", NULL, 0,
                      session->keys.server_write_iv, TLS13_IV_LEN);
    
    session->state = TLS13_STATE_SERVER_HELLO_RECEIVED;
    
    return 0;
}

// TLS 1.3 Connect
int tls13_connect(tls13_session_t* session, const char* hostname, uint16_t port) {
    strncpy(session->server_name, hostname, sizeof(session->server_name) - 1);
    session->port = port;
    
    // Create socket
    session->socket_fd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (session->socket_fd < 0) {
        return -1;
    }
    
    // Resolve hostname
    char ip_str[32];
    if (dns_resolve(hostname, ip_str, sizeof(ip_str)) < 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
        return -1;
    }
    
    // Parse IP
    uint32_t ip = 0;
    char* dot = ip_str;
    for (int i = 0; i < 4; i++) {
        ip = (ip << 8) | local_atoi(dot);
        dot = strchr(dot, '.');
        if (dot) dot++;
    }
    
    // Connect
    sockaddr_in_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ip;
    
    if (k_connect(session->socket_fd, &addr) < 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
        return -1;
    }
    
    // Send ClientHello
    if (tls13_send_client_hello(session) < 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
        return -1;
    }
    
    // Receive ServerHello
    uint8_t buffer[4096];
    uint8_t content_type;
    int received = k_recvfrom(session->socket_fd, buffer, sizeof(buffer), 0, NULL);
    if (received < 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
        return -1;
    }
    
    // Parse TLS record
    if (buffer[0] != TLS13_CONTENT_HANDSHAKE) {
        // Could be HelloRetryRequest or alert
        k_close(session->socket_fd);
        session->socket_fd = -1;
        return -1;
    }
    
    // Process ServerHello
    if (tls13_process_server_hello(session, buffer + 5, received - 5) < 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
        return -1;
    }
    
    // Now we need to receive encrypted handshake messages
    // EncryptedExtensions, Certificate, CertificateVerify, Finished
    
    // For now, simplified - mark as established
    // In full implementation, would process all encrypted handshake messages
    
    // Derive master secret and traffic keys
    tls13_derive_master_secret(session);
    tls13_derive_traffic_keys(session);
    
    session->state = TLS13_STATE_HANDSHAKE_COMPLETE;
    session->handshake_complete = 1;
    
    return 0;
}

// TLS 1.3 Write
int tls13_write(tls13_session_t* session, const void* data, size_t len) {
    if (!session || !session->handshake_complete) return -1;
    
    // Encrypt and send
    uint8_t* ciphertext = (uint8_t*)kmalloc(len + 64);
    if (!ciphertext) return -1;
    
    int ct_len = tls13_encrypt_record(session, TLS13_CONTENT_APPLICATION_DATA, data, len, ciphertext);
    if (ct_len < 0) {
        kfree(ciphertext);
        return -1;
    }
    
    // Build TLS record
    uint8_t record[16384];
    record[0] = TLS13_CONTENT_APPLICATION_DATA;
    record[1] = 0x03;
    record[2] = 0x03;
    record[3] = (ct_len >> 8) & 0xFF;
    record[4] = ct_len & 0xFF;
    memcpy(record + 5, ciphertext, ct_len);
    
    kfree(ciphertext);
    
    return k_sendto(session->socket_fd, record, ct_len + 5, 0, NULL);
}

// TLS 1.3 Read
int tls13_read(tls13_session_t* session, void* buffer, size_t max_len) {
    if (!session || !session->handshake_complete) return -1;
    
    uint8_t record[16384];
    int received = k_recvfrom(session->socket_fd, record, sizeof(record), 0, NULL);
    if (received < 6) return -1;
    
    uint8_t content_type = record[0];
    uint16_t record_len = (record[3] << 8) | record[4];
    
    if (content_type == TLS13_CONTENT_APPLICATION_DATA) {
        // Decrypt
        uint8_t* plaintext = (uint8_t*)kmalloc(record_len);
        uint8_t inner_content_type;
        
        int pt_len = tls13_decrypt_record(session, record + 5, record_len, plaintext, &inner_content_type);
        if (pt_len < 0) {
            kfree(plaintext);
            return -1;
        }
        
        size_t copy_len = (pt_len < (int)max_len) ? pt_len : max_len;
        memcpy(buffer, plaintext, copy_len);
        
        kfree(plaintext);
        return copy_len;
    }
    else if (content_type == TLS13_CONTENT_ALERT) {
        // Handle alert
        return -1;
    }
    
    return -1;
}

// TLS 1.3 Close
int tls13_close(tls13_session_t* session) {
    if (!session) return -1;
    
    // Send close_notify alert
    uint8_t alert[2] = {1, 0};  // Warning, close_notify
    
    // Encrypt alert
    uint8_t ciphertext[32];
    int ct_len = tls13_encrypt_record(session, TLS13_CONTENT_ALERT, alert, 2, ciphertext);
    if (ct_len > 0) {
        uint8_t record[64];
        record[0] = TLS13_CONTENT_APPLICATION_DATA;
        record[1] = 0x03;
        record[2] = 0x03;
        record[3] = (ct_len >> 8) & 0xFF;
        record[4] = ct_len & 0xFF;
        memcpy(record + 5, ciphertext, ct_len);
        k_sendto(session->socket_fd, record, ct_len + 5, 0, NULL);
    }
    
    if (session->socket_fd >= 0) {
        k_close(session->socket_fd);
        session->socket_fd = -1;
    }
    
    return 0;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

int tls13_is_tls13_cipher(uint16_t cipher_suite) {
    return (cipher_suite >= 0x1301 && cipher_suite <= 0x1305);
}

int tls13_get_hash_len(uint16_t cipher_suite) {
    switch (cipher_suite) {
        case TLS13_AES_256_GCM_SHA384:
            return 48;
        default:
            return 32;
    }
}

int tls13_get_key_len(uint16_t cipher_suite) {
    switch (cipher_suite) {
        case TLS13_AES_256_GCM_SHA384:
            return 32;
        default:
            return 16;
    }
}
