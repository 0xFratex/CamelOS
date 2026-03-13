// hal/drivers/net_e1000.c - Intel e1000/e1000e Gigabit Ethernet Driver
// Supports Intel 8254x, 8257x, 8258x, I21x, I21x-V series
// This driver enables networking on real hardware

#include "net_e1000.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include "../../core/net.h"
#include "../../core/net_if.h"
#include "../cpu/timer.h"
#include "../cpu/paging.h"
#include "../../sys/io_ports.h"
#include "serial.h"
#include "pci.h"

// ============================================================================
// DEBUG CONFIGURATION
// ============================================================================
#define E1000_DEBUG_ENABLED     0
#define E1000_DEBUG_INIT        1
#define E1000_DEBUG_RX          0
#define E1000_DEBUG_TX          0

// ============================================================================
// REGISTER DEFINITIONS
// ============================================================================

// Control Register bits
#define E1000_CTRL_FD           0x00000001  // Full Duplex
#define E1000_CTRL_LRST         0x00000008  // Link Reset
#define E1000_CTRL_ASDE         0x00000020  // Auto-Speed Detection Enable
#define E1000_CTRL_SLU          0x00000040  // Set Link Up
#define E1000_CTRL_ILOS         0x00000080  // Invert Loss of Signal
#define E1000_CTRL_SPEED_MASK   0x00000300
#define E1000_CTRL_SPEED_10     0x00000000
#define E1000_CTRL_SPEED_100    0x00000100
#define E1000_CTRL_SPEED_1000   0x00000200
#define E1000_CTRL_FRCSPD       0x00000800  // Force Speed
#define E1000_CTRL_FRCDPLX      0x00001000  // Force Duplex
#define E1000_CTRL_RST          0x00004000  // Device Reset
#define E1000_CTRL_RFCE         0x00008000  // Receive Flow Control Enable
#define E1000_CTRL_TFCE         0x00010000  // Transmit Flow Control Enable
#define E1000_CTRL_VME          0x40000000  // VLAN Mode Enable
#define E1000_CTRL_PHY_RST      0x80000000  // PHY Reset

// Status Register bits
#define E1000_STATUS_FD         0x00000001  // Full Duplex
#define E1000_STATUS_LU         0x00000002  // Link Up
#define E1000_STATUS_FUNC_MASK  0x0000000C
#define E1000_STATUS_TXOFF      0x00000010  // Transmission Paused
#define E1000_STATUS_SPEED_MASK 0x000000C0
#define E1000_STATUS_SPEED_10   0x00000000
#define E1000_STATUS_SPEED_100  0x00000040
#define E1000_STATUS_SPEED_1000 0x00000080
#define E1000_STATUS_ASDV_MASK  0x00000300  // Auto-Speed Detection Value
#define E1000_STATUS_PHYRA      0x00000400  // PHY Reset Asserted
#define E1000_STATUS_GIO_M_ENA  0x00000800  // GIO Master Enable

// Interrupt Mask bits
#define E1000_IMS_TXDW          0x00000001  // Transmit Descriptor Written Back
#define E1000_IMS_TXQE          0x00000002  // Transmit Queue Empty
#define E1000_IMS_LSC           0x00000004  // Link Status Change
#define E1000_IMS_RXDMT0        0x00000010  // Receive Descriptor Minimum Threshold
#define E1000_IMS_RXO           0x00000040  // Receiver Overrun
#define E1000_IMS_RXT0          0x00000080  // Receiver Timer Interrupt
#define E1000_IMS_MDAC          0x00000200  // MDI Access Complete
#define E1000_IMS_RXCFG         0x00000400  // Receive /C/ ordered sets
#define E1000_IMS_PHYINT        0x00001000  // PHY Interrupt
#define E1000_IMS_GPI_EN0       0x00002000  // General Purpose Interrupt 0
#define E1000_IMS_GPI_EN1       0x00004000  // General Purpose Interrupt 1
#define E1000_IMS_GPI_EN2       0x00008000  // General Purpose Interrupt 2
#define E1000_IMS_GPI_EN3       0x00010000  // General Purpose Interrupt 3

