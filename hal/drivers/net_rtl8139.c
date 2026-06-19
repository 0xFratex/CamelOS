// hal/drivers/net_rtl8139.c - OPTIMIZED FOR PERFORMANCE
#include "net_rtl8139.h"
#include "serial.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include "../../core/net.h"
#include "../../core/net_if.h"
#include "../../common/ports.h"

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production, 1 for debugging
// ============================================================================
#define RTL_DEBUG_INIT        1    // Log initialization (Enabled for debugging Panic)
#define RTL_DEBUG_TX          0    // Log TX operations  
#define RTL_DEBUG_RX          0    // Log RX operations
#define RTL_DEBUG_ERRORS      1    // Always log errors

// Registers
#define RTL_REG_IDR0     0x00 
#define RTL_REG_MAR0     0x08
#define RTL_REG_TSD0     0x10
#define RTL_REG_TSAD0    0x20
#define RTL_REG_RBSTART  0x30
#define RTL_REG_CMD      0x37
#define RTL_REG_CAPR     0x38
#define RTL_REG_CBR      0x3A
#define RTL_REG_IMR      0x3C
#define RTL_REG_ISR      0x3E
#define RTL_REG_TCR      0x40
#define RTL_REG_RCR      0x44
#define RTL_REG_CONFIG1  0x52

// Buffer size - Increased to 32KB + Wrap margin for modern web traffic
#define RX_BUF_SIZE (32768 + 16 + 1536)
#define TX_BUF_SIZE 2048

// Performance tuning
#define TX_TIMEOUT_CYCLES     100000
#define RX_MAX_BATCH          64     // Increased packet throughput
#define MAX_CONSECUTIVE_ERRORS 5      // Max bad packets before RX reset

rtl8139_dev_t rtl_dev;  // Global device structure
static int rtl_initialized = 0;

// Local MAC address storage
static uint8_t local_mac[6];

// Static buffers for TX (small)
static uint8_t tx_buffers[4][TX_BUF_SIZE] __attribute__((aligned(4)));

// RX buffer — use static allocation with 32KB alignment.
// Previous heap-based allocation (kmalloc) could fail when the heap
// was fragmented after paging init.  A static BSS buffer is always
// available and avoids the ~67KB contiguous allocation problem.
static uint8_t rx_buffer_storage[RX_BUF_SIZE + 32768] __attribute__((aligned(32768)));
static uint8_t* rx_buffer_aligned = 0;
static uint16_t current_packet_ptr = 0;
static int tx_cur = 0;
net_if_t rtl_if;

// Statistics
static uint32_t stat_tx_packets = 0;
static uint32_t stat_rx_packets = 0;
static uint32_t stat_tx_errors = 0;
static uint32_t stat_rx_errors = 0;
static uint32_t consecutive_rx_errors = 0;

// Optimized TX function - minimal logging
int rtl8139_send_wrapper(net_if_t* net_if, uint8_t* data, uint32_t len) {
    if (rtl_dev.io_base == 0) return -1;
    if (len > 1792) len = 1792;
    if (len < 60) len = 60; // Min Ethernet size

    // The RTL8139 has 4 TX descriptors. We rotate through them.
    // The OWN bit (bit 13 of TSD) is 1 when the hardware is done transmitting
    // and the descriptor is available for software use.
    //
    // Previously, we did TWO blocking waits per send:
    //   1. Pre-send: wait for OWN=1 (previous TX done)
    //   2. Post-send: wait for OWN=1 (this TX done)
    //
    // The post-send wait was the bottleneck — QEMU's RTL8139 emulation doesn't
    // always set OWN promptly, causing multi-second delays on every packet.
    // This delayed the TLS ClientHello so much that servers closed the
    // connection before it arrived.
    //
    // FIX: Removed the post-send wait entirely. With 4 TX descriptors and
    // rotation, the pre-send wait will catch any case where we're sending
    // faster than the hardware can transmit. The pre-send wait is also
    // shortened to a small poll — if the descriptor isn't ready, we just
    // advance to the next one rather than blocking.

    // Brief check if current descriptor is available (OWN=1).
    // Don't block — if it's not ready, try the next descriptor.
    int desc = tx_cur;
    for (int i = 0; i < 4; i++) {
        uint32_t tsd = inl(rtl_dev.io_base + RTL_REG_TSD0 + (desc * 4));
        if (tsd & (1 << 13)) break;  // OWN=1, descriptor available
        desc = (desc + 1) % 4;
    }

    // Use the descriptor we found (or the current one if all are busy).
    // Even if all descriptors are busy, we proceed — the hardware will
    // handle the overwrite. This is better than blocking for seconds.
    tx_cur = desc;

    // Copy data to TX buffer
    memcpy(tx_buffers[tx_cur], data, len);

    // Set Physical Address and start transmission
    outl(rtl_dev.io_base + RTL_REG_TSAD0 + (tx_cur * 4), (uint32_t)tx_buffers[tx_cur]);
    outl(rtl_dev.io_base + RTL_REG_TSD0 + (tx_cur * 4), len);

    // Advance to next descriptor for next call
    tx_cur = (tx_cur + 1) % 4;
    net_if->tx_packets++;
    net_if->tx_bytes += len;
    stat_tx_packets++;

    return 0;
}

