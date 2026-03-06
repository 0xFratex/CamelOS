// core/tls.h - TLS 1.2+ Protocol Implementation
// Supports: TLS 1.2, TLS 1.3 (partial)
// Cryptography: RSA, AES-128/256-GCM, SHA-256/384, ECDHE
#ifndef TLS_H
#define TLS_H

#include "../include/types.h"

// ============================================================================
// TLS VERSION CONSTANTS
// ============================================================================
#define TLS_VERSION_1_0     0x0301
#define TLS_VERSION_1_1     0x0302
#define TLS_VERSION_1_2     0x0303
#define TLS_VERSION_1_3     0x0304

#define TLS_MIN_VERSION     TLS_VERSION_1_2
#define TLS_MAX_VERSION     TLS_VERSION_1_3

// ============================================================================
// TLS CONTENT TYPES
// ============================================================================
#define TLS_CONTENT_CHANGE_CIPHER_SPEC  20
#define TLS_CONTENT_ALERT              21
#define TLS_CONTENT_HANDSHAKE          22
#define TLS_CONTENT_APPLICATION_DATA   23

// ============================================================================
// TLS HANDSHAKE TYPES
// ============================================================================
#define TLS_HANDSHAKE_HELLO_REQUEST        0
#define TLS_HANDSHAKE_CLIENT_HELLO         1
#define TLS_HANDSHAKE_SERVER_HELLO         2
#define TLS_HANDSHAKE_NEW_SESSION_TICKET   4
#define TLS_HANDSHAKE_CERTIFICATE          11
#define TLS_HANDSHAKE_SERVER_KEY_EXCHANGE  12
#define TLS_HANDSHAKE_CERTIFICATE_REQUEST  13
#define TLS_HANDSHAKE_SERVER_HELLO_DONE    14
#define TLS_HANDSHAKE_CERTIFICATE_VERIFY   15
#define TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE  16
#define TLS_HANDSHAKE_FINISHED             20

// ============================================================================
// TLS CIPHER SUITES
// ============================================================================
// TLS 1.2 cipher suites
#define TLS_RSA_WITH_AES_128_CBC_SHA256       0x003C
#define TLS_RSA_WITH_AES_256_CBC_SHA256       0x003D
#define TLS_RSA_WITH_AES_128_GCM_SHA256       0x009C
#define TLS_RSA_WITH_AES_256_GCM_SHA384       0x009D
#define TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 0xC02F
#define TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 0xC030
#define TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 0xC02B
#define TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 0xC02C

// TLS 1.3 cipher suites
#define TLS_AES_128_GCM_SHA256               0x1301
#define TLS_AES_256_GCM_SHA384               0x1302
#define TLS_CHACHA20_POLY1305_SHA256         0x1303

// ============================================================================
// TLS ALERT LEVELS AND DESCRIPTIONS
// ============================================================================
#define TLS_ALERT_LEVEL_WARNING     1
#define TLS_ALERT_LEVEL_FATAL       2

#define TLS_ALERT_CLOSE_NOTIFY              0
#define TLS_ALERT_UNEXPECTED_MESSAGE       10
#define TLS_ALERT_BAD_RECORD_MAC           20
#define TLS_ALERT_DECRYPTION_FAILED        21
#define TLS_ALERT_RECORD_OVERFLOW          22
#define TLS_ALERT_DECOMPRESSION_FAILURE    30
#define TLS_ALERT_HANDSHAKE_FAILURE        40
#define TLS_ALERT_NO_CERTIFICATE           41
#define TLS_ALERT_BAD_CERTIFICATE          42
#define TLS_ALERT_UNSUPPORTED_CERTIFICATE  43
#define TLS_ALERT_CERTIFICATE_REVOKED      44
#define TLS_ALERT_CERTIFICATE_EXPIRED      45
#define TLS_ALERT_CERTIFICATE_UNKNOWN      46
#define TLS_ALERT_ILLEGAL_PARAMETER        47
#define TLS_ALERT_UNKNOWN_CA               48
#define TLS_ALERT_ACCESS_DENIED            49
#define TLS_ALERT_DECODE_ERROR             50
#define TLS_ALERT_DECRYPT_ERROR            51
#define TLS_ALERT_PROTOCOL_VERSION         70
#define TLS_ALERT_INSUFFICIENT_SECURITY    71
#define TLS_ALERT_INTERNAL_ERROR           80
#define TLS_ALERT_USER_CANCELED            90
#define TLS_ALERT_NO_RENEGOTIATION        100

