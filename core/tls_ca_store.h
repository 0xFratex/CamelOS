// core/tls_ca_store.h - Root CA Certificate Store for TLS
// Contains embedded root CA certificates for major certificate authorities
// This enables proper HTTPS certificate validation

#ifndef TLS_CA_STORE_H
#define TLS_CA_STORE_H

#include <types.h>

// Maximum number of root CAs we can store
#define TLS_MAX_ROOT_CAS 64

// Maximum size of a single certificate (DER encoded)
#define TLS_MAX_CERT_SIZE 4096

// Root CA entry
typedef struct {
    const char* name;           // CA name (e.g., "DigiCert Global Root CA")
    const uint8_t* cert_der;    // DER-encoded certificate
    uint32_t cert_len;          // Certificate length
    uint32_t flags;             // Flags (trusted, expired, etc.)
} root_ca_entry_t;

// Certificate flags
#define CA_FLAG_TRUSTED     0x01
#define CA_FLAG_EXPIRED     0x02
#define CA_FLAG_EV          0x04  // Extended Validation

// Initialize the CA store
void tls_ca_store_init(void);

// Find a root CA by name
const root_ca_entry_t* tls_ca_find(const char* name);

// Verify a certificate chain against root CAs
int tls_verify_cert_chain(const uint8_t* cert_chain, uint32_t chain_len);

// Get number of loaded root CAs
int tls_ca_count(void);

// ============================================================================
// EMBEDDED ROOT CA CERTIFICATES (DER format)
// These are the most common root CAs needed for Google and modern websites
// ============================================================================

// Google Internet Authority G3 (for *.google.com)
extern const uint8_t google_g3_root[];
extern const uint32_t google_g3_root_len;

// DigiCert Global Root CA (widely used)
extern const uint8_t digicert_global_root[];
extern const uint32_t digicert_global_root_len;

// Let's Encrypt ISRG Root X1 (free certificates)
extern const uint8_t letsencrypt_isrg_root[];
extern const uint32_t letsencrypt_isrg_root_len;

// GlobalSign Root CA
extern const uint8_t globalsign_root[];
extern const uint32_t globalsign_root_len;

// Comodo/Sectigo Root CA
extern const uint8_t sectigo_root[];
extern const uint32_t sectigo_root_len;

// Amazon Root CA (for AWS services)
extern const uint8_t amazon_root[];
extern const uint32_t amazon_root_len;

// Microsoft RSA Root CA 2017
extern const uint8_t microsoft_root[];
extern const uint32_t microsoft_root_len;

// Cloudflare Origin CA
extern const uint8_t cloudflare_root[];
extern const uint32_t cloudflare_root_len;

// Google Trust Services GlobalSign Root
extern const uint8_t google_trust_root[];
extern const uint32_t google_trust_root_len;

// DigiCert TLS RSA SHA256 2020 CA1
extern const uint8_t digicert_tls_2020[];
extern const uint32_t digicert_tls_2020_len;

// GTS Root R1 (Google Trust Services)
extern const uint8_t gts_root_r1[];
extern const uint32_t gts_root_r1_len;

// GTS Root R2 (Google Trust Services)
extern const uint8_t gts_root_r2[];
extern const uint32_t gts_root_r2_len;

// GTS Root R3 (Google Trust Services)
extern const uint8_t gts_root_r3[];
extern const uint32_t gts_root_r3_len;

// GTS Root R4 (Google Trust Services)
extern const uint8_t gts_root_r4[];
extern const uint32_t gts_root_r4_len;

#endif // TLS_CA_STORE_H
