// hal/drivers/net_rtl8169.c
#include "net_rtl8169.h"
#include "serial.h"
#include "pci.h"
#include <memory.h>
#include <string.h>
#include <net.h>
#include <net_if.h>
#include <ports.h>
#include <apic.h>

// Registers
#define R8169_IDR0      0x00
#define R8169_TNPDS     0x20 
#define R8169_CMD       0x37
#define R8169_IMR       0x3C
#define R8169_ISR       0x3E
#define R8169_TCR       0x40
#define R8169_RCR       0x44
#define R8169_9346CR    0x50
#define R8169_RDS       0xE4
#define R8169_RMS       0xDA
#define R8169_MTPS      0xEC

// Descriptors
#define NUM_RX_DESC 64
#define NUM_TX_DESC 64
#define RX_BUF_SIZE 1536
#define DESC_OWN (1U << 31)
#define DESC_EOR (1U << 30)
#define DESC_FS  (1U << 29)
#define DESC_LS  (1U << 28)

static uint32_t io_base = 0;
static net_if_t rtl_if;
static volatile rtl8169_desc_t* rx_descs;
static volatile rtl8169_desc_t* tx_descs;
static uint8_t* rx_buffers[NUM_RX_DESC];
static uint8_t* tx_buffers[NUM_TX_DESC];
static int cur_rx = 0;
static int cur_tx = 0;

void r8169_unlock() { outb(io_base + R8169_9346CR, 0xC0); }
void r8169_lock()   { outb(io_base + R8169_9346CR, 0x00); }

void rtl8169_handler() {
    extern void int_to_str(int, char*);

    if (!io_base) return;

    uint16_t status = inw(io_base + R8169_ISR);
    outw(io_base + R8169_ISR, status);

    if (status != 0) {
        s_printf("[R8169] ISR Status=0x");
        char buf[8]; int_to_str(status, buf); s_printf(buf); s_printf("\n");
    }

    if (status & 0x01) {
        while ((rx_descs[cur_rx].cmd_status & DESC_OWN) == 0) {
            uint32_t cmd = rx_descs[cur_rx].cmd_status;
            
            if ((cmd & DESC_FS) && (cmd & DESC_LS)) {
                uint32_t len = cmd & 0x3FFF;
                if (len > 4) {
                    s_printf("[R8169] RX OK. Len=");
                    char buf[8]; int_to_str(len-4, buf); s_printf(buf); s_printf("\n");
                    
                    net_handle_packet(rx_buffers[cur_rx], len - 4);
                    rtl_if.rx_packets++;
                    rtl_if.rx_bytes += len - 4;
                }
            }

            uint32_t new_cmd = DESC_OWN | (RX_BUF_SIZE & 0x1FFF);
            if (cur_rx == NUM_RX_DESC - 1) new_cmd |= DESC_EOR;
            rx_descs[cur_rx].cmd_status = new_cmd;

            cur_rx = (cur_rx + 1) % NUM_RX_DESC;
        }
    }
    
    apic_send_eoi();
}

int rtl8169_send(net_if_t* net_if, uint8_t* data, uint32_t len) {
    if (!io_base) return -1;
    if (len > RX_BUF_SIZE) len = RX_BUF_SIZE;

    if (tx_descs[cur_tx].cmd_status & DESC_OWN) {
        s_printf("[R8169] TX Busy!\n");
        return -1;
    }

    s_printf("[R8169] TX Packet Len=");
    char buf[8]; extern void int_to_str(int, char*); int_to_str(len, buf); s_printf(buf); s_printf("\n");

    memcpy(tx_buffers[cur_tx], data, len);

    uint32_t cmd = DESC_OWN | DESC_FS | DESC_LS | (len & 0xFFFF);
    if (cur_tx == NUM_TX_DESC - 1) cmd |= DESC_EOR;

    tx_descs[cur_tx].buf_addr_lo = (uint32_t)tx_buffers[cur_tx];
    tx_descs[cur_tx].cmd_status = cmd;

    outb(io_base + 0x38, 0x40); 

    cur_tx = (cur_tx + 1) % NUM_TX_DESC;
    rtl_if.tx_packets++;
    return 0;
}