// ============================================================================
// TLS STATES
// ============================================================================
typedef enum {
    TLS_STATE_INIT,
    TLS_STATE_CONNECTING,
    TLS_STATE_HANDSHAKE_START,
    TLS_STATE_HELLO_SENT,
    TLS_STATE_HELLO_RECEIVED,
    TLS_STATE_CERTIFICATE_RECEIVED,
    TLS_STATE_KEY_EXCHANGE_RECEIVED,
    TLS_STATE_HELLO_DONE_RECEIVED,
    TLS_STATE_KEY_EXCHANGE_SENT,
    TLS_STATE_CHANGE_CIPHER_SENT,
    TLS_STATE_FINISHED_SENT,
    TLS_STATE_ESTABLISHED,
    TLS_STATE_CLOSED,
    TLS_STATE_ERROR
} tls_state_t;

// ============================================================================
// TLS ERROR CODES
// ============================================================================
typedef enum {
    TLS_OK = 0,
    TLS_ERR_SOCKET = -1,
    TLS_ERR_HANDSHAKE = -2,
    TLS_ERR_CERTIFICATE = -3,
    TLS_ERR_CIPHER = -4,
    TLS_ERR_MAC = -5,
    TLS_ERR_DECRYPT = -6,
    TLS_ERR_ENCRYPT = -7,
    TLS_ERR_PROTOCOL = -8,
    TLS_ERR_VERSION = -9,
    TLS_ERR_MEMORY = -10,
    TLS_ERR_TIMEOUT = -11,
    TLS_ERR_CERT_VERIFY = -12,
    TLS_ERR_SIGNATURE = -13,
    TLS_ERR_KEY_EXCHANGE = -14
} tls_error_t;

// ============================================================================
// X.509 CERTIFICATE STRUCTURE
// ============================================================================
#define TLS_MAX_CERT_SIZE     4096
#define TLS_MAX_CERT_CHAIN    4
#define TLS_MAX_CN_LENGTH     256
#define TLS_MAX_ORG_LENGTH    256

typedef struct {
    // Raw certificate data
    uint8_t* raw_data;
    uint32_t raw_len;
    
    // Parsed fields
    char common_name[TLS_MAX_CN_LENGTH];
    char organization[TLS_MAX_ORG_LENGTH];
    char issuer_cn[TLS_MAX_CN_LENGTH];
    char issuer_org[TLS_MAX_ORG_LENGTH];
    
    // Validity
    uint32_t not_before;  // Unix timestamp
    uint32_t not_after;   // Unix timestamp
    
    // Public key info
    uint8_t public_key[512];  // RSA up to 4096 bits
    uint16_t public_key_len;
    uint8_t public_key_type;  // 1=RSA, 2=ECDSA, 3=Ed25519
    
    // Signature
    uint8_t signature[512];
    uint16_t signature_len;
    uint8_t signature_alg;  // 1=SHA256withRSA, 2=SHA384withRSA, 3=ECDSA
    
    // Certificate fingerprint (SHA-256)
    uint8_t fingerprint[32];
    
    // Self-signed flag
    int is_self_signed;
    
    // Certificate chain index
    int chain_index;
} x509_cert_t;

// ============================================================================
// RSA KEY STRUCTURE
// ============================================================================
#define TLS_MAX_RSA_MODULUS_SIZE  512  // 4096 bits

typedef struct {
    uint8_t modulus[TLS_MAX_RSA_MODULUS_SIZE];
    uint16_t modulus_len;
    uint8_t exponent[8];
    uint8_t exponent_len;
    uint8_t private_exponent[TLS_MAX_RSA_MODULUS_SIZE];
    uint16_t private_exponent_len;
    uint8_t prime_p[256];
    uint8_t prime_q[256];
    uint16_t prime_len;
} rsa_key_t;

// ============================================================================
// ELLIPTIC CURVE STRUCTURES
// ============================================================================
#define TLS_MAX_EC_POINT_SIZE  133  // Uncompressed point for P-521

typedef enum {
    EC_CURVE_P256 = 23,   // secp256r1 (NIST P-256)
    EC_CURVE_P384 = 24,   // secp384r1 (NIST P-384)
    EC_CURVE_P521 = 25,   // secp521r1 (NIST P-521)
    EC_CURVE_X25519 = 29, // X25519 for key exchange
    EC_CURVE_X448 = 30    // X448 for key exchange
} ec_curve_type_t;

typedef struct {
    ec_curve_type_t curve;
    uint8_t public_key[TLS_MAX_EC_POINT_SIZE];
    uint16_t public_key_len;
    uint8_t private_key[66];  // Max private key size
    uint16_t private_key_len;
} ec_key_t;

// ============================================================================
// AES-GCM CONTEXT
// ============================================================================
#define TLS_GCM_IV_SIZE     12
#define TLS_GCM_TAG_SIZE    16
#define TLS_AES_BLOCK_SIZE  16