// Receive Control bits
#define E1000_RCTL_EN           0x00000002  // Receiver Enable
#define E1000_RCTL_SBP          0x00000004  // Store Bad Packets
#define E1000_RCTL_UPE          0x00000008  // Unicast Promiscuous Enabled
#define E1000_RCTL_MPE          0x00000010  // Multicast Promiscuous Enabled
#define E1000_RCTL_LPE          0x00000020  // Long Packet Reception Enable
#define E1000_RCTL_LBM_MASK     0x000000C0  // Loopback Mode
#define E1000_RCTL_LBM_NORMAL   0x00000000
#define E1000_RCTL_RDMTS_MASK   0x00000300  // Receive Descriptor Minimum Threshold Size
#define E1000_RCTL_RDMTS_HALF   0x00000000
#define E1000_RCTL_RDMTS_QUART  0x00000100
#define E1000_RCTL_RDMTS_EIGHTH 0x00000200
#define E1000_RCTL_MO_MASK      0x00003000  // Multicast Offset
#define E1000_RCTL_BAM          0x00008000  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_MASK   0x00030000  // Receive Buffer Size
#define E1000_RCTL_BSIZE_2048   0x00000000
#define E1000_RCTL_BSIZE_1024   0x00010000
#define E1000_RCTL_BSIZE_512    0x00020000
#define E1000_RCTL_BSIZE_256    0x00030000
#define E1000_RCTL_VFE          0x00040000  // VLAN Filter Enable
#define E1000_RCTL_CFIEN        0x00080000  // Canonical Form Indicator Enable
#define E1000_RCTL_CFI          0x00100000  // Canonical Form Indicator
#define E1000_RCTL_DPF          0x00400000  // Discard Pause Frames
#define E1000_RCTL_PMCF         0x00800000  // Pass MAC Control Frames
#define E1000_RCTL_BSEX         0x02000000  // Buffer Size Extension
#define E1000_RCTL_SECRC        0x04000000  // Strip Ethernet CRC

// Transmit Control bits
#define E1000_TCTL_EN           0x00000002  // Transmit Enable
#define E1000_TCTL_PSP          0x00000008  // Pad Short Packets
#define E1000_TCTL_CT_MASK      0x00000FF0  // Collision Threshold
#define E1000_TCTL_COLD_MASK    0x003FF000  // Collision Distance
#define E1000_TCTL_SWXOFF       0x00400000  // Software XOFF Transmission
#define E1000_TCTL_RTLC         0x01000000  // Re-transmit on Late Collision

// Descriptor status bits
#define E1000_TXD_STAT_DD       0x00000001  // Descriptor Done
#define E1000_TXD_STAT_EC       0x00000002  // Excess Collisions
#define E1000_TXD_STAT_LC       0x00000004  // Late Collision
#define E1000_TXD_STAT_TU       0x00000008  // Transmit Underrun

#define E1000_RXD_STAT_DD       0x01        // Descriptor Done
#define E1000_RXD_STAT_EOP      0x02        // End of Packet

// Command bits
#define E1000_TXD_CMD_EOP       0x01000000  // End of Packet
#define E1000_TXD_CMD_IFCS      0x02000000  // Insert FCS
#define E1000_TXD_CMD_IC        0x04000000  // Insert Checksum
#define E1000_TXD_CMD_RS        0x08000000  // Report Status
#define E1000_TXD_CMD_RPS       0x10000000  // Report Packet Sent
#define E1000_TXD_CMD_DEXT      0x20000000  // Descriptor Extension
#define E1000_TXD_CMD_VLE       0x40000000  // VLAN Enable
#define E1000_TXD_CMD_IDE       0x80000000  // Interrupt Delay Enable

