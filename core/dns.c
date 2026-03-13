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
#define DNS_DEBUG_ENABLED     0

#define DNS_CACHE_SIZE 32     // Increased cache size
#define DNS_TIMEOUT 300       // Reduced timeout (was 500)

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

// QEMU DNS is 10.0.2.3
// In network byte order (big endian): 10.0.2.3 = 0x0A000203
#define QEMU_DNS_IP 0x0A000203 

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

int dns_resolve(const char* domain, char* ip_out, int max_len) {
    // 1. Check Cache first (fast path)
    for(int i=0; i<dns_count; i++) {
        if(strcmp(dns_cache[i].domain, domain) == 0) {
            // Cache stores IP in host byte order
            ip_to_str(dns_cache[i].ip, ip_out);
            return 0;  // Cache hit - instant return
        }
    }

    // 2. Ensure ARP for gateway is resolved first
    extern int arp_resolve(uint32_t ip, uint8_t* mac_out);
    uint8_t gw_mac[6];
    arp_resolve(QEMU_DNS_IP, gw_mac);  // Don't warn on failure

    // 3. Query
    int s = k_socket(AF_INET, SOCK_DGRAM, 0);
    if(s < 0) return -1;

    uint8_t pkt[512];
    memset(pkt, 0, 512);
    
    dns_header_t* hdr = (dns_header_t*)pkt;
    hdr->id = htons(0x1234);
    hdr->flags = htons(0x0100);
    hdr->qdcount = htons(1);
    
    int len = sizeof(dns_header_t);
    len += dns_encode(domain, pkt + len);
    
    dns_question_t* q = (dns_question_t*)(pkt + len);
    q->qtype = htons(1);
    q->qclass = htons(1);
    len += sizeof(dns_question_t);
    
    // Destination
    sockaddr_in_t dest;
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    dest.sin_addr = QEMU_DNS_IP;
    
    k_sendto(s, pkt, len, 0, &dest);
    
    // Receive with optimized timeout
    uint8_t resp[512];
    uint32_t start = get_tick_count();
    
    while((get_tick_count() - start) < DNS_TIMEOUT) {
        extern void rtl8139_poll();
        rtl8139_poll();
        
        int r = k_recvfrom(s, resp, 512, 0, 0);
        if (r > 0) {
            int ptr = 12;
            while(resp[ptr] != 0) ptr += (resp[ptr] + 1);
            ptr += 5;
            
            if (ptr + 16 > r) break;
            ptr += 2; // Skip Name
            
            uint16_t type = ntohs(*(uint16_t*)(resp + ptr));
            ptr += 8;
            
            uint16_t dlen = ntohs(*(uint16_t*)(resp + ptr));
            ptr += 2;
            
            if (type == 1 && dlen == 4) {
                // Read IP from DNS response
                uint32_t raw_ip = *(uint32_t*)(resp + ptr);
                uint32_t final_ip = ntohl(raw_ip);
                
                // Add to cache
                if(dns_count < DNS_CACHE_SIZE) {
                    strcpy(dns_cache[dns_count].domain, domain);
                    dns_cache[dns_count].ip = final_ip;
                    dns_cache[dns_count].timestamp = get_tick_count();
                    dns_count++;
                }
                
                ip_to_str(final_ip, ip_out);
                k_close(s);
                return 0;
            }
        }
    }
    
    k_close(s);
    return -1;
}