// core/crypto_selftests.c - simple self-tests for crypto primitives

#include "tls13.h"
#include "tls.h"
#include "sha256.h"
#include "string.h"

extern void s_printf(const char* fmt, ...);

int aes_gcm_self_test(void) {
    aes_gcm_ctx_t ctx;
    uint8_t key[16] = {0};
    uint8_t iv[12] = {0};
    uint8_t pt[16] = {0};
    uint8_t ct[16];
    uint8_t tag[16];
    const uint8_t expected_ct[16] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,
        0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78
    };
    const uint8_t expected_tag[16] = {
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
        0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };

    if (aes_gcm_init(&ctx, key, 128, iv) != 0) {
        s_printf("[AES-GCM] init failed\n");
        return -1;
    }

    if (aes_gcm_encrypt(&ctx, pt, 16, NULL, 0, ct, tag) != 0) {
        s_printf("[AES-GCM] encrypt failed\n");
        return -1;
    }

    if (memcmp(ct, expected_ct, 16) != 0) {
        s_printf("[AES-GCM] CT mismatch\n");
        return -1;
    }

    if (memcmp(tag, expected_tag, 16) != 0) {
        s_printf("[AES-GCM] TAG mismatch\n");
        return -1;
    }

    s_printf("[AES-GCM] Self-test PASSED\n");
    return 0;
}

int sha256_self_test(void) {
    uint8_t out[32];
    const uint8_t expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };

    sha256_hash((const uint8_t*)"abc", 3, out);
    if (memcmp(out, expected, 32) != 0) {
        s_printf("[SHA256] Self-test FAILED\n");
        return -1;
    }
    s_printf("[SHA256] Self-test PASSED\n");
    return 0;
}