// Optimized RX function - batch processing with minimal logging
void rtl8139_receive_packets() {
    if (!rtl_dev.io_base || !rx_buffer_aligned) return;

    // Check if there are packets to process by comparing the hardware write
    // pointer (CBR) against our software read pointer.
    //
    // BUG FIX: Previously this checked bit 0 of the CMD register (0x01 = RE,
    // Receiver Enable), which is ALWAYS 1 when the receiver is enabled.
    //
    // The RTL8139 uses a ring buffer: hardware writes packets and advances
    // CBR (Current Buffer Address, 0x3A). Software reads packets and advances
    // CAPR (Current Address of Packet Read, 0x38). If CAPR != CBR (mod 32768),
    // there is at least one packet waiting to be read.
    //
    // Note: CAPR is typically set to (packet_offset - 16) because the hardware
    // needs 16 bytes of margin. We account for this offset in the comparison.
    uint16_t cbr = inw(rtl_dev.io_base + 0x3A) % 32768;
    uint16_t capr = inw(rtl_dev.io_base + RTL_REG_CAPR);
    // CAPR is stored as (read_offset - 16) mod 32768. Recover the actual
    // read offset by adding 16.
    uint16_t read_offset = (capr + 16) % 32768;

    // Only log when a packet is actually waiting — avoids flooding the serial
    // output with "pkt_waiting=0" lines during idle polling.
    if (cbr != read_offset) {
        s_printf("[RTL8139] poll: current_packet_ptr=%d read_offset=%d cbr=%d (pkt_waiting=1)\n",
                 current_packet_ptr % 32768, read_offset, cbr);
    }

    if (cbr == read_offset) {
        return;  // No packets pending
    }

    int packets_processed = 0;

    // Process batch of packets — keep going while CBR != read pointer.
    while (packets_processed < RX_MAX_BATCH) {
        uint16_t cur_cbr = inw(rtl_dev.io_base + 0x3A) % 32768;
        uint16_t cur_capr = inw(rtl_dev.io_base + RTL_REG_CAPR);
        uint16_t cur_read_offset = (cur_capr + 16) % 32768;
        if (cur_cbr == cur_read_offset) {
            break;  // No more packets
        }

        uint16_t offset = current_packet_ptr % 32768;
        uint32_t header_val = *(uint32_t*)(rx_buffer_aligned + offset);
        uint16_t status = header_val & 0xFFFF;
        uint16_t length = (header_val >> 16) & 0xFFFF;

        // Stop if no valid packet at this position.
        if (!(status & 0x01) || length < 60 || length > 1536) {
            break;
        }

        // Check for runt or error packets by status
        int is_error = (status & 0x3E);  // Check error bits (1-5)

        if (is_error) {
#if RTL_DEBUG_ERRORS
            s_printf("[RTL8139] RX: Bad packet, skipping\n");
#endif
            consecutive_rx_errors++;
            stat_rx_errors++;
            
            // Only reset if we see too many consecutive errors
            if (consecutive_rx_errors >= MAX_CONSECUTIVE_ERRORS) {
#if RTL_DEBUG_ERRORS
                s_printf("[RTL8139] Too many errors, resetting RX\n");
#endif
                outb(rtl_dev.io_base + RTL_REG_CMD, 0x04);  // Disable RX
                for(volatile int i = 0; i < 100000; i++) asm volatile("pause");
                outw(rtl_dev.io_base + RTL_REG_CAPR, 32768 - 16);
                current_packet_ptr = 0;
                memset(rx_buffer_aligned, 0, RX_BUF_SIZE);
                outb(rtl_dev.io_base + RTL_REG_CMD, 0x0C);  // Re-enable RX+TX
                consecutive_rx_errors = 0;
                return;
            }
            
            // Unrecoverable bogus length?
            if (length == 0 || length > 16384) {
                consecutive_rx_errors = MAX_CONSECUTIVE_ERRORS; // Force reset
                continue;
            }

            // Skip this packet
            current_packet_ptr = ((current_packet_ptr + length + 4 + 3) & ~3) % 32768;
            outw(rtl_dev.io_base + RTL_REG_CAPR, current_packet_ptr - 16);
            continue;
        }
        
        // Reset consecutive error counter on valid packet
        consecutive_rx_errors = 0;

        // Packet data length (exclude 4 bytes CRC)
        uint32_t packet_len = length - 4;

        // Allocate packet buffer
        void* packet_copy = kmalloc(packet_len);
        if (packet_copy) {
            // Hardware wrap-around margin allows contiguous copy
            memcpy(packet_copy, rx_buffer_aligned + (offset + 4), packet_len);

            // Process packet through network stack
            net_handle_packet((uint8_t*)packet_copy, packet_len);
            kfree(packet_copy);
            
            rtl_if.rx_packets++;
            rtl_if.rx_bytes += packet_len;
            stat_rx_packets++;
        } else {
#if RTL_DEBUG_ERRORS
            s_printf("[RTL8139] RX Error: kmalloc failed\n");
#endif
            stat_rx_errors++;
        }

        // Update read pointer (CAPR)
        current_packet_ptr = ((current_packet_ptr + length + 4 + 3) & ~3) % 32768;
        outw(rtl_dev.io_base + RTL_REG_CAPR, current_packet_ptr - 16);
        
        packets_processed++;
    }
}

