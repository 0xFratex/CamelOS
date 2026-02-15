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
#define RTL_DEBUG_INIT        0    // Log initialization
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

// Buffer size
#define RX_BUF_SIZE (8192 + 16 + 1500)
#define TX_BUF_SIZE 2048

// Performance tuning
#define TX_TIMEOUT_CYCLES     100000
#define RX_MAX_BATCH          32     // Max packets to process per poll

rtl8139_dev_t rtl_dev;  // Global device structure
static int rtl_initialized = 0;

// Local MAC address storage
static uint8_t local_mac[6];

static uint8_t tx_buffers[4][TX_BUF_SIZE] __attribute__((aligned(4)));
// CRITICAL: RTL8139 requires RX buffer to be 8KB aligned (lower 13 bits = 0)
static uint8_t rx_buffer[RX_BUF_SIZE + 8192] __attribute__((aligned(8192)));
static uint8_t* rx_buffer_aligned = 0;
static uint16_t current_packet_ptr = 0;
static int tx_cur = 0;
net_if_t rtl_if;

// Statistics
static uint32_t stat_tx_packets = 0;
static uint32_t stat_rx_packets = 0;
static uint32_t stat_tx_errors = 0;
static uint32_t stat_rx_errors = 0;

// Optimized TX function - minimal logging
int rtl8139_send_wrapper(net_if_t* net_if, uint8_t* data, uint32_t len) {
    if (rtl_dev.io_base == 0) return -1;
    if (len > 1792) len = 1792;
    if (len < 60) len = 60; // Min Ethernet size

    // Read current Status
    uint32_t tsd = inl(rtl_dev.io_base + RTL_REG_TSD0 + (tx_cur * 4));

    // Wait for OWN bit (Bit 13) to be 1 (Driver owns buffer)
    int timeout = TX_TIMEOUT_CYCLES;
    while (!(tsd & (1 << 13)) && timeout--) {
        tsd = inl(rtl_dev.io_base + RTL_REG_TSD0 + (tx_cur * 4));
        asm volatile("pause");
    }

    if (timeout <= 0) {
#if RTL_DEBUG_ERRORS
        s_printf("[RTL8139] TX timeout on descriptor\n");
#endif
        tx_cur = (tx_cur + 1) % 4;
        stat_tx_errors++;
        return -1;
    }

    // Copy data to TX buffer
    memcpy(tx_buffers[tx_cur], data, len);

    // Set Physical Address and start transmission
    outl(rtl_dev.io_base + RTL_REG_TSAD0 + (tx_cur * 4), (uint32_t)tx_buffers[tx_cur]);
    outl(rtl_dev.io_base + RTL_REG_TSD0 + (tx_cur * 4), len);
    
    // Wait for transmission to complete
    timeout = TX_TIMEOUT_CYCLES;
    while (timeout--) {
        uint32_t tsd_status = inl(rtl_dev.io_base + RTL_REG_TSD0 + (tx_cur * 4));
        if (tsd_status & (1 << 13)) break;
        asm volatile("pause");
    }
    
    if (timeout <= 0) {
#if RTL_DEBUG_ERRORS
        s_printf("[RTL8139] TX completion timeout\n");
#endif
        stat_tx_errors++;
    }
    
    tx_cur = (tx_cur + 1) % 4;
    net_if->tx_packets++;
    net_if->tx_bytes += len;
    stat_tx_packets++;

    return 0;
}