void rtl8169_init(pci_device_t* dev) {
    s_printf("[R8169] Initializing Driver...\n");
    pci_enable_bus_master(dev);
    io_base = dev->bar[0] & ~3;

    ioapic_set_gsi_redirect(45, 0x80, 0, 0, 0); 

    outb(io_base + R8169_CMD, 0x10);
    for(volatile int i=0; i<100000; i++) {
        if(!(inb(io_base + R8169_CMD) & 0x10)) break;
    }

    rx_descs = (volatile rtl8169_desc_t*)kmalloc_a(sizeof(rtl8169_desc_t) * NUM_RX_DESC);
    tx_descs = (volatile rtl8169_desc_t*)kmalloc_a(sizeof(rtl8169_desc_t) * NUM_TX_DESC);

    for(int i=0; i<NUM_RX_DESC; i++) {
        rx_buffers[i] = (uint8_t*)kmalloc(RX_BUF_SIZE);
        rx_descs[i].buf_addr_lo = (uint32_t)rx_buffers[i];
        rx_descs[i].buf_addr_hi = 0;
        uint32_t cmd = DESC_OWN | (RX_BUF_SIZE & 0x1FFF);
        if(i == NUM_RX_DESC-1) cmd |= DESC_EOR;
        rx_descs[i].cmd_status = cmd;
    }

    for(int i=0; i<NUM_TX_DESC; i++) {
        tx_buffers[i] = (uint8_t*)kmalloc(RX_BUF_SIZE);
        tx_descs[i].cmd_status = 0;
    }

    r8169_unlock();

    outb(io_base + R8169_CMD, 0x0C); 
    outw(io_base + R8169_RMS, RX_BUF_SIZE); 
    outb(io_base + R8169_MTPS, 0x3B);
    outl(io_base + R8169_TCR, 0x03000700);
    outl(io_base + R8169_RCR, 0x0000E70F); 

    outl(io_base + R8169_RDS, (uint32_t)rx_descs);
    outl(io_base + R8169_RDS+4, 0);
    outl(io_base + R8169_TNPDS, (uint32_t)tx_descs);
    outl(io_base + R8169_TNPDS+4, 0);

    outw(io_base + R8169_IMR, 0x0005); 

    r8169_lock();

    memset(&rtl_if, 0, sizeof(net_if_t));
    strcpy(rtl_if.name, "eth0");
    for(int i=0; i<6; i++) rtl_if.mac[i] = inb(io_base + i);
    rtl_if.send = rtl8169_send;
    rtl_if.driver_state = dev;
    rtl_if.is_up = 1;

    net_register_interface(&rtl_if);
    s_printf("[R8169] Driver Loaded. IRQ 45 -> Vec 0x80.\n");
    
    net_dhcp_discover();
}

// Poll function for RTL8169 - similar to RTL8139
void rtl8169_poll() {
    if (!io_base) return;
    
    // Read and acknowledge interrupt status
    uint16_t status = inw(io_base + R8169_ISR);
    if (status) {
        outw(io_base + R8169_ISR, status);
        
        // Process receive packets if RX interrupt
        if (status & 0x01) {
            // Process all available receive descriptors
            while ((rx_descs[cur_rx].cmd_status & DESC_OWN) == 0) {
                uint32_t cmd = rx_descs[cur_rx].cmd_status;
                
                if ((cmd & DESC_FS) && (cmd & DESC_LS)) {
                    uint32_t len = cmd & 0x3FFF;
                    if (len > 4) {
                        net_handle_packet(rx_buffers[cur_rx], len - 4);
                        rtl_if.rx_packets++;
                        rtl_if.rx_bytes += len - 4;
                    }
                }

                // Return descriptor to NIC
                uint32_t new_cmd = DESC_OWN | (RX_BUF_SIZE & 0x1FFF);
                if (cur_rx == NUM_RX_DESC - 1) new_cmd |= DESC_EOR;
                rx_descs[cur_rx].cmd_status = new_cmd;

                cur_rx = (cur_rx + 1) % NUM_RX_DESC;
            }
        }
    }
}