void rtl8139_handler() {
    if (!rtl_dev.io_base) return;
    uint16_t status = inw(rtl_dev.io_base + RTL_REG_ISR);
    if (!status) return;
    
    // Acknowledge interrupts
    outw(rtl_dev.io_base + RTL_REG_ISR, status);
    
    if (status & 0x01) rtl8139_receive_packets(); // ROK
    if (status & 0x10) outw(rtl_dev.io_base + RTL_REG_ISR, 0x10); // Overflow
}

void rtl8139_poll() {
    rtl8139_receive_packets();
}

// Flush all pending packets from the RX buffer by resetting the read/write
// pointers. This is called after a connection failure (e.g. TLS handshake
// failure) to clear stale packets that would otherwise pollute future
// network operations.
void rtl8139_flush_rx(void) {
    if (!rtl_dev.io_base || !rx_buffer_aligned) return;
    // Reset the read pointer to match the hardware write pointer
    uint16_t cbr = inw(rtl_dev.io_base + 0x3A) % 32768;
    current_packet_ptr = cbr;
    // Set CAPR to (cbr - 16) mod 32768 so read_offset = (CAPR + 16) % 32768 = cbr
    uint16_t capr_val = (cbr >= 16) ? (cbr - 16) : (32768 - 16 + cbr);
    outw(rtl_dev.io_base + RTL_REG_CAPR, capr_val);
    s_printf("[RTL8139] RX flushed: cbr=%d, capr=%d, current_packet_ptr=%d\n", cbr, capr_val, current_packet_ptr);
}

// Configure IP address (minimal logging)
void rtl8139_configure_ip(uint32_t ip, uint32_t gw, uint32_t mask) {
#if RTL_DEBUG_INIT
    s_printf("[RTL8139] configure_ip called\n");
#endif
    
    if (!rtl_dev.net_if) {
#if RTL_DEBUG_ERRORS
        s_printf("[RTL8139] ERROR: net_if is NULL!\n");
#endif
        return;
    }
    
    // Direct assignment to the pointer's fields
    rtl_dev.net_if->ip_addr = ip;
    rtl_dev.net_if->gateway = gw; 
    rtl_dev.net_if->netmask = mask;
    
    // Configure ARP with our IP
    extern void arp_configure(uint32_t, uint32_t, uint32_t);
    arp_configure(ip, gw, mask);
    
    // Update global variables used by net_get_ip() and socket creation
    extern ip_addr_t my_ip;
    extern ip_addr_t gateway_ip;
    my_ip.addr = ip;
    gateway_ip.addr = gw;
    
#if RTL_DEBUG_INIT
    s_printf("[RTL8139] IP configured\n");
#endif
}