// Optimized RX function - batch processing with minimal logging
void rtl8139_receive_packets() {
    if (!rtl_dev.io_base || !rx_buffer_aligned) return;

    // Check if RX buffer is empty (Bit 0 of CMD)
    uint8_t cmd = inb(rtl_dev.io_base + RTL_REG_CMD);
    if (cmd & 0x01) {
        return;  // No packets
    }

    int packets_processed = 0;
    
    // Process batch of packets
    while ((inb(rtl_dev.io_base + RTL_REG_CMD) & 0x01) == 0 && 
           packets_processed < RX_MAX_BATCH) {
        
        uint16_t offset = current_packet_ptr % 8192;
        
        // Header: [Status 16b] [Length 16b]
        // Read as 32-bit to ensure atomicity
        uint32_t header_val = *(uint32_t*)(rx_buffer_aligned + offset);
        uint16_t status = header_val & 0xFFFF;
        uint16_t length = (header_val >> 16) & 0xFFFF;

        // Sanity check - RTL8139 length includes:
        // - Packet data (60-1514 bytes for Ethernet)
        // - 4 bytes CRC (added by hardware)
        // - Does NOT include the 4-byte header
        // So valid range is approximately 64-1520
        if (length < 60 || length > 1520) {
#if RTL_DEBUG_ERRORS
            s_printf("[RTL8139] RX Error: Invalid length\n");
#endif
            // Reset RX logic
            outb(rtl_dev.io_base + RTL_REG_CMD, 0x04);  // Disable RX
            for(volatile int i = 0; i < 10000; i++) asm volatile("pause");
            outw(rtl_dev.io_base + RTL_REG_CAPR, 0);
            current_packet_ptr = 0;
            // Clear buffer
            memset(rx_buffer_aligned, 0, RX_BUF_SIZE);
            outb(rtl_dev.io_base + RTL_REG_CMD, 0x0C);  // Re-enable RX+TX
            stat_rx_errors++;
            return;
        }

        // Check ROK (Receive OK) - Bit 0 of status
        if (status & 0x01) {
            // Length includes packet + CRC
            // Actual packet data is length - 4 (CRC is at the end)
            uint32_t packet_len = length - 4;

            // Allocate packet buffer
            void* packet_copy = kmalloc(packet_len);
            if (packet_copy) {
                // Handle Ring Buffer Wrap
                // Packet data starts after 4-byte header
                if (offset + 4 + packet_len > 8192) {
                    uint32_t chunk1 = 8192 - (offset + 4);
                    memcpy(packet_copy, rx_buffer_aligned + offset + 4, chunk1);
                    memcpy((uint8_t*)packet_copy + chunk1, rx_buffer_aligned, packet_len - chunk1);
                } else {
                    memcpy(packet_copy, rx_buffer_aligned + offset + 4, packet_len);
                }

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
        } else {
#if RTL_DEBUG_ERRORS
            s_printf("[RTL8139] RX Error: Bad status\n");
#endif
            stat_rx_errors++;
        }

        // Update read pointer - RTL8139 requires:
        // 1. Move past this packet (offset + 4 + length)
        // 2. Align to 4-byte boundary
        // 3. Write CAPR as (new_offset - 16)
        current_packet_ptr = ((current_packet_ptr + length + 4 + 3) & ~3) % 8192;
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
    
    // 3. Init Buffers - ensure 8KB alignment
    rx_buffer_aligned = (uint8_t*)(((uint32_t)rx_buffer + 8191) & ~8191);
    memset(rx_buffer_aligned, 0, RX_BUF_SIZE);
    
#if RTL_DEBUG_INIT
    s_printf("[RTL8139] RX buffer aligned\n");
#endif
    
    // Verify alignment
    if ((uint32_t)rx_buffer_aligned & 0x1FFF) {
#if RTL_DEBUG_ERRORS
        s_printf("[RTL8139] ERROR: RX buffer not 8KB aligned!\n");
#endif
    }
    
    outl(rtl_dev.io_base + RTL_REG_RBSTART, (uint32_t)rx_buffer_aligned);
    
    // 4. Interrupts (ROK + TOK)
    outw(rtl_dev.io_base + RTL_REG_IMR, 0x0005); 
    
    // 5. Receive Config - Accept all packets for compatibility
    outl(rtl_dev.io_base + RTL_REG_RCR, 0x0000003F);
    
    // Accept ALL multicast
    outl(rtl_dev.io_base + RTL_REG_MAR0, 0xFFFFFFFF);
    outl(rtl_dev.io_base + RTL_REG_MAR0 + 4, 0xFFFFFFFF);
    
    // 6. Transmit Configuration
    outl(rtl_dev.io_base + RTL_REG_TCR, 0x00000700);
    
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
