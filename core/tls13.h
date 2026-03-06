// core/tls13.h - TLS 1.3 Protocol Implementation
// Implements: TLS 1.3 handshake, HKDF, modern cipher suites
// Based on RFC 8446

#ifndef TLS13_H
#define TLS13_H

#include "../include/types.h"

// ============================================================================
// TLS 1.3 SPECIFIC CONSTANTS
// ============================================================================

// TLS 1.3 version
#define TLS13_VERSION 0x0304
#define TLS13_LEGACY_VERSION 0x0303  // Used in ClientHello for compatibility

// TLS 1.3 Content Types (same as 1.2 but encrypted differently)
#define TLS13_CONTENT_CHANGE_CIPHER_SPEC  20
#define TLS13_CONTENT_ALERT              21
#define TLS13_CONTENT_HANDSHAKE          22
#define TLS13_CONTENT_APPLICATION_DATA   23

// TLS 1.3 Handshake Types (new types)
#define TLS13_HELLO_REQUEST_RETRY        0
#define TLS13_CLIENT_HELLO               1
#define TLS13_SERVER_HELLO               2
#define TLS13_ENCRYPTED_EXTENSIONS       8
#define TLS13_CERTIFICATE               11
#define TLS13_CERTIFICATE_REQUEST       13
#define TLS13_CERTIFICATE_VERIFY        15
#define TLS13_FINISHED                  20
#define TLS13_KEY_UPDATE                24
#define TLS13_COMPRESSED_CERTIFICATE    25
#define TLS13_NEW_SESSION_TICKET         4

// TLS 1.3 Cipher Suites (AEAD only)
#define TLS13_AES_128_GCM_SHA256        0x1301
#define TLS13_AES_256_GCM_SHA384        0x1302
#define TLS13_CHACHA20_POLY1305_SHA256  0x1303
#define TLS13_AES_128_CCM_SHA256        0x1304
#define TLS13_AES_128_CCM_8_SHA256      0x1305

// TLS 1.3 Extension Types
#define TLS13_EXT_SERVER_NAME              0x0000
#define TLS13_EXT_MAX_FRAGMENT_LENGTH      0x0001
#define TLS13_EXT_STATUS_REQUEST           0x0005
#define TLS13_EXT_SUPPORTED_GROUPS         0x000A
#define TLS13_EXT_EC_POINT_FORMATS         0x000B
#define TLS13_EXT_SIGNATURE_ALGORITHMS     0x000D
#define TLS13_EXT_USE_SRTP                 0x000E
#define TLS13_EXT_HEARTBEAT                0x000F
#define TLS13_EXT_ALPN                     0x0010
#define TLS13_EXT_SCT                      0x0012
#define TLS13_EXT_SUPPORTED_VERSIONS       0x002B
#define TLS13_EXT_PSK_KEY_EXCHANGE_MODES   0x002D
#define TLS13_EXT_KEY_SHARE                0x0033
#define TLS13_EXT_COOKIE                   0x002C
#define TLS13_EXT_PRE_SHARED_KEY           0x0029
#define TLS13_EXT_EARLY_DATA               0x002A
#define TLS13_EXT_POST_HANDSHAKE_AUTH      0x0031

// Supported Groups (Named Curves)
#define TLS13_GROUP_SECP256R1    0x0017
#define TLS13_GROUP_SECP384R1    0x0018
#define TLS13_GROUP_SECP521R1    0x0019
#define TLS13_GROUP_X25519       0x001D
#define TLS13_GROUP_X448         0x001E
#define TLS13_GROUP_FFDHE2048    0x0100
#define TLS13_GROUP_FFDHE3072    0x0101
#define TLS13_GROUP_FFDHE4096    0x0102

// Signature Schemes
#define TLS13_SIG_RSA_PKCS1_SHA256   0x0401
#define TLS13_SIG_RSA_PKCS1_SHA384   0x0501
#define TLS13_SIG_RSA_PKCS1_SHA512   0x0601
#define TLS13_SIG_ECDSA_SECP256R1    0x0403
#define TLS13_SIG_ECDSA_SECP384R1    0x0503
#define TLS13_SIG_ECDSA_SECP521R1    0x0603
#define TLS13_SIG_RSA_PSS_RSAE_SHA256  0x0804
#define TLS13_SIG_RSA_PSS_RSAE_SHA384  0x0805
#define TLS13_SIG_RSA_PSS_RSAE_SHA512  0x0806
#define TLS13_SIG_ED25519            0x0807
#define TLS13_SIG_ED448              0x0808