typedef struct {
    uint32_t key[60];    // Expanded key schedule (max for AES-256)
    uint8_t iv[TLS_GCM_IV_SIZE];
    uint8_t counter[TLS_AES_BLOCK_SIZE];
    uint8_t gcm_h[TLS_AES_BLOCK_SIZE];     // Hash subkey
    uint8_t gcm_j0[TLS_AES_BLOCK_SIZE];    // Pre-counter block
    uint8_t gcm_len_a[8];  // Length of AAD
    uint8_t gcm_len_c[8];  // Length of ciphertext
    int key_bits;          // 128 or 256
} aes_gcm_ctx_t;

// ============================================================================
// SHA-256/384 CONTEXT
// ============================================================================
#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32
#define SHA384_DIGEST_SIZE 48
#define SHA512_DIGEST_SIZE 64

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[SHA256_BLOCK_SIZE];
} sha256_ctx_t;

typedef struct {
    uint64_t state[8];
    uint64_t count_high;
    uint64_t count_low;
    uint8_t buffer[128];
} sha512_ctx_t;

// ============================================================================
// TLS SESSION CONTEXT
// ============================================================================
#define TLS_MAX_RANDOM        32
#define TLS_MAX_SESSION_ID    32
#define TLS_MAX_MASTER_SECRET 48
#define TLS_MAX_KEY_BLOCK     256

typedef struct tls_session {
    // Connection info
    int socket_fd;
    uint16_t version;
    tls_state_t state;
    tls_error_t last_error;
    
    // Server info
    char server_name[256];
    uint16_t port;
    
    // Handshake data
    uint8_t client_random[TLS_MAX_RANDOM];
    uint8_t server_random[TLS_MAX_RANDOM];
    uint8_t session_id[TLS_MAX_SESSION_ID];
    uint8_t session_id_len;
    
    // Cipher suite
    uint16_t cipher_suite;
    uint8_t cipher_key_size;
    uint8_t cipher_iv_size;
    uint8_t cipher_mac_size;
    
    // Key material
    uint8_t master_secret[TLS_MAX_MASTER_SECRET];
    uint8_t key_block[TLS_MAX_KEY_BLOCK];
    
    // Sequence numbers
    uint64_t read_seq_num;
    uint64_t write_seq_num;
    
    // Encryption contexts
    aes_gcm_ctx_t read_ctx;
    aes_gcm_ctx_t write_ctx;
    
    // MAC keys (for CBC mode)
    uint8_t read_mac_key[32];
    uint8_t write_mac_key[32];
    
    // IV for CBC mode
    uint8_t read_iv[16];
    uint8_t write_iv[16];
    
    // Certificate chain
    x509_cert_t cert_chain[TLS_MAX_CERT_CHAIN];
    int cert_count;
    
    // Server public key (RSA or EC)
    rsa_key_t server_rsa_key;
    ec_key_t server_ec_key;
    int server_key_type;  // 1=RSA, 2=ECDSA, 3=ECDHE
    
    // ECDHE key exchange
    ec_key_t ecdhe_key;
    
    // Handshake hash (for Finished message)
    sha256_ctx_t handshake_hash;
    uint8_t handshake_hash_val[SHA256_DIGEST_SIZE];
    
    // Flags
    int is_server;
    int verify_cert;
    int session_resumed;
    
    // Application data buffer
    uint8_t app_data[16384];
    int app_data_len;
    
    // Callbacks
    void (*on_alert)(int level, int desc, void* user_data);
    void (*on_cert_verify)(x509_cert_t* cert, void* user_data);
    void* callback_user_data;
    
} tls_session_t;

// ============================================================================
// TLS RECORD HEADER
// ============================================================================
typedef struct {
    uint8_t content_type;
    uint16_t version;
    uint16_t length;
} __attribute__((packed)) tls_record_header_t;

// ============================================================================
// TLS HANDSHAKE HEADER
// ============================================================================
typedef struct {
    uint8_t handshake_type;
    uint8_t length[3];  // 24-bit length
} __attribute__((packed)) tls_handshake_header_t;

// ============================================================================
// PUBLIC API FUNCTIONS
// ============================================================================

// Session management
tls_session_t* tls_create_session(void);
void tls_destroy_session(tls_session_t* session);
int tls_connect(tls_session_t* session, const char* hostname, uint16_t port);
int tls_close(tls_session_t* session);

// Data transfer
int tls_write(tls_session_t* session, const void* data, size_t len);
int tls_read(tls_session_t* session, void* buffer, size_t max_len);

