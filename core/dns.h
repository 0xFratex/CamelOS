#ifndef DNS_H
#define DNS_H

#include "net.h"

// DNS Header
typedef struct {
    uint16_t id;               // Identification
    uint16_t flags;            // Flags
    uint16_t qdcount;          // Number of questions
    uint16_t ancount;          // Number of answers
    uint16_t nscount;          // Number of authority records
    uint16_t arcount;          // Number of additional records
} __attribute__((packed)) dns_header_t;

// DNS Question
typedef struct {
    // QNAME - variable length, terminated with 0
    uint16_t qtype;            // Query type
    uint16_t qclass;           // Query class
} __attribute__((packed)) dns_question_t;

// DNS Resource Record
typedef struct {
    // NAME - variable length, terminated with 0 or pointer
    uint16_t type;             // Record type
    uint16_t dns_class;        // Record class
    uint32_t ttl;              // Time to live
    uint16_t rdlength;         // RDATA length
    // RDATA - variable length
} __attribute__((packed)) dns_rr_t;

// DNS Record Types
#define DNS_TYPE_A     1  // IPv4 address
#define DNS_TYPE_AAAA  28 // IPv6 address
#define DNS_TYPE_CNAME 5  // Canonical name

// DNS Classes
#define DNS_CLASS_IN   1  // Internet

// DNS Flags
#define DNS_FLAG_QUERY     0x0000
#define DNS_FLAG_RESPONSE  0x8000
#define DNS_FLAG_STANDARD  0x0000
#define DNS_FLAG_RECURSION_DESIRED 0x0100
#define DNS_FLAG_RECURSION_AVAILABLE 0x0200

// Resolves a hostname to an IP address string
int dns_resolve(const char* domain, char* ip_str, int buffer_len);

#endif