// Register offsets
#define E1000_CTRL      0x0000  // Device Control
#define E1000_STATUS    0x0008  // Device Status
#define E1000_EECD      0x0010  // EEPROM/FLASH Control
#define E1000_EERD      0x0014  // EEPROM Read
#define E1000_CTRL_EXT  0x0018  // Extended Device Control
#define E1000_MDIC      0x0020  // MDI Control
#define E1000_FCAL      0x0028  // Flow Control Address Low
#define E1000_FCAH      0x002C  // Flow Control Address High
#define E1000_FCT       0x0030  // Flow Control Type
#define E1000_VET       0x0038  // VLAN Ether Type
#define E1000_ICR       0x00C0  // Interrupt Cause Read
#define E1000_ITR       0x00C4  // Interrupt Throttling Rate
#define E1000_ICS       0x00C8  // Interrupt Cause Set
#define E1000_IMS       0x00D0  // Interrupt Mask Set
#define E1000_IMC       0x00D8  // Interrupt Mask Clear
#define E1000_RCTL      0x0100  // RX Control
#define E1000_FCTTV     0x0170  // Flow Control Transmit Timer Value
#define E1000_TXCW      0x0178  // Transmit Configuration Word
#define E1000_RXCW      0x0180  // Receive Configuration Word
#define E1000_TCTL      0x0400  // TX Control
#define E1000_TIPG      0x0410  // TX Inter-packet Gap
#define E1000_TDBAL     0x3800  // TX Descriptor Base Low
#define E1000_TDBAH     0x3804  // TX Descriptor Base High
#define E1000_TDLEN     0x3808  // TX Descriptor Length
#define E1000_TDH       0x3810  // TX Descriptor Head
#define E1000_TDT       0x3818  // TX Descriptor Tail
#define E1000_TIDV      0x3820  // TX Interrupt Delay Value
#define E1000_TXDCTL    0x3828  // TX Descriptor Control
#define E1000_TADV      0x382C  // TX Absolute Interrupt Delay Value
#define E1000_RDBAL     0x2800  // RX Descriptor Base Low
#define E1000_RDBAH     0x2804  // RX Descriptor Base High
#define E1000_RDLEN     0x2808  // RX Descriptor Length
#define E1000_RDH       0x2810  // RX Descriptor Head
#define E1000_RDT       0x2818  // RX Descriptor Tail
#define E1000_RDTR      0x2820  // RX Interrupt Delay Timer
#define E1000_RXDCTL    0x2828  // RX Descriptor Control
#define E1000_RADV      0x282C  // RX Absolute Interrupt Delay Value
#define E1000_RSRPD     0x2C00  // RX Small Packet Detect Interrupt
#define E1000_RA        0x5400  // Receive Address (0-15)
#define E1000_MTA       0x5200  // Multicast Table Array
#define E1000_VFTA      0x5600  // VLAN Filter Table Array

// ============================================================================
// DESCRIPTOR STRUCTURES
// ============================================================================

typedef struct {
    uint64_t buffer_addr;     // Address of the descriptor's data buffer
    uint16_t length;          // Length of data in buffer
    uint16_t checksum;        // Packet checksum
    uint8_t status;           // Descriptor status
    uint8_t errors;           // Descriptor errors
    uint16_t special;         // Special fields
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t buffer_addr;     // Address of the descriptor's data buffer
    uint16_t length;          // Length of data in buffer
    uint8_t checksum_offset;  // Offset of checksum
    uint8_t cmd;              // Descriptor command
    uint8_t status;           // Descriptor status
    uint8_t checksum_start;   // Start of checksum
    uint16_t special;         // Special fields
} __attribute__((packed)) e1000_tx_desc_t;

// ============================================================================
// DRIVER STATE
// ============================================================================

#define E1000_NUM_RX_DESC  256
#define E1000_NUM_TX_DESC  256
#define E1000_BUFFER_SIZE  2048