// Configuration
void tls_set_verify(tls_session_t* session, int verify);
void tls_set_hostname(tls_session_t* session, const char* hostname);
void tls_set_callbacks(tls_session_t* session,
                       void (*on_alert)(int, int, void*),
                       void (*on_cert_verify)(x509_cert_t*, void*),
                       void* user_data);

// Certificate validation
int tls_verify_certificate(x509_cert_t* cert, const char* hostname);
int tls_verify_cert_chain(x509_cert_t* chain, int count, const char* hostname);

// ============================================================================
// CRYPTOGRAPHIC PRIMITIVES
// ============================================================================

// SHA-256
void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len);
void sha256_final(sha256_ctx_t* ctx, uint8_t* digest);
void sha256_hash(const uint8_t* data, size_t len, uint8_t* digest);

// SHA-384 (uses SHA-512 internally with different initial values)
void sha384_hash(const uint8_t* data, size_t len, uint8_t* digest);

// AES
void aes_set_key(aes_gcm_ctx_t* ctx, const uint8_t* key, int key_bits);
void aes_encrypt_block(aes_gcm_ctx_t* ctx, const uint8_t* input, uint8_t* output);
void aes_decrypt_block(aes_gcm_ctx_t* ctx, const uint8_t* input, uint8_t* output);

// AES-GCM
int aes_gcm_init(aes_gcm_ctx_t* ctx, const uint8_t* key, int key_bits, const uint8_t* iv);
int aes_gcm_encrypt(aes_gcm_ctx_t* ctx, const uint8_t* plaintext, size_t pt_len,
                    const uint8_t* aad, size_t aad_len,
                    uint8_t* ciphertext, uint8_t* tag);
int aes_gcm_decrypt(aes_gcm_ctx_t* ctx, const uint8_t* ciphertext, size_t ct_len,
                    const uint8_t* aad, size_t aad_len,
                    const uint8_t* tag, uint8_t* plaintext);

// RSA
int rsa_public_encrypt(rsa_key_t* key, const uint8_t* plaintext, size_t pt_len,
                       uint8_t* ciphertext);
int rsa_private_decrypt(rsa_key_t* key, const uint8_t* ciphertext, size_t ct_len,
                        uint8_t* plaintext);
int rsa_verify_pkcs1(rsa_key_t* key, const uint8_t* signature, size_t sig_len,
                     const uint8_t* hash, size_t hash_len, int hash_alg);

// ECDH
int ecdh_generate_keypair(ec_key_t* key, ec_curve_type_t curve);
int ecdh_compute_shared_secret(ec_key_t* private_key, const uint8_t* peer_public,
                               size_t peer_len, uint8_t* shared_secret);

// HKDF (HMAC-based Key Derivation Function)
int hkdf_extract(const uint8_t* salt, size_t salt_len,
                 const uint8_t* ikm, size_t ikm_len,
                 uint8_t* prk);
int hkdf_expand(const uint8_t* prk, size_t prk_len,
                const uint8_t* info, size_t info_len,
                uint8_t* okm, size_t okm_len);

// TLS-specific key derivation
int tls_prf(const uint8_t* secret, size_t secret_len,
            const char* label,
            const uint8_t* seed, size_t seed_len,
            uint8_t* output, size_t output_len);

// ============================================================================
// X.509 PARSING
// ============================================================================

// Parse DER-encoded certificate
int x509_parse_der(const uint8_t* der_data, size_t len, x509_cert_t* cert);

// Parse PEM-encoded certificate(s)
int x509_parse_pem(const char* pem_data, size_t len, x509_cert_t* certs, int max_certs);

// Verify certificate signature
int x509_verify_signature(x509_cert_t* cert, x509_cert_t* issuer_cert);

// Check certificate validity period
int x509_check_validity(x509_cert_t* cert);

// Get certificate fingerprint
void x509_get_fingerprint(x509_cert_t* cert, uint8_t* fingerprint);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Random number generation
void tls_get_random(uint8_t* buffer, size_t len);
uint8_t tls_get_random_byte(void);

// Big-endian conversion
uint16_t tls_read_uint16(const uint8_t* p);
uint32_t tls_read_uint24(const uint8_t* p);
uint32_t tls_read_uint32(const uint8_t* p);
uint64_t tls_read_uint64(const uint8_t* p);

void tls_write_uint16(uint16_t v, uint8_t* p);
void tls_write_uint24(uint32_t v, uint8_t* p);
void tls_write_uint32(uint32_t v, uint8_t* p);
void tls_write_uint64(uint64_t v, uint8_t* p);

// Constant-time memory comparison (for MAC verification)
int tls_constant_time_memcmp(const void* a, const void* b, size_t len);

// Get error string
const char* tls_error_string(tls_error_t err);

#endif // TLS_H