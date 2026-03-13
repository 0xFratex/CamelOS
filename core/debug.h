// debug.h - Improved debugging system
#ifndef DEBUG_H
#define DEBUG_H

#include "../include/types.h"

// Log levels
#define LOG_TRACE   0
#define LOG_DEBUG   1
#define LOG_INFO    2
#define LOG_WARN    3
#define LOG_ERROR   4
#define LOG_FATAL   5

// Debug domains
#define DBG_NET     0x0001
#define DBG_ARP     0x0002
#define DBG_DNS     0x0004
#define DBG_TCP     0x0008
#define DBG_UDP     0x0010
#define DBG_DRIVER  0x0020
#define DBG_MEM     0x0040
#define DBG_FS      0x0080
#define DBG_ALL     0xFFFF

// Configuration
extern uint32_t debug_level;
extern uint32_t debug_domains;

// Initialize debugging
void debug_init(void);

// Set debug level and domains
void debug_set_level(uint32_t level);
void debug_set_domains(uint32_t domains);

// Logging macros
#define LOG(level, domain, fmt, ...) \
    do { \
        if((level) >= debug_level && ((domain) & debug_domains)) \
            debug_log(level, domain, fmt, ##__VA_ARGS__); \
    } while(0)

#define NET_TRACE(fmt, ...) LOG(LOG_TRACE, DBG_NET, fmt, ##__VA_ARGS__)
#define NET_DEBUG(fmt, ...) LOG(LOG_DEBUG, DBG_NET, fmt, ##__VA_ARGS__)
#define NET_INFO(fmt, ...)  LOG(LOG_INFO,  DBG_NET, fmt, ##__VA_ARGS__)
#define NET_WARN(fmt, ...)  LOG(LOG_WARN,  DBG_NET, fmt, ##__VA_ARGS__)
#define NET_ERROR(fmt, ...) LOG(LOG_ERROR, DBG_NET, fmt, ##__VA_ARGS__)

#define ARP_TRACE(fmt, ...) LOG(LOG_TRACE, DBG_ARP, fmt, ##__VA_ARGS__)
#define ARP_DEBUG(fmt, ...) LOG(LOG_DEBUG, DBG_ARP, fmt, ##__VA_ARGS__)
#define ARP_INFO(fmt, ...)  LOG(LOG_INFO,  DBG_ARP, fmt, ##__VA_ARGS__)

// Hex dump utility
void hex_dump(const void* data, size_t size, const char* desc);

// Packet capture utility
void pcap_start(const char* filename);
void pcap_stop(void);
void pcap_write_packet(const void* data, size_t len, int outgoing);

#endif