typedef struct {
    // PCI info
    uint16_t pci_bus;
    uint16_t pci_dev;
    uint16_t pci_func;
    uint16_t vendor_id;
    uint16_t device_id;
    
    // Memory-mapped I/O
    uint32_t mmio_base;
    uint32_t mmio_size;
    uint8_t* mmio;
    
    // I/O port (legacy)
    uint32_t io_base;
    int use_mmio;
    
    // EEPROM/Flash
    uint16_t eeprom[64];
    int has_eeprom;
    
    // MAC address
    uint8_t mac_addr[6];
    
    // Descriptors
    e1000_rx_desc_t* rx_descs;
    e1000_tx_desc_t* tx_descs;
    uint32_t rx_desc_phys;
    uint32_t tx_desc_phys;
    
    // Buffers
    uint8_t* rx_buffers[E1000_NUM_RX_DESC];
    uint8_t* tx_buffers[E1000_NUM_TX_DESC];
    
    // Descriptor indices
    uint32_t rx_current;
    uint32_t tx_current;
    uint32_t tx_dirty;
    
    // Link status
    int link_up;
    int speed;
    int duplex;
    
    // Statistics
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    
    // Interface
    net_if_t netif;
    
    // Lock
    volatile int tx_lock;
    
} e1000_dev_t;

// Device instances
static e1000_dev_t e1000_devices[4];
static int e1000_device_count = 0;

// ============================================================================
// MMIO ACCESS FUNCTIONS
// ============================================================================

static inline void e1000_write_reg(e1000_dev_t* dev, uint32_t reg, uint32_t value) {
    if (dev->use_mmio) {
        *(volatile uint32_t*)(dev->mmio + reg) = value;
    } else {
        outl(dev->io_base, reg);
        outl(dev->io_base + 4, value);
    }
}

static inline uint32_t e1000_read_reg(e1000_dev_t* dev, uint32_t reg) {
    if (dev->use_mmio) {
        return *(volatile uint32_t*)(dev->mmio + reg);
    } else {
        outl(dev->io_base, reg);
        return inl(dev->io_base + 4);
    }
}

// ============================================================================
// EEPROM FUNCTIONS
// ============================================================================

static uint16_t e1000_read_eeprom(e1000_dev_t* dev, uint8_t addr) {
    uint32_t data;
    
    // Wait for EEPROM to be ready
    for (int i = 0; i < 1000; i++) {
        if (e1000_read_reg(dev, E1000_EECD) & 0x10) break;
    }
    
    // Request EEPROM read
    e1000_write_reg(dev, E1000_EERD, (addr << 8) | 1);
    
    // Wait for read to complete
    for (int i = 0; i < 1000; i++) {
        data = e1000_read_reg(dev, E1000_EERD);
        if (data & 0x10) {
            return (uint16_t)(data >> 16);
        }
    }
    
    return 0;
}

static void e1000_read_mac_addr(e1000_dev_t* dev) {
    // Try to read from EEPROM first
    if (dev->has_eeprom) {
        uint16_t word;
        word = e1000_read_eeprom(dev, 0);
        dev->mac_addr[0] = word & 0xFF;
        dev->mac_addr[1] = word >> 8;
        word = e1000_read_eeprom(dev, 1);
        dev->mac_addr[2] = word & 0xFF;
        dev->mac_addr[3] = word >> 8;
        word = e1000_read_eeprom(dev, 2);
        dev->mac_addr[4] = word & 0xFF;
        dev->mac_addr[5] = word >> 8;
    } else {
        // Read from receive address register
        uint32_t rar_low = e1000_read_reg(dev, E1000_RA);
        uint32_t rar_high = e1000_read_reg(dev, E1000_RA + 4);
        
        dev->mac_addr[0] = rar_low & 0xFF;
        dev->mac_addr[1] = (rar_low >> 8) & 0xFF;
        dev->mac_addr[2] = (rar_low >> 16) & 0xFF;
        dev->mac_addr[3] = (rar_low >> 24) & 0xFF;
        dev->mac_addr[4] = rar_high & 0xFF;
        dev->mac_addr[5] = (rar_high >> 8) & 0xFF;
    }
    
#if E1000_DEBUG_INIT
    // Debug: MAC address printed via serial
    serial_write_string("[E1000] MAC initialized\n");
#endif
}

// ============================================================================
// PHY FUNCTIONS
// ============================================================================

