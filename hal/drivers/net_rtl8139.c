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
//
// The RTL8139 has 4 TX descriptors. We round-robin through them.
// If the current descriptor is still busy (OWN=0), we try the next
// one. This avoids blocking and avoids the need for resets.
//
// Key insight: in QEMU, the NIC processes packets near-instantly.
// If OWN is 0, it means the NIC is currently transmitting a packet
// on that descriptor — not that it's "stuck". We just use a
// different descriptor. With 4 descriptors, we can always find a
// free one unless we're sending 4+ packets in a burst (rare).
//
// If ALL 4 descriptors are busy, we do a short spin-wait on the
// current one. This should complete quickly in QEMU.
int rtl8139_send_wrapper(net_if_t* net_if, uint8_t* data, uint32_t len) {
    if (rtl_dev.io_base == 0) return -1;
    if (len > 1792) len = 1792;
    if (len < 60) len = 60; // Min Ethernet size

    // Try to find a free descriptor (OWN bit = 1).
    // Round-robin through all 4 descriptors.
    int desc = -1;
    for (int i = 0; i < 4; i++) {
        int try_desc = (tx_cur + i) % 4;
        uint32_t tsd = inl(rtl_dev.io_base + RTL_REG_TSD0 + (try_desc * 4));
        if (tsd & (1 << 13)) {
            // OWN=1, driver owns this descriptor — use it
            desc = try_desc;
            break;
        }
    }

    if (desc < 0) {
        // All 4 descriptors busy. Short spin-wait on the current one.
        // In QEMU this should complete in a few iterations.
        int timeout = TX_TIMEOUT_CYCLES;
        uint32_t tsd;
        while (timeout--) {
            tsd = inl(rtl_dev.io_base + RTL_REG_TSD0 + (tx_cur * 4));
            if (tsd & (1 << 13)) break;
            asm volatile("pause");
        }
        if (timeout <= 0) {
            // Genuinely stuck. Force-reset all descriptors to OWN=1.
            // This is safe in QEMU — the NIC will accept the next write.
            #if RTL_DEBUG_ERRORS
            s_printf("[RTL8139] TX all descriptors busy, force-resetting\n");
            #endif
            for (int i = 0; i < 4; i++) {
                outl(rtl_dev.io_base + RTL_REG_TSAD0 + (i * 4),
                     (uint32_t)tx_buffers[i]);
                outl(rtl_dev.io_base + RTL_REG_TSD0 + (i * 4), 0x2000);
            }
            stat_tx_errors++;
            // Don't drop the packet — use the current descriptor
            desc = tx_cur;
        } else {
            desc = tx_cur;
        }
    }

    // Copy data to TX buffer
    memcpy(tx_buffers[desc], data, len);

    // Set Physical Address and start transmission.
    // Writing the length to TSD clears the OWN bit and starts TX.
    outl(rtl_dev.io_base + RTL_REG_TSAD0 + (desc * 4), (uint32_t)tx_buffers[desc]);
    outl(rtl_dev.io_base + RTL_REG_TSD0 + (desc * 4), len);

    // Advance to next descriptor for next time
    tx_cur = (desc + 1) % 4;
    net_if->tx_packets++;
    net_if->tx_bytes += len;
    stat_tx_packets++;

    return 0;
}

