// hal/drivers/ahci.h - Advanced Host Controller Interface (SATA) Driver
// Supports SATA drives via AHCI for real hardware compatibility

#ifndef AHCI_H
#define AHCI_H

#include <types.h>

// ============================================================================
// AHCI CONSTANTS
// ============================================================================

#define AHCI_MAX_PORTS          32
#define AHCI_MAX_CMD_SLOTS      32
#define AHCI_SECTOR_SIZE        512

// Generic Host Control (GHC) registers
#define AHCI_GHC_CAP            0x00    // HBA Capabilities
#define AHCI_GHC_GHC            0x04    // Global HBA Control
#define AHCI_GHC_IS             0x08    // Interrupt Status
#define AHCI_GHC_PI             0x0C    // Ports Implemented
#define AHCI_GHC_VS             0x10    // AHCI Version
#define AHCI_GHC_CCC_CTL        0x14    // Command Completion Coalescing Control
#define AHCI_GHC_CCC_PORTS      0x18    // CCC Ports
#define AHCI_GHC_EM_LOC         0x1C    // Enclosure Management Location
#define AHCI_GHC_EM_CTL         0x20    // EM Control
#define AHCI_GHC_CAP2           0x24    // HBA Capabilities Extended
#define AHCI_GHC_BOHC           0x28    // BIOS/OS Handoff Control

// GHC bits
#define AHCI_GHC_AE             0x80000000  // AHCI Enable
#define AHCI_GHC_IE             0x00000002  // Interrupt Enable
#define AHCI_GHC_HR             0x00000001  // HBA Reset

// Port registers (offset from port base)
#define AHCI_PORT_CLB           0x00    // Command List Base
#define AHCI_PORT_CLBU          0x04    // Command List Base Upper
#define AHCI_PORT_FB            0x08    // FIS Base
#define AHCI_PORT_FBU           0x0C    // FIS Base Upper
#define AHCI_PORT_IS            0x10    // Interrupt Status
#define AHCI_PORT_IE            0x14    // Interrupt Enable
#define AHCI_PORT_CMD           0x18    // Command and Status
#define AHCI_PORT_RES1          0x1C    // Reserved
#define AHCI_PORT_TFD           0x20    // Task File Data
#define AHCI_PORT_SIG           0x24    // Signature
#define AHCI_PORT_SSTS          0x28    // SATA Status
#define AHCI_PORT_SCTL          0x2C    // SATA Control
#define AHCI_PORT_SERR          0x30    // SATA Error
#define AHCI_PORT_SACT          0x34    // SATA Active
#define AHCI_PORT_CI            0x38    // Command Issue
#define AHCI_PORT_SNTF          0x3C    // SATA Notification
#define AHCI_PORT_FBS           0x40    // FIS-based Switching Control
#define AHCI_PORT_DEVSLP        0x44    // Device Sleep
#define AHCI_PORT_RES2          0x48    // Reserved
#define AHCI_PORT_VS            0x70    // Vendor Specific

// Port CMD bits
#define AHCI_PORT_CMD_ST        0x0001  // Start
#define AHCI_PORT_CMD_SUD       0x0002  // Spin-Up Device
#define AHCI_PORT_CMD_POD       0x0004  // Power On Device
#define AHCI_PORT_CMD_CLO       0x0008  // Command List Override
#define AHCI_PORT_CMD_FRE       0x0010  // FIS Receive Enable
#define AHCI_PORT_CMD_CCS       0x01E0  // Current Command Slot
#define AHCI_PORT_CMD_ISS       0x0200  // Interface Select
#define AHCI_PORT_CMD_FR        0x4000  // FIS Receive Running
#define AHCI_PORT_CMD_CR        0x8000  // Command List Running

// Port IS bits
#define AHCI_PORT_IS_DHRS       0x00000001  // Device to Host Register FIS
#define AHCI_PORT_IS_PSS        0x00000002  // PIO Setup FIS
#define AHCI_PORT_IS_DSS        0x00000004  // DMA Setup FIS
#define AHCI_PORT_IS_SDBS       0x00000008  // Set Device Bits FIS
#define AHCI_PORT_IS_UFS        0x00000010  // Unknown FIS
#define AHCI_PORT_IS_DPS        0x00000020  // Descriptor Processed
#define AHCI_PORT_IS_PCS        0x00000040  // Port Connect Change
#define AHCI_PORT_IS_DMPS       0x00000080  // Device Mechanical Presence
#define AHCI_PORT_IS_PRCS       0x00400000  // PhyRdy Change Status
#define AHCI_PORT_IS_IPMS       0x00800000  // Interface Power Management
#define AHCI_PORT_IS_OFS        0x01000000  // Overflow Status
#define AHCI_PORT_IS_INFS       0x04000000  // Interface Non-fatal Error
#define AHCI_PORT_IS_IFS        0x08000000  // Interface Fatal Error
#define AHCI_PORT_IS_HBDS       0x10000000  // Host Bus Data Error
#define AHCI_PORT_IS_HBFS       0x20000000  // Host Bus Fatal Error
#define AHCI_PORT_IS_TFES       0x40000000  // Task File Error
#define AHCI_PORT_IS_CPDS       0x80000000  // Cold Port Detect Status

// SATA Status bits
#define AHCI_SSTS_DET_MASK      0x0F
#define AHCI_SSTS_DET_NONE      0x00    // No device
#define AHCI_SSTS_DET_NOPHY     0x01    // Device present, no phy
#define AHCI_SSTS_DET_PRESENT   0x03    // Device present, phy established
#define AHCI_SSTS_DET_OFFLINE   0x04    // Device offline