static uint16_t e1000_read_phy(e1000_dev_t* dev, uint8_t addr) {
    e1000_write_reg(dev, E1000_MDIC, addr | (1 << 23));
    
    for (int i = 0; i < 10000; i++) {
        uint32_t data = e1000_read_reg(dev, E1000_MDIC);
        if (data & (1 << 28)) {
            return (uint16_t)data;
        }
    }
    
    return 0;
}

static void e1000_write_phy(e1000_dev_t* dev, uint8_t addr, uint16_t value) {
    e1000_write_reg(dev, E1000_MDIC, addr | (1 << 23) | (value << 16) | (1 << 26));
    
    for (int i = 0; i < 10000; i++) {
        if (e1000_read_reg(dev, E1000_MDIC) & (1 << 28)) break;
    }
}

// ============================================================================
// RESET AND INITIALIZATION
// ============================================================================

static void e1000_reset(e1000_dev_t* dev) {
    // Issue global reset
    e1000_write_reg(dev, E1000_CTRL, E1000_CTRL_RST | E1000_CTRL_SLU);
    
    // Wait for reset to complete
    for (int i = 0; i < 100000; i++) {
        if (!(e1000_read_reg(dev, E1000_CTRL) & E1000_CTRL_RST)) break;
    }
    
    // Wait for auto-negotiation to complete
    for (int i = 0; i < 100000; i++) {
        if (e1000_read_reg(dev, E1000_STATUS) & E1000_STATUS_LU) break;
    }
}

