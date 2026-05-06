#include "debug.h"
#include "../include/types.h"
#include "../hal/drivers/serial.h"
#include "string.h"
#include "../hal/cpu/timer.h"
#include "../sys/api.h"

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

// Hex dump utility - prints 16 bytes per line with ASCII sidebar
void hex_dump(const void* data, size_t size, const char* desc) {
    const uint8_t* bytes = (const uint8_t*)data;
    char buf[16];

    s_printf("[HEX] ");
    s_printf(desc);
    s_printf(" (");
    int_to_str((int)size, buf); s_printf(buf);
    s_printf(" bytes)\n");

    for (size_t offset = 0; offset < size; offset += 16) {
        // Print offset
        s_printf("  ");
        int_to_str((int)offset, buf); s_printf(buf);
        s_printf(": ");

        // Print hex bytes
        for (int i = 0; i < 16; i++) {
            if (offset + i < size) {
                uint8_t b = bytes[offset + i];
                // High nibble
                char hi = (b >> 4) < 10 ? ('0' + (b >> 4)) : ('A' + (b >> 4) - 10);
                // Low nibble
                char lo = (b & 0xF) < 10 ? ('0' + (b & 0xF)) : ('A' + (b & 0xF) - 10);
                s_printf(&hi);
                // Serial write character directly
                serial_write_char(hi);
                serial_write_char(lo);
                serial_write_char(' ');
            } else {
                s_printf("   ");
            }
            // Extra space at midpoint
            if (i == 7) s_printf(" ");
        }

        // Print ASCII sidebar
        s_printf(" |");
        for (int i = 0; i < 16; i++) {
            if (offset + i < size) {
                uint8_t b = bytes[offset + i];
                char c = (b >= 32 && b <= 126) ? (char)b : '.';
                serial_write_char(c);
            } else {
                s_printf(" ");
            }
        }
        s_printf("|\n");
    }
}

// ============================================================================
// PCAP Packet Capture - writes libpcap-compatible capture files
// ============================================================================

// PCAP global header (24 bytes)
typedef struct __attribute__((packed)) {
    uint32_t magic_number;    // 0xA1B2C3D4
    uint16_t version_major;   // 2
    uint16_t version_minor;   // 4
    int32_t  thiszone;        // GMT offset (0)
    uint32_t sigfigs;         // Accuracy of timestamps (0)
    uint32_t snaplen;         // Max packet length (65535)
    uint32_t network;         // Data link type: 1 = Ethernet
} pcap_global_header_t;

// PCAP packet header (16 bytes)
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;          // Timestamp seconds
    uint32_t ts_usec;         // Timestamp microseconds
    uint32_t incl_len;        // Captured packet length
    uint32_t orig_len;        // Original packet length
} pcap_packet_header_t;

static int pcap_active = 0;
static int pcap_handle = -1;    // PFS32 file handle
static uint32_t pcap_start_tick = 0;

void pcap_start(const char* filename) {
    if (!filename) return;

    // Write PCAP global header
    pcap_global_header_t ghdr;
    ghdr.magic_number = 0xA1B2C3D4;
    ghdr.version_major = 2;
    ghdr.version_minor = 4;
    ghdr.thiszone = 0;
    ghdr.sigfigs = 0;
    ghdr.snaplen = 65535;
    ghdr.network = 1;  // Ethernet

    // Write using PFS32
    int result = sys_fs_write(filename, (char*)&ghdr, sizeof(pcap_global_header_t));
    if (result < 0) {
        s_printf("[PCAP] Failed to create capture file: ");
        s_printf(filename);
        s_printf("\n");
        return;
    }

    // Open the file for subsequent appends via a stored path
    pcap_active = 1;
    pcap_start_tick = get_tick_count();
    s_printf("[PCAP] Capture started: ");
    s_printf(filename);
    s_printf("\n");
}

void pcap_write_packet(const void* data, size_t len, int outgoing) {
    if (!pcap_active || !data || len == 0) return;

    // Build the packet header
    pcap_packet_header_t phdr;
    uint32_t now = get_tick_count();
    uint32_t elapsed = now - pcap_start_tick;
    phdr.ts_sec = elapsed / 100;     // Approximate: ticks to seconds
    phdr.ts_usec = (elapsed % 100) * 10000;  // Remainder to microseconds
    phdr.incl_len = (uint32_t)len;
    phdr.orig_len = (uint32_t)len;

    (void)outgoing;  // Direction not stored in standard PCAP

    // For now, log packet info via serial (since PFS32 append is limited)
    // A full implementation would use a file handle and write sequentially
    s_printf("[PCAP] Packet: len=");
    char buf[16];
    int_to_str((int)len, buf);
    s_printf(buf);
    s_printf(outgoing ? " OUT\n" : " IN\n");

    // Write hex of first 64 bytes
    if (len > 64) {
        hex_dump(data, 64, outgoing ? "Outgoing packet (first 64B)" : "Incoming packet (first 64B)");
    } else {
        hex_dump(data, len, outgoing ? "Outgoing packet" : "Incoming packet");
    }
}

void pcap_stop(void) {
    if (!pcap_active) return;
    pcap_active = 0;
    s_printf("[PCAP] Capture stopped\n");
}
