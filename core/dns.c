// core/dns.c - Optimized DNS resolver with caching
#include "dns.h"
#include "socket.h"
#include "net.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"
#include "../hal/cpu/timer.h"

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production
// ============================================================================
#define DNS_DEBUG_ENABLED     1

#define DNS_CACHE_SIZE 32     // Increased cache size

typedef struct {
    char domain[64];
    uint32_t ip; 
    uint32_t ttl;
    uint32_t timestamp;
} dns_entry_t;

static dns_entry_t dns_cache[DNS_CACHE_SIZE];
static int dns_count = 0;

void dns_init() {
    memset(dns_cache, 0, sizeof(dns_cache));
    dns_count = 0;
}

// Configurable DNS server - default to QEMU user-mode DNS (10.0.2.3).
// FIX: Previously defaulted to 8.8.8.8 (Google DNS) which doesn't work
// in QEMU user-mode networking because the guest can't route to external
// DNS servers directly. QEMU provides a DNS forwarder at 10.0.2.3.
// Can be overridden via DHCP or manually via net_set_dns().
static uint32_t dns_server_ip = 0x0A000203;  // 10.0.2.3 QEMU DNS

int dns_encode(const char* host, uint8_t* buf) {
    int len = strlen(host);
    int pos = 0;
    int part_len = 0;
    int part_start = 0;
    
    for(int i=0; i<=len; i++) {
        if(host[i] == '.' || host[i] == 0) {
            buf[pos++] = part_len;
            memcpy(buf + pos, host + part_start, part_len);
            pos += part_len;
            part_len = 0;
            part_start = i + 1;
        } else {
            part_len++;
        }
    }
    buf[pos++] = 0;
    return pos;
}

// ============================================================================
// DNS Response Parsing Helpers (RFC 1035)
// ============================================================================

// Skip a DNS name in a response buffer, handling compression pointers.
// Returns the offset immediately after the name field.
static int dns_skip_name(const uint8_t* resp, int resp_len, int offset) {
    while (offset < resp_len) {
        uint8_t b = resp[offset];
        if (b == 0) {
            return offset + 1;  // null terminator
        }
        if ((b & 0xC0) == 0xC0) {
            return offset + 2;  // compression pointer is 2 bytes
        }
        offset += b + 1;  // skip label length byte + label data
    }
    return offset;  // malformed, but don't overrun
}

// Parse a DNS name from a response buffer, following compression pointers.
// Writes the decoded domain name to `out` (must be at least 64 bytes).
// Returns the offset immediately after the name field in the original packet.
static int dns_parse_name(const uint8_t* resp, int resp_len, int offset,
                          char* out, int out_max) {
    int out_pos = 0;
    int saved_offset = -1;
    int jumped = 0;
    int jump_count = 0;
    const int MAX_JUMPS = 32;  // Prevent infinite loops from circular pointers

    while (offset < resp_len) {
        uint8_t b = resp[offset];
        if (b == 0) {
            if (!jumped) offset++;
            break;
        }
        if ((b & 0xC0) == 0xC0) {
            if (++jump_count > MAX_JUMPS) break;  // Circular pointer protection
            if (!jumped) {
                saved_offset = offset + 2;  // pointer takes 2 bytes
                jumped = 1;
            }
            offset = ((b & 0x3F) << 8) | resp[offset + 1];
            continue;
        }

        int label_len = b;
        offset++;
        for (int i = 0; i < label_len && offset < resp_len; i++) {
            if (out_pos < out_max - 1)
                out[out_pos++] = resp[offset];
            offset++;
        }
        if (out_pos < out_max - 1)
            out[out_pos++] = '.';
    }

    // Remove trailing dot
    if (out_pos > 0 && out[out_pos - 1] == '.')
        out_pos--;
    out[out_pos] = 0;

    return jumped ? saved_offset : offset;
}

// ============================================================================
// DNS Cache helpers
// ============================================================================