static void e1000_init_rx(e1000_dev_t* dev) {
    // Allocate descriptor ring
    dev->rx_descs = (e1000_rx_desc_t*)kmalloc(sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC + 16);
    dev->rx_desc_phys = (uint32_t)dev->rx_descs;
    
    // Align to 16 bytes
    if (dev->rx_desc_phys & 0xF) {
        dev->rx_desc_phys = (dev->rx_desc_phys + 16) & ~0xF;
        dev->rx_descs = (e1000_rx_desc_t*)dev->rx_desc_phys;
    }
    
    memset(dev->rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    
    // Allocate buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        dev->rx_buffers[i] = (uint8_t*)kmalloc(E1000_BUFFER_SIZE);
        dev->rx_descs[i].buffer_addr = (uint64_t)(uint32_t)dev->rx_buffers[i];
        dev->rx_descs[i].status = 0;
    }
    
    dev->rx_current = 0;
    
    // Set up RX descriptor ring
    e1000_write_reg(dev, E1000_RDBAL, (uint32_t)dev->rx_desc_phys);
    e1000_write_reg(dev, E1000_RDBAH, 0);
    e1000_write_reg(dev, E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write_reg(dev, E1000_RDH, 0);
    e1000_write_reg(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);
    
    // Enable RX
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE |
                    E1000_RCTL_MPE | E1000_RCTL_LPE | E1000_RCTL_BAM |
                    E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    e1000_write_reg(dev, E1000_RCTL, rctl);
}

static void e1000_init_tx(e1000_dev_t* dev) {
    // Allocate descriptor ring
    dev->tx_descs = (e1000_tx_desc_t*)kmalloc(sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC + 16);
    dev->tx_desc_phys = (uint32_t)dev->tx_descs;
    
    // Align to 16 bytes
    if (dev->tx_desc_phys & 0xF) {
        dev->tx_desc_phys = (dev->tx_desc_phys + 16) & ~0xF;
        dev->tx_descs = (e1000_tx_desc_t*)dev->tx_desc_phys;
    }
    
    memset(dev->tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    
    // Allocate buffers
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        dev->tx_buffers[i] = (uint8_t*)kmalloc(E1000_BUFFER_SIZE);
    }
    
    dev->tx_current = 0;
    dev->tx_dirty = 0;
    
    // Set up TX descriptor ring
    e1000_write_reg(dev, E1000_TDBAL, (uint32_t)dev->tx_desc_phys);
    e1000_write_reg(dev, E1000_TDBAH, 0);
    e1000_write_reg(dev, E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write_reg(dev, E1000_TDH, 0);
    e1000_write_reg(dev, E1000_TDT, 0);
    
    // Enable TX
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (0x10 << 4) |   // Collision Threshold
                    (0x40 << 12);   // Collision Distance
    e1000_write_reg(dev, E1000_TCTL, tctl);
    
    // Set inter-packet gap
    e1000_write_reg(dev, E1000_TIPG, (6 << 20) | (8 << 10) | 10);
}

static void e1000_setup_interrupts(e1000_dev_t* dev) {
    // Enable interrupts
    e1000_write_reg(dev, E1000_IMS, E1000_IMS_RXT0 | E1000_IMS_RXO | 
                                      E1000_IMS_LSC | E1000_IMS_TXDW);
}

static void e1000_set_mac(e1000_dev_t* dev) {
    // Set receive address
    uint32_t rar_low = dev->mac_addr[0] | (dev->mac_addr[1] << 8) |
                       (dev->mac_addr[2] << 16) | (dev->mac_addr[3] << 24);
    uint32_t rar_high = dev->mac_addr[4] | (dev->mac_addr[5] << 8) | 0x80000000; // AV bit
    
    e1000_write_reg(dev, E1000_RA, rar_low);
    e1000_write_reg(dev, E1000_RA + 4, rar_high);
    
    // Clear multicast table
    for (int i = 0; i < 128; i++) {
        e1000_write_reg(dev, E1000_MTA + i * 4, 0);
    }
}

// ============================================================================
// PACKET TRANSMISSION
// ============================================================================

static int e1000_send(net_if_t* netif, uint8_t* data, uint32_t len) {
    e1000_dev_t* dev = (e1000_dev_t*)netif->driver_state;
    
    if (!dev->link_up) {
        return -1;
    }
    
    if (len > E1000_BUFFER_SIZE) {
        len = E1000_BUFFER_SIZE;
    }
    
    // Wait for available descriptor
    uint32_t next_tx = (dev->tx_current + 1) % E1000_NUM_TX_DESC;
    int timeout = 10000;
    while (timeout-- > 0) {
        if (dev->tx_descs[next_tx].status & E1000_TXD_STAT_DD) break;
        if (dev->tx_descs[dev->tx_current].status & E1000_TXD_STAT_DD) break;
    }
    
    if (timeout <= 0) {
        dev->tx_errors++;
        return -1;
    }
    
    // Copy data to buffer
    memcpy(dev->tx_buffers[dev->tx_current], data, len);
    
    // Set up descriptor
    dev->tx_descs[dev->tx_current].buffer_addr = (uint64_t)(uint32_t)dev->tx_buffers[dev->tx_current];
    dev->tx_descs[dev->tx_current].length = len;
    dev->tx_descs[dev->tx_current].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    dev->tx_descs[dev->tx_current].status = 0;
    
    // Advance tail
    uint32_t tdt = e1000_read_reg(dev, E1000_TDT);
    tdt = (tdt + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(dev, E1000_TDT, tdt);
    
    dev->tx_current = (dev->tx_current + 1) % E1000_NUM_TX_DESC;
    dev->tx_packets++;
    dev->tx_bytes += len;
    
    return 0;
}

// ============================================================================
// PACKET RECEPTION
// ============================================================================

void e1000_poll(e1000_dev_t* dev) {
    // Check link status
    uint32_t status = e1000_read_reg(dev, E1000_STATUS);
    dev->link_up = !!(status & E1000_STATUS_LU);
    
    if (!dev->link_up) return;
    
    // Process received packets
    while (dev->rx_descs[dev->rx_current].status & E1000_RXD_STAT_DD) {
        e1000_rx_desc_t* desc = &dev->rx_descs[dev->rx_current];
        
        if (desc->errors == 0) {
            uint8_t* packet = dev->rx_buffers[dev->rx_current];
            uint16_t len = desc->length;
            
            // Pass to network stack
            net_handle_packet(packet, len);
            
            dev->rx_packets++;
            dev->rx_bytes += len;
        } else {
            dev->rx_errors++;
        }
        
        // Mark descriptor as available
        desc->status = 0;
        
        // Update RDT
        uint32_t rdt = e1000_read_reg(dev, E1000_RDT);
        rdt = (rdt + 1) % E1000_NUM_RX_DESC;
        e1000_write_reg(dev, E1000_RDT, rdt);
        
        dev->rx_current = (dev->rx_current + 1) % E1000_NUM_RX_DESC;
    }
}

// ============================================================================
// PCI PROBE AND INITIALIZATION
// ============================================================================

int e1000_probe(uint16_t bus, uint16_t dev_num, uint16_t func) {
    uint32_t vendor_device = pci_read_config_dword(bus, dev_num, func, 0);
    uint16_t vendor = vendor_device & 0xFFFF;
    uint16_t device = (vendor_device >> 16) & 0xFFFF;
    
    // Check for Intel vendor
    if (vendor != 0x8086) return 0;
    
    // Check for e1000 device IDs
    // 8254x, 8257x, 8258x series
    if (device == 0x100E || device == 0x100F || device == 0x1010 ||
        device == 0x1011 || device == 0x1012 || device == 0x1013 ||
        device == 0x1014 || device == 0x1015 || device == 0x1016 ||
        device == 0x1017 || device == 0x1018 || device == 0x1019 ||
        device == 0x101A || device == 0x101D || device == 0x101E ||
        device == 0x1026 || device == 0x1027 || device == 0x1028 ||
        device == 0x1049 || device == 0x104A || device == 0x104B ||
        device == 0x104C || device == 0x104D || device == 0x105E ||
        device == 0x105F || device == 0x1060 || device == 0x1075 ||
        device == 0x1076 || device == 0x1077 || device == 0x1078 ||
        device == 0x1079 || device == 0x107A || device == 0x107B ||
        device == 0x107C || device == 0x107D || device == 0x107E ||
        device == 0x107F || device == 0x108A || device == 0x108B ||
        device == 0x108C || device == 0x1096 || device == 0x1097 ||
        device == 0x1098 || device == 0x1099 || device == 0x109A ||
        device == 0x10A4 || device == 0x10A5 || device == 0x10B5 ||
        device == 0x10B9 || device == 0x10BA || device == 0x10BB ||
        device == 0x10BC || device == 0x10BD || device == 0x10C4 ||
        device == 0x10C5 || device == 0x10C9 || device == 0x10D5 ||
        device == 0x10D6 || device == 0x10D9 || device == 0x10DA ||
        device == 0x10EA || device == 0x10EB || device == 0x10EF ||
        device == 0x10F0 || device == 0x10F5 || device == 0x10F6 ||
        device == 0x1501 || device == 0x1502 || device == 0x1503 ||
        device == 0x150A || device == 0x150C || device == 0x150D ||
        device == 0x150E || device == 0x150F || device == 0x1510 ||
        device == 0x1511 || device == 0x1516 || device == 0x1518 ||
        device == 0x151C || device == 0x1521 || device == 0x1522 ||
        device == 0x1523 || device == 0x1524 || device == 0x1525 ||
        device == 0x1526 || device == 0x1527 || device == 0x1528 ||
        device == 0x1529 || device == 0x152A || device == 0x152D ||
        device == 0x152E || device == 0x152F || device == 0x1530 ||
        device == 0x1531 || device == 0x1532 || device == 0x1533 ||
        device == 0x1534 || device == 0x1535 || device == 0x1536 ||
        device == 0x1537 || device == 0x1538 || device == 0x1539 ||
        device == 0x153A || device == 0x153B || device == 0x153C ||
        device == 0x153D || device == 0x153E || device == 0x153F ||
        device == 0x1559 || device == 0x155A || device == 0x155D ||
        device == 0x1560 || device == 0x1562 || device == 0x1563 ||
        device == 0x156F || device == 0x1570 || device == 0x157B ||
        device == 0x157C || device == 0x15A0 || device == 0x15A1 ||
        device == 0x15A2 || device == 0x15A3 || device == 0x15D7 ||
        device == 0x15D8 || device == 0x15E3 || device == 0x1A1C ||
        device == 0x1A1D || device == 0x1F41 || device == 0x1F40 ||
        device == 0x294C || device == 0x10EA || device == 0x10EB) {
        return 1;
    }
    
    return 0;
}

int e1000_init(uint16_t bus, uint16_t dev_num, uint16_t func) {
    if (e1000_device_count >= 4) return -1;
    
    e1000_dev_t* dev = &e1000_devices[e1000_device_count];
    memset(dev, 0, sizeof(e1000_dev_t));
    
    dev->pci_bus = bus;
    dev->pci_dev = dev_num;
    dev->pci_func = func;
    
    // Get device IDs
    uint32_t vendor_device = pci_read_config_dword(bus, dev_num, func, 0);
    dev->vendor_id = vendor_device & 0xFFFF;
    dev->device_id = (vendor_device >> 16) & 0xFFFF;
    
#if E1000_DEBUG_INIT
    serial_write_string("[E1000] Found device\n");
#endif
    
    // Get BARs
    uint32_t bar0 = pci_read_config_dword(bus, dev_num, func, 0x10);
    uint32_t bar1 = pci_read_config_dword(bus, dev_num, func, 0x14);
    uint32_t bar2 = pci_read_config_dword(bus, dev_num, func, 0x18);
    
    // Determine I/O method
    if (bar0 & 1) {
        // I/O port
        dev->use_mmio = 0;
        dev->io_base = bar0 & ~3;
    } else {
        // Memory-mapped I/O
        dev->use_mmio = 1;
        dev->mmio_base = bar0 & ~0xF;
        dev->mmio_size = 0x10000; // 64KB default
        
        // Map MMIO region
        dev->mmio = (uint8_t*)dev->mmio_base;
        paging_map_region(dev->mmio_base, dev->mmio_base, dev->mmio_size, 0x03);
    }
    
    // Enable bus mastering and memory/IO access
    uint32_t cmd = pci_read_config_dword(bus, dev_num, func, 0x04);
    cmd |= 0x07; // IOSE | MSE | BME
    pci_write_config_dword(bus, dev_num, func, 0x04, cmd);
    
    // Check for EEPROM
    uint32_t eecd = e1000_read_reg(dev, E1000_EECD);
    dev->has_eeprom = (eecd & 0x10) ? 1 : 0;
    
    // Read MAC address
    e1000_read_mac_addr(dev);
    
    // Reset device
    e1000_reset(dev);
    
    // Initialize hardware
    e1000_set_mac(dev);
    e1000_init_rx(dev);
    e1000_init_tx(dev);
    e1000_setup_interrupts(dev);
    
    // Update link status
    uint32_t status = e1000_read_reg(dev, E1000_STATUS);
    dev->link_up = !!(status & E1000_STATUS_LU);
    dev->duplex = !!(status & E1000_STATUS_FD);
    
    if ((status & E1000_STATUS_SPEED_MASK) == E1000_STATUS_SPEED_1000) {
        dev->speed = 1000;
    } else if ((status & E1000_STATUS_SPEED_MASK) == E1000_STATUS_SPEED_100) {
        dev->speed = 100;
    } else {
        dev->speed = 10;
    }
    
#if E1000_DEBUG_INIT
    serial_write_string("[E1000] Link initialized\n");
#endif
    
    // Register network interface
    memcpy(dev->netif.name, "eth", 4);
    dev->netif.driver_state = dev;
    memcpy(dev->netif.mac, dev->mac_addr, 6);
    dev->netif.send = e1000_send;
    net_register_interface(&dev->netif);
    
    e1000_device_count++;
    return 0;
}

void e1000_init_all(void) {
    // Scan PCI bus for e1000 devices
    for (int bus = 0; bus < 256; bus++) {
        for (int dev_num = 0; dev_num < 32; dev_num++) {
            for (int func = 0; func < 8; func++) {
                if (e1000_probe(bus, dev_num, func)) {
                    e1000_init(bus, dev_num, func);
                }
            }
        }
    }
}

void e1000_poll_all(void) {
    for (int i = 0; i < e1000_device_count; i++) {
        e1000_poll(&e1000_devices[i]);
    }
}