#define AHCI_SSTS_SPD_MASK      0xF0
#define AHCI_SSTS_SPD_GEN1      0x10    // 1.5 Gbps
#define AHCI_SSTS_SPD_GEN2      0x20    // 3.0 Gbps
#define AHCI_SSTS_SPD_GEN3      0x30    // 6.0 Gbps

#define AHCI_SSTS_IPM_MASK      0xF00
#define AHCI_SSTS_IPM_ACTIVE    0x100
#define AHCI_SSTS_IPM_PARTIAL   0x200
#define AHCI_SSTS_IPM_SLUMBER   0x600

// Command FIS types
#define AHCI_FIS_REG_H2D        0x27    // Register - Host to Device
#define AHCI_FIS_REG_D2H        0x34    // Register - Device to Host
#define AHCI_FIS_DMA_ACT        0x39    // DMA Activate
#define AHIS_FIS_DMA_SETUP      0x41    // DMA Setup
#define AHCI_FIS_DATA           0x46    // Data
#define AHCI_FIS_BIST           0x58    // BIST
#define AHCI_FIS_PIO_SETUP      0x5F    // PIO Setup
#define AHCI_FIS_DEV_BITS       0xA1    // Set Device Bits

// Device signatures
#define AHCI_SIG_ATA            0x00000101  // SATA disk
#define AHCI_SIG_ATAPI          0xEB140101  // SATAPI optical
#define AHCI_SIG_PM             0x96690101  // Port multiplier

// Command types
#define AHCI_CMD_READ_DMA_EXT   0x25
#define AHCI_CMD_WRITE_DMA_EXT  0x35
#define AHCI_CMD_IDENTIFY       0xEC
#define AHCI_CMD_IDENTIFY_PACKET 0xA1
#define AHCI_CMD_READ_SECTORS   0x20
#define AHCI_CMD_WRITE_SECTORS  0x30

// ============================================================================
// STRUCTURES
// ============================================================================

// Command list entry
typedef struct {
    uint16_t dw0_flags;      // Flags
    uint16_t dw0_prdtl;      // Physical Region Descriptor Table Length
    uint32_t dw1;            // Reserved / PRD byte count
    uint32_t cmd_table_base; // Command Table Base
    uint32_t cmd_table_baseu;// Command Table Base Upper
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

// Physical Region Descriptor
typedef struct {
    uint32_t dba;            // Data Base Address
    uint32_t dbau;           // Data Base Address Upper
    uint32_t reserved;
    uint32_t dbc;            // Data Byte Count (bit 0 = interrupt on completion)
} __attribute__((packed)) ahci_prdt_t;

// Command Table
typedef struct {
    uint8_t cfis[64];        // Command FIS
    uint8_t atapi[16];       // ATAPI Command
    uint8_t reserved[48];
    ahci_prdt_t prdt[8];     // PRDT entries
} __attribute__((packed)) ahci_cmd_table_t;

// Received FIS structure
typedef struct {
    uint8_t dsfis[28];       // DMA Setup FIS
    uint8_t reserved1[4];
    uint8_t psfis[20];       // PIO Setup FIS
    uint8_t reserved2[12];
    uint8_t rfis[20];        // D2H Register FIS
    uint8_t reserved3[4];
    uint8_t sdbfis[8];       // Set Device Bits FIS
    uint8_t ufis[64];        // Unknown FIS
    uint8_t reserved4[96];
} __attribute__((packed)) ahci_fis_t;

// Port structure
typedef struct {
    uint32_t base;           // Port register base
    int number;              // Port number
    int type;                // Device type (0=none, 1=ATA, 2=ATAPI)
    uint32_t signature;      // Device signature
    
    // DMA structures
    ahci_cmd_header_t* cmd_list;
    ahci_cmd_table_t* cmd_table;
    ahci_fis_t* fis;
    
    // DMA physical addresses
    uint32_t cmd_list_phys;
    uint32_t cmd_table_phys;
    uint32_t fis_phys;
    
    // Command slot tracking
    uint32_t cmd_slot;
    
    // Lock
    volatile int lock;
    
} ahci_port_t;

// HBA structure
typedef struct {
    uint32_t mmio_base;      // MMIO base address
    uint32_t mmio_size;      // MMIO size
    uint8_t* mmio;           // MMIO pointer
    
    uint32_t cap;            // Capabilities
    uint32_t cap2;           // Extended capabilities
    uint32_t version;        // AHCI version
    uint32_t pi;             // Ports implemented
    
    ahci_port_t ports[AHCI_MAX_PORTS];
    int port_count;
    
    // PCI info
    uint16_t pci_bus;
    uint16_t pci_dev;
    uint16_t pci_func;
    
} ahci_hba_t;

// ============================================================================
// API
// ============================================================================

// Initialize AHCI controller
int ahci_init(uint16_t bus, uint16_t dev, uint16_t func);

// Initialize all AHCI controllers
void ahci_init_all(void);

// Read sectors from a port
int ahci_read_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer);

// Write sectors to a port
int ahci_write_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer);

// Get port count
int ahci_get_port_count(void);

// Get port by number
ahci_port_t* ahci_get_port(int port_num);

// Get port type
int ahci_get_port_type(ahci_port_t* port);

// Check if port has device
int ahci_port_has_device(ahci_port_t* port);

// Get port capacity (in sectors)
uint64_t ahci_get_capacity(ahci_port_t* port);

// Get identify data
int ahci_identify(ahci_port_t* port, void* buffer);

// Poll for completion
int ahci_poll_completion(ahci_port_t* port, uint32_t slot, uint32_t timeout_ms);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Convert port type to string
const char* ahci_port_type_str(int type);

// Print port info
void ahci_print_port_info(ahci_port_t* port);

#endif // AHCI_H