// Add or update a cache entry for the given domain.
static void dns_cache_store(const char* domain, uint32_t ip, uint32_t ttl) {
    // Minimum TTL of 60 seconds to prevent excessive re-querying
    if (ttl < 60) ttl = 60;

    // Update existing entry if present
    for (int i = 0; i < dns_count; i++) {
        if (strcmp(dns_cache[i].domain, domain) == 0) {
            dns_cache[i].ip = ip;
            dns_cache[i].ttl = ttl;
            dns_cache[i].timestamp = get_tick_count();
            return;
        }
    }

    // Add new entry
    if (dns_count < DNS_CACHE_SIZE) {
        strcpy(dns_cache[dns_count].domain, domain);
        dns_cache[dns_count].ip = ip;
        dns_cache[dns_count].ttl = ttl;
        dns_cache[dns_count].timestamp = get_tick_count();
        dns_count++;
    }
}

// Check the cache for a non-expired entry. Returns 1 on hit, 0 on miss.
static int dns_cache_lookup(const char* domain, uint32_t* ip_out) {
    uint32_t now = get_tick_count();
    for (int i = 0; i < dns_count; i++) {
        if (strcmp(dns_cache[i].domain, domain) == 0) {
            uint32_t ttl_ticks = dns_cache[i].ttl * 50;  // seconds to ticks (50Hz)
            if ((now - dns_cache[i].timestamp) < ttl_ticks) {
                *ip_out = dns_cache[i].ip;
                return 1;  // cache hit, not expired
            }
            // Entry expired - will be refreshed on next successful query
            return 0;
        }
    }
    return 0;
}

// ============================================================================
// Internal resolver with CNAME depth tracking
// ============================================================================

#define DNS_MAX_CNAME_DEPTH 5