// PSK Key Exchange Modes
#define TLS13_PSK_KE          0x00  // PSK-only (no (EC)DHE)
#define TLS13_PSK_DHE_KE      0x01  // PSK with (EC)DHE

// TLS 1.3 Key Sizes
#define TLS13_HASH_LEN          32   // SHA-256 hash length
#define TLS13_KEY_LEN_128       16   // AES-128 key length
#define TLS13_KEY_LEN_256       32   // AES-256 key length
#define TLS13_IV_LEN            12   // Nonce length for AEAD
#define TLS13_TAG_LEN           16   // AEAD tag length

// ============================================================================
// TLS 1.3 KEY SCHEDULE STRUCTURES
// ============================================================================

// TLS 1.3 Key Schedule Secrets
typedef struct {
    // Early secrets (for 0-RTT)
    uint8_t early_secret[TLS13_HASH_LEN];
    uint8_t early_traffic_secret[TLS13_HASH_LEN];
    
    // Handshake secrets
    uint8_t handshake_secret[TLS13_HASH_LEN];
    uint8_t client_handshake_traffic_secret[TLS13_HASH_LEN];
    uint8_t server_handshake_traffic_secret[TLS13_HASH_LEN];
    
    // Master secrets
    uint8_t master_secret[TLS13_HASH_LEN];
    uint8_t client_traffic_secret_0[TLS13_HASH_LEN];
    uint8_t server_traffic_secret_0[TLS13_HASH_LEN];
    
    // Exporter secret
    uint8_t exporter_secret[TLS13_HASH_LEN];
    
    // Derived keys
    uint8_t client_write_key[32];
    uint8_t server_write_key[32];
    uint8_t client_write_iv[TLS13_IV_LEN];
    uint8_t server_write_iv[TLS13_IV_LEN];
    
    // Key length (16 or 32 bytes)
    int key_len;
    
} tls13_key_schedule_t;

// TLS 1.3 Transcript Hash
typedef struct {
    uint8_t client_hello_hash[TLS13_HASH_LEN];
    uint8_t server_hello_hash[TLS13_HASH_LEN];
    uint8_t encrypted_extensions_hash[TLS13_HASH_LEN];
    uint8_t certificate_hash[TLS13_HASH_LEN];
    uint8_t certificate_verify_hash[TLS13_HASH_LEN];
    uint8_t server_finished_hash[TLS13_HASH_LEN];
} tls13_transcript_hash_t;

// ============================================================================
// TLS 1.3 SESSION STRUCTURE
// ============================================================================

typedef struct tls13_session {
    // Connection info
    int socket_fd;
    uint16_t negotiated_version;
    uint16_t cipher_suite;
    int is_server;
    
    // Server info
    char server_name[256];
    uint16_t port;
    
    // Key schedule
    tls13_key_schedule_t keys;
    tls13_transcript_hash_t transcript;
    
    // Handshake state
    int state;
    int handshake_complete;
    
    // Random values
    uint8_t client_random[32];
    uint8_t server_random[32];
    
    // Key share (for (EC)DHE)
    uint8_t key_share_public[64];
    uint8_t key_share_private[32];
    uint16_t key_share_group;
    uint8_t server_key_share_public[64];
    size_t server_key_share_len;
    
    // PSK (Pre-Shared Key) for session resumption
    uint8_t psk[64];
    size_t psk_len;
    int psk_identity;
    
    // Certificate info
    uint8_t* server_cert;
    size_t server_cert_len;
    
    // Traffic keys for encryption/decryption
    uint64_t client_record_seq;
    uint64_t server_record_seq;
    
    // Application data buffer
    uint8_t app_data[16384];
    size_t app_data_len;
    
    // Error info
    int last_error;
    char error_msg[128];
    
} tls13_session_t;

// ============================================================================
// TLS 1.3 STATES
// ============================================================================

typedef enum {
    TLS13_STATE_INIT = 0,
    TLS13_STATE_HELLO_RETRY,
    TLS13_STATE_CLIENT_HELLO_SENT,
    TLS13_STATE_SERVER_HELLO_RECEIVED,
    TLS13_STATE_ENCRYPTED_EXTENSIONS_RECEIVED,
    TLS13_STATE_CERTIFICATE_RECEIVED,
    TLS13_STATE_CERTIFICATE_VERIFY_RECEIVED,
    TLS13_STATE_SERVER_FINISHED_RECEIVED,
    TLS13_STATE_CLIENT_FINISHED_SENT,
    TLS13_STATE_HANDSHAKE_COMPLETE,
    TLS13_STATE_ERROR
} tls13_state_t;

