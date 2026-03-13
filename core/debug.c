#include "debug.h"
#include "../include/types.h"
#include "../hal/drivers/serial.h"
#include "../core/string.h"
#include "../hal/cpu/timer.h"

// GCC Builtins for varargs
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)
typedef __builtin_va_list va_list;

uint32_t debug_level = LOG_INFO;
uint32_t debug_domains = DBG_ALL;

// Log colors for VGA
static uint32_t log_colors[] = {
    0xFF888888, // TRACE - gray
    0xFF00AAFF, // DEBUG - blue
    0xFF00FF00, // INFO  - green
    0xFFFFFF00, // WARN  - yellow
    0xFFFF6600, // ERROR - orange
    0xFFFF0000, // FATAL - red
};

static const char* level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const char* domain_names[] = {
    "NET", "ARP", "DNS", "TCP", "UDP", "DRIVER", "MEM", "FS"
};

void debug_init(void) {
    s_printf("[DEBUG] Debug system initialized\n");
    s_printf("[DEBUG] Level: ");
    s_printf(level_names[debug_level]);
    s_printf(", Domains: 0x");
    char buf[16]; int_to_str(debug_domains, buf); s_printf(buf);
    s_printf("\n");
}

void debug_set_level(uint32_t level) {
    debug_level = level;
}

void debug_set_domains(uint32_t domains) {
    debug_domains = domains;
}

void debug_log(uint32_t level, uint32_t domain, const char* fmt, ...) {
    // Simplified logging - just print the message with level prefix
    s_printf("[");
    s_printf(level_names[level]);
    s_printf("] ");
    s_printf(fmt);
    s_printf("\n");
}

// Hex dump utility
void hex_dump(const void* data, size_t size, const char* desc) {
    s_printf("[HEX] ");
    s_printf(desc);
    s_printf(" (");
    char buf[16]; int_to_str((int)size, buf); s_printf(buf);
    s_printf(" bytes)\n");
    // TODO: Implement full hex dump
}

// TODO: Implement packet capture functionality
void pcap_start(const char* filename) {
    s_printf("[PCAP] Packet capture not implemented\n");
}

void pcap_write_packet(const void* data, size_t len, int outgoing) {
    // Stub
}

void pcap_stop(void) {
    // Stub
}