// Optimized RX function - batch processing with minimal logging
void rtl8139_receive_packets() {
    if (!rtl_dev.io_base || !rx_buffer_aligned) return;

    // Check if there are packets to process.
    //
    // BUG FIX: Previously this checked bit 0 of the CMD register (0x01 = RE,
    // Receiver Enable), which is ALWAYS 1 when the receiver is enabled. The
    // check was therefore inverted: it returned "no packets" whenever the
    // receiver was on, which is always. As a result, rtl8139_poll() (called
    // from net_poll() in every k_connect / k_recvfrom / DNS loop iteration)
    // NEVER processed any packets — the function returned immediately every
    // time. The only way packets got processed was via the ISR handler
    // (rtl8139_handler) firing on the ROK interrupt, which was unreliable
    // and caused SYN-ACKs to arrive "late" (actually they were in the RX
    // buffer the whole time, just never drained by polling).
    //
    // The RTL8139 has no "buffer empty" bit in the CMD register. The correct
    // way to poll is to check the packet header at the current read position:
    // if the ROK bit (bit 0 of the status field) is set, a valid packet is
    // waiting. We also accept the ROK bit in the ISR as a secondary trigger.
    uint16_t offset = current_packet_ptr % 32768;
    uint32_t header_val = *(uint32_t*)(rx_buffer_aligned + offset);
    uint16_t status = header_val & 0xFFFF;
    uint16_t length = (header_val >> 16) & 0xFFFF;

    // No valid packet waiting if ROK bit is not set, or length is implausible.
    // Note: we must also check the ISR ROK bit because the status field can
    // be stale (leftover from a previous packet that we already processed).
    uint16_t isr = inw(rtl_dev.io_base + RTL_REG_ISR);
    if (!(status & 0x01) && !(isr & 0x01)) {
        return;  // No packets
    }
    // If ISR says ROK but status doesn't, the header may not be written yet;
    // try anyway — the loop below will validate the header before using it.

    int packets_processed = 0;

    // Process batch of packets. The loop condition checks both the ISR ROK
    // bit and the status field at the current read position.
    while (packets_processed < RX_MAX_BATCH) {
        offset = current_packet_ptr % 32768;
        header_val = *(uint32_t*)(rx_buffer_aligned + offset);
        status = header_val & 0xFFFF;
        length = (header_val >> 16) & 0xFFFF;

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
                // CRITICAL: CAPR must be (offset - 16) due to the RTL8139
                // hardware quirk. Writing 0 instead of (0 - 16) = 0xFFF0
                // leaves the read pointer misaligned, causing the driver
                // to re-read stale packets. This was Bug 2 in the user's
                // analysis: the reset cleared hardware CAPR to 0 but the
                // software current_packet_ptr was also 0, so they were
                // "in sync" — but the hardware expects CAPR=offset-16,
                // not CAPR=offset. With CAPR=0, hardware reads from
                // offset 0+16=16, but software reads from offset 0.
                outw(rtl_dev.io_base + RTL_REG_CAPR, (uint16_t)(0 - 16));
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

        // CRITICAL: Advance the read pointer and update CAPR BEFORE
        // processing the packet. The previous code called net_handle_packet
        // first, then advanced the pointer. But net_handle_packet can
        // trigger TX (e.g. a TCP ACK), which can trigger an RX interrupt,
        // which calls rtl8139_receive_packets RECURSIVELY. The recursive
        // call would see the same packet at the old current_packet_ptr
        // (because we hadn't advanced it yet) and re-deliver it — causing
        // the infinite dup-segment storm seen in the logs.
        //
        // By advancing the pointer first, the recursive call will see
        // the next packet (or no packet) instead of re-reading this one.
        uint16_t next_ptr = (uint16_t)(((current_packet_ptr + length + 4 + 3) & ~3) % 32768);
        current_packet_ptr = next_ptr;
        outw(rtl_dev.io_base + RTL_REG_CAPR, current_packet_ptr - 16);

        // Now safe to process the packet — any nested poll will see
        // the updated read pointer.
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

// ---------------------------------------------------------------------------
// rtl8139_flush_rx
//
// Drains any packets sitting in the NIC's RX ring buffer without delivering
// them to the network stack. Used by the Browser app (and any other network
// client) after a failed connection attempt, where stale packets from the
// previous connection might still be sitting in the ring and would otherwise
// be delivered to the next connection — confusing the TCP state machine and
// the TLS parser (the "6 zero bytes" bug in the original tls_recv_record
// was almost certainly caused by stale bytes from a previous connection
// arriving before the new connection's ClientHello was even sent).
//
// Strategy: poll up to 256 packets in a tight loop, discarding each one
// by advancing CAPR past it. We don't call net_handle_packet — the goal
// is to DROP, not deliver. We also reset the software-side
// current_packet_ptr to match the new CAPR so the next real poll starts
// at the right place.
//
// If the hardware reports no packets for 4 consecutive polls, we declare
// the buffer flushed and return.
// ---------------------------------------------------------------------------
void rtl8139_flush_rx(void) {
    if (rtl_dev.io_base == 0) return;

    int flushed = 0;
    int empty_polls = 0;

    while (empty_polls < 4 && flushed < 256) {
        uint16_t cbr = inw(rtl_dev.io_base + RTL_REG_CBR) % 32768;
        // current_packet_ptr is where the software last left off reading.
        // If CBR == current_packet_ptr (modulo 32768), there's nothing to read.
        if (cbr == current_packet_ptr) {
            empty_polls++;
            // Tiny pause to let the NIC catch up if a packet is mid-receive.
            for (volatile int i = 0; i < 100; i++) {}
            continue;
        }
        empty_polls = 0;

        // Read the packet header at current_packet_ptr.
        uint16_t offset = current_packet_ptr % 32768;
        uint16_t header = inw(rtl_dev.io_base + 0 + 0);  // unused; we read from rx_buffer_aligned
        (void)header;

        // Read the 4-byte packet header from the RX buffer.
        // Layout: [status_lo, status_hi, len_lo, len_hi]
        uint8_t* hdr_ptr = rx_buffer_aligned + offset;
        uint16_t status = hdr_ptr[0] | (hdr_ptr[1] << 8);
        uint16_t length = hdr_ptr[2] | (hdr_ptr[3] << 8);

        // Validate. If invalid, we can't safely advance — bail out and
        // let the regular RX path deal with it on the next poll.
        if (!(status & 0x01) || length < 60 || length > 1536) {
            // Bogus header — just advance one packet's worth to avoid
            // an infinite loop. The regular receive path will handle
            // error recovery on the next real poll.
            break;
        }

        // Advance past this packet. Same formula as rtl8139_receive_packets:
        //   new_ptr = (old + length + 4 + 3) & ~3, mod 32768
        current_packet_ptr = (uint16_t)(((current_packet_ptr + length + 4 + 3) & ~3) % 32768);
        outw(rtl_dev.io_base + RTL_REG_CAPR, current_packet_ptr - 16);

        flushed++;
    }

    if (flushed > 0) {
        s_printf("[RTL8139] RX flushed: %d packets discarded\n", flushed);
    }
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
    
    // Initialize CAPR to (0 - 16) to match current_packet_ptr=0.
    // The RTL8139 hardware quirk: the actual read position is CAPR+16.
    // So to read from offset 0, we write CAPR = 0 - 16 = 0xFFF0.
    current_packet_ptr = 0;
    outw(rtl_dev.io_base + RTL_REG_CAPR, (uint16_t)(0 - 16));
    
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