// ============================================================================
// HKDF FUNCTIONS (RFC 5869)
// ============================================================================

// HKDF-Extract(salt, IKM) -> PRK
int hkdf_extract_sha256(const uint8_t* salt, size_t salt_len,
                        const uint8_t* ikm, size_t ikm_len,
                        uint8_t* prk);

// HKDF-Expand(PRK, info, L) -> OKM
int hkdf_expand_sha256(const uint8_t* prk, size_t prk_len,
                       const uint8_t* info, size_t info_len,
                       uint8_t* okm, size_t okm_len);

// HKDF-Expand-Label(Secret, Label, Context, Length) -> key
int hkdf_expand_label(const uint8_t* secret, size_t secret_len,
                      const char* label,
                      const uint8_t* context, size_t context_len,
                      uint8_t* output, size_t output_len);

// ============================================================================
// TLS 1.3 KEY DERIVATION
// ============================================================================

// Derive early secret from PSK or zero
int tls13_derive_early_secret(tls13_session_t* session, const uint8_t* psk, size_t psk_len);

// Derive handshake secret from shared secret
int tls13_derive_handshake_secret(tls13_session_t* session, const uint8_t* shared_secret, size_t ss_len);

// Derive master secret
int tls13_derive_master_secret(tls13_session_t* session);

// Derive traffic keys
int tls13_derive_traffic_keys(tls13_session_t* session);

// Derive finished key
int tls13_derive_finished_key(const uint8_t* traffic_secret, uint8_t* finished_key);

// ============================================================================
// TLS 1.3 RECORD LAYER
// ============================================================================

// Encrypt TLS 1.3 record (AEAD)
int tls13_encrypt_record(tls13_session_t* session,
                         uint8_t content_type,
                         const uint8_t* plaintext, size_t pt_len,
                         uint8_t* ciphertext);

// Decrypt TLS 1.3 record (AEAD)
int tls13_decrypt_record(tls13_session_t* session,
                         const uint8_t* ciphertext, size_t ct_len,
                         uint8_t* plaintext, uint8_t* content_type);

// Build TLS 1.3 nonce from IV and sequence number
void tls13_build_nonce(const uint8_t* iv, uint64_t seq, uint8_t* nonce);

// ============================================================================
// TLS 1.3 HANDSHAKE
// ============================================================================

// Create TLS 1.3 session
tls13_session_t* tls13_create_session(void);

// Destroy TLS 1.3 session
void tls13_destroy_session(tls13_session_t* session);

// Connect to server using TLS 1.3
int tls13_connect(tls13_session_t* session, const char* hostname, uint16_t port);

// Close TLS 1.3 connection
int tls13_close(tls13_session_t* session);

// Send ClientHello
int tls13_send_client_hello(tls13_session_t* session);

// Process ServerHello
int tls13_process_server_hello(tls13_session_t* session, const uint8_t* data, size_t len);

// Process EncryptedExtensions
int tls13_process_encrypted_extensions(tls13_session_t* session, const uint8_t* data, size_t len);

// Process Certificate
int tls13_process_certificate(tls13_session_t* session, const uint8_t* data, size_t len);

// Process CertificateVerify
int tls13_process_certificate_verify(tls13_session_t* session, const uint8_t* data, size_t len);

// Process Finished
int tls13_process_finished(tls13_session_t* session, const uint8_t* data, size_t len);

// Send Finished
int tls13_send_finished(tls13_session_t* session);

// Compute Finished verify data
int tls13_compute_finished_data(const uint8_t* finished_key, const uint8_t* transcript_hash,
                                uint8_t* verify_data);

// ============================================================================
// TLS 1.3 DATA TRANSFER
// ============================================================================

// Write application data
int tls13_write(tls13_session_t* session, const void* data, size_t len);

// Read application data
int tls13_read(tls13_session_t* session, void* buffer, size_t max_len);

// ============================================================================
// X25519 ELLIPTIC CURVE DIFFIE-HELLMAN (RFC 7748)
// ============================================================================

// Generate X25519 key pair
int x25519_generate_keypair(uint8_t* public_key, uint8_t* private_key);

// Compute X25519 shared secret
int x25519_compute_shared(const uint8_t* private_key, const uint8_t* peer_public,
                          uint8_t* shared_secret);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Check if cipher suite is TLS 1.3
int tls13_is_tls13_cipher(uint16_t cipher_suite);

// Get hash algorithm for cipher suite
int tls13_get_hash_len(uint16_t cipher_suite);

// Get key length for cipher suite
int tls13_get_key_len(uint16_t cipher_suite);

#endif // TLS13_H