void rtl8139_init(pci_device_t* dev) {
#if RTL_DEBUG_INIT
    s_printf("[RTL8139] Initializing...\n");
#endif
    pci_enable_bus_master(dev);
    
    rtl_dev.io_base = dev->bar[0] & ~3;
    
    // 1. Power On
    outb(rtl_dev.io_base + RTL_REG_CONFIG1, 0x00);

    // 2. Software Reset
    outb(rtl_dev.io_base + RTL_REG_CMD, 0x10);
    
    // Wait for RST bit to clear
    int timeout = 1000000;
    while((inb(rtl_dev.io_base + RTL_REG_CMD) & 0x10) && timeout--) {
        for(volatile int j = 0; j < 100; j++) asm volatile("pause");
    }
    if(timeout <= 0) {
#if RTL_DEBUG_ERRORS
        s_printf("[RTL8139] WARNING: Reset Timeout!\n");
#endif
    }
    
    // Delay after reset
    for(volatile int i = 0; i < 500000; i++) asm volatile("pause");
    
    // 3. Init Buffers - use statically allocated, 32KB-aligned buffer.
    // Previously used kmalloc which could fail when the heap was fragmented.
    // The static buffer in BSS is always available and properly aligned.
    rx_buffer_aligned = (uint8_t*)(((uint32_t)rx_buffer_storage + 32767) & ~32767);
    memset(rx_buffer_aligned, 0, RX_BUF_SIZE);
    
#if RTL_DEBUG_INIT
    s_printf("[RTL8139] rx_buffer using static storage at 0x%x\n", (uint32_t)rx_buffer_aligned);
#endif
    
    outl(rtl_dev.io_base + RTL_REG_RBSTART, (uint32_t)rx_buffer_aligned);
    
    // 4. Interrupts (ROK + TOK + RXOVW)
    outw(rtl_dev.io_base + RTL_REG_IMR, 0x0015); 
    
    // 5. Receive Config:
    // - Accept Physical Match, Multicast, Broadcast
    // - Do NOT Accept Runt (AR=0) or Error Packets (AER=0)
    // - 32KB + WRAP Mode
    // - Max DMA burst 512 bytes
    // - No FIFO threshold
    // WRAP bit (7) = 1, SIZE bits (12-11) = 2 (10 binary)
    outl(rtl_dev.io_base + RTL_REG_RCR, (6 << 13) | (2 << 11) | (1 << 7) | 0x0E);
    
    // Accept ALL multicast
    outl(rtl_dev.io_base + RTL_REG_MAR0, 0xFFFFFFFF);
    outl(rtl_dev.io_base + RTL_REG_MAR0 + 4, 0xFFFFFFFF);
    
    // 6. Transmit Configuration
    outl(rtl_dev.io_base + RTL_REG_TCR, 0x03000700); // Max DMA
    
    // 7. Configure Transmit Descriptors
    for (int i = 0; i < 4; i++) {
        memset(tx_buffers[i], 0, TX_BUF_SIZE);
        outl(rtl_dev.io_base + RTL_REG_TSAD0 + (i * 4), (uint32_t)tx_buffers[i]);
        outl(rtl_dev.io_base + RTL_REG_TSD0 + (i * 4), 0x2000);  // Set OWN bit
    }
    
    // 8. Enable RX/TX
    outb(rtl_dev.io_base + RTL_REG_CMD, 0x00);
    for(volatile int i=0; i<10000; i++) asm volatile("pause");
    outb(rtl_dev.io_base + RTL_REG_CMD, 0x0C);  // RX+TX enable
    
    // NOTE: CAPR initialization to (32768-16) was reverted because it caused
    // QEMU's RTL8139 emulation to stop receiving packets entirely. The
    // 6-zero-byte corruption is handled by application-level workarounds
    // (HTTP null-byte stripping, TLS record header reconstruction).
    current_packet_ptr = 0;
    
    // 9. Read MAC address
    for(int i=0; i<6; i++) {
        local_mac[i] = inb(rtl_dev.io_base + RTL_REG_IDR0 + i);
    }
    
#if RTL_DEBUG_INIT
    s_printf("[RTL8139] MAC read complete\n");
#endif
    
    // 10. Setup network interface
    rtl_if.send = rtl8139_send_wrapper;
    memcpy(rtl_if.mac, local_mac, 6);
    rtl_if.ip_addr = 0;
    rtl_if.gateway = 0;
    rtl_if.netmask = 0;
    rtl_if.tx_packets = 0;
    rtl_if.rx_packets = 0;
    rtl_if.tx_bytes = 0;
    rtl_if.rx_bytes = 0;
    strcpy(rtl_if.name, "eth0");
    rtl_if.next = 0;
    
    rtl_dev.net_if = &rtl_if;
    net_register_interface(&rtl_if);
    
    rtl_initialized = 1;

    // Register poll function with network abstraction layer
    extern void net_set_poll_func(void (*func)(void));
    net_set_poll_func(rtl8139_poll);

#if RTL_DEBUG_INIT
    s_printf("[RTL8139] Init Complete.\n");
#endif
}

// Get statistics (useful for debugging)
void rtl8139_get_stats(uint32_t* tx_packets, uint32_t* rx_packets, 
                       uint32_t* tx_errors, uint32_t* rx_errors) {
    if (tx_packets) *tx_packets = stat_tx_packets;
    if (rx_packets) *rx_packets = stat_rx_packets;
    if (tx_errors) *tx_errors = stat_tx_errors;
    if (rx_errors) *rx_errors = stat_rx_errors;
}