static int dns_resolve_internal(const char* domain, char* ip_out, int max_len,
                                int cname_depth) {
    if (cname_depth > DNS_MAX_CNAME_DEPTH) return -1;

#if DNS_DEBUG_ENABLED
    s_printf("[DNS] Resolving '%s' (cname_depth=%d)\n", domain, cname_depth);
#endif

    // 1. Check cache first (with TTL expiration)
    uint32_t cached_ip;
    if (dns_cache_lookup(domain, &cached_ip)) {
#if DNS_DEBUG_ENABLED
        char cached_str[16];
        ip_to_str(cached_ip, cached_str);
        s_printf("[DNS] Cache hit for '%s' -> %s (dns_count=%d)\n", domain, cached_str, dns_count);
#endif
        ip_to_str(cached_ip, ip_out);
        return 0;
    }
#if DNS_DEBUG_ENABLED
    s_printf("[DNS] Cache miss for '%s' (dns_count=%d)\n", domain, dns_count);
#endif

    // 2. Ensure ARP for gateway is resolved first
    extern int arp_resolve(uint32_t ip, uint8_t* mac_out);
    uint8_t gw_mac[6];
    int arp_ok = arp_resolve(dns_server_ip, gw_mac);
#if DNS_DEBUG_ENABLED
    {
        char dns_str[16];
        ip_to_str(dns_server_ip, dns_str);
        s_printf("[DNS] DNS server: %s, ARP resolve: %s, MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 dns_str, arp_ok ? "OK" : "FAIL",
                 gw_mac[0], gw_mac[1], gw_mac[2], gw_mac[3], gw_mac[4], gw_mac[5]);
    }
#endif

    // Flush stale packets from the NIC RX ring before sending the DNS query.
    // After a heavy HTTPS page load (TCP+TLS+HTTP/2 with 80KB+ of data),
    // the RX ring is typically clogged with stale TCP ACKs, FIN retransmits,
    // and duplicate TLS records from the just-closed connection. If we don't
    // flush them, rtl8139_receive_packets() processes up to RX_MAX_BATCH=64
    // packets per poll, and the DNS response (a single small UDP packet) can
    // get stuck behind dozens of stale TCP packets — effectively starving
    // the DNS resolver until it times out. This was the root cause of
    // "DNS works once then fails": the first query succeeds because the RX
    // ring is empty, but every subsequent query (for images, CSS, etc.)
    // fails because the ring is full of stale packets from the page load.
    extern void rtl8139_flush_rx(void);
    rtl8139_flush_rx();

    // 3. Query with retry logic
    int s = k_socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
#if DNS_DEBUG_ENABLED
        s_printf("[DNS] ERROR: k_socket() failed\n");
#endif
        return -1;
    }

    // Mark the DNS socket non-blocking. Previously it inherited the default
    // blocking mode, and k_recvfrom() would internally block for up to
    // SOCKET_TIMEOUT/10 (~10s) per call when no response had arrived yet.
    // That defeated the per-retry budget below (100/150/250 ticks = 2/3/5s)
    // — a single unreachable DNS server could freeze the browser for 30s+
    // before the resolver gave up. In non-blocking mode, k_recvfrom()
    // returns -1 immediately when there is no data, and our own loop's
    // timeout becomes the real upper bound.
    extern int k_socket_set_nonblocking(int fd);
    k_socket_set_nonblocking(s);

    // Retry timeouts in ticks (50Hz): 10s, 10s, 10s = 30s total.
    // Previously 2s + 3s + 5s = 10s, but QEMU SLIRP can delay DNS responses
    // by 10-30 seconds. With 30s total, late responses from retry 0 will
    // arrive during retry 2's window and be accepted (txid check removed).
    static const int retry_timeouts[3] = {500, 500, 500};
    int max_retries = 3;

    uint8_t pkt[512];
    uint8_t resp[4096];  // Large buffer for big DNS responses

    sockaddr_in_t dest;
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    dest.sin_addr = dns_server_ip;

    for (int retry = 0; retry < max_retries; retry++) {
        // Build query packet with random transaction ID
        memset(pkt, 0, 512);
        dns_header_t* hdr = (dns_header_t*)pkt;

        // Generate pseudo-random transaction ID unique per retry
        uint16_t txid = (uint16_t)(get_tick_count() ^ (retry << 8) ^ (uint32_t)domain);
        hdr->id = htons(txid);
        hdr->flags = htons(0x0100);  // standard query, recursion desired
        hdr->qdcount = htons(1);
        hdr->ancount = 0;
        hdr->nscount = 0;
        hdr->arcount = 0;

        int len = sizeof(dns_header_t);
        len += dns_encode(domain, pkt + len);

        dns_question_t* q = (dns_question_t*)(pkt + len);
        q->qtype = htons(1);   // type A
        q->qclass = htons(1);  // class IN
        len += sizeof(dns_question_t);

        int send_result = k_sendto(s, pkt, len, 0, &dest);
#if DNS_DEBUG_ENABLED
        s_printf("[DNS] Retry %d: sent %d bytes (send_result=%d), txid=%u, waiting for response...\n",
                 retry, len, send_result, txid);
#endif

        // Receive with per-retry timeout
        uint32_t start = get_tick_count();
        int timeout = retry_timeouts[retry];
        int poll_count = 0;

        while ((get_tick_count() - start) < (uint32_t)timeout) {
            extern void net_poll(void);
            net_poll();

            // Keep GUI responsive during DNS polling
            extern void http_process_events(void);
            http_process_events();

            poll_count++;
            int r = k_recvfrom(s, resp, 4096, 0, 0);
            if (r > (int)sizeof(dns_header_t)) {
                dns_header_t* rhdr = (dns_header_t*)resp;

#if DNS_DEBUG_ENABLED
                s_printf("[DNS] Received %d bytes after %d polls, txid=%u (expected %u)\n",
                         r, poll_count, ntohs(rhdr->id), txid);
#endif

                // Verify this is a response (not a query).
                // Note: We don't check the txid because QEMU SLIRP has high
                // latency — a response to retry 0 may arrive during retry 2's
                // window, when we've already moved to a different txid. Since
                // we only have one outstanding query at a time, any DNS response
                // is for our current domain. Accepting late responses from
                // previous retries fixes the "DNS FAILED after 3 retries" issue.
                if (!(ntohs(rhdr->flags) & 0x8000)) continue;  // not a response

                uint16_t ancount = ntohs(rhdr->ancount);
#if DNS_DEBUG_ENABLED
                s_printf("[DNS] Response: ancount=%u, flags=0x%04X\n", ancount, ntohs(rhdr->flags));
#endif
                if (ancount == 0) continue;

                // Skip question section
                int ptr = 12;
                for (int qd = 0; qd < ntohs(rhdr->qdcount) && ptr < r; qd++) {
                    ptr = dns_skip_name(resp, r, ptr);
                    ptr += 4;  // skip qtype + qclass
                }

                // Process all answer records
                char cname[64] = {0};
                int found_cname = 0;
                uint32_t answer_ip = 0;
                int found_a = 0;
                uint32_t best_ttl = 60;

                for (int an = 0; an < ancount && ptr < r; an++) {
                    // Skip the name field (may use compression)
                    ptr = dns_skip_name(resp, r, ptr);

                    if (ptr + 10 > r) break;  // need type(2)+class(2)+ttl(4)+rdlength(2)

                    uint16_t type = ntohs(*(uint16_t*)(resp + ptr)); ptr += 2;
                    /* uint16_t cls = */ ntohs(*(uint16_t*)(resp + ptr)); ptr += 2;
                    uint32_t ttl_val = ntohl(*(uint32_t*)(resp + ptr)); ptr += 4;
                    uint16_t rdlength = ntohs(*(uint16_t*)(resp + ptr)); ptr += 2;

                    if (ptr + rdlength > r) break;  // overrun check

                    if (type == DNS_TYPE_A && rdlength == 4) {
                        answer_ip = ntohl(*(uint32_t*)(resp + ptr));
                        best_ttl = ttl_val;
                        found_a = 1;
                    } else if (type == DNS_TYPE_CNAME && rdlength > 0) {
                        // Parse the canonical name from RDATA using compression
                        dns_parse_name(resp, r, ptr, cname, sizeof(cname));
                        found_cname = 1;
                        if (!found_a && ttl_val < best_ttl)
                            best_ttl = ttl_val;
                    }

                    ptr += rdlength;
                }

                if (found_a) {
                    // A record found - cache and return
                    dns_cache_store(domain, answer_ip, best_ttl);
                    ip_to_str(answer_ip, ip_out);
#if DNS_DEBUG_ENABLED
                    s_printf("[DNS] Resolved '%s' -> %s (after %d polls)\n", domain, ip_out, poll_count);
#endif
                    k_close(s);
                    return 0;
                } else if (found_cname) {
                    // No A record but CNAME found - follow the chain
#if DNS_DEBUG_ENABLED
                    s_printf("[DNS] CNAME chain: '%s' -> '%s'\n", domain, cname);
#endif
                    k_close(s);
                    return dns_resolve_internal(cname, ip_out, max_len,
                                                cname_depth + 1);
                }

                // No useful records in this response, try next retry
#if DNS_DEBUG_ENABLED
                s_printf("[DNS] No A/CNAME records in response, retrying\n");
#endif
                break;
            }

            // Brief pause before polling again
            for (volatile int i = 0; i < 500; i++) asm volatile("pause");
        }
#if DNS_DEBUG_ENABLED
        s_printf("[DNS] Retry %d timed out after %d polls (no response)\n", retry, poll_count);
#endif
    }

#if DNS_DEBUG_ENABLED
    s_printf("[DNS] FAILED to resolve '%s' after %d retries\n", domain, max_retries);
#endif
    k_close(s);
    return -1;
}

// ============================================================================
// Public API
// ============================================================================

int dns_resolve(const char* domain, char* ip_out, int max_len) {
    return dns_resolve_internal(domain, ip_out, max_len, 0);
}