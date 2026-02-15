// hal/drivers/ahci.c - Advanced Host Controller Interface (SATA) Driver Implementation
// Supports SATA drives via AHCI for real hardware compatibility

#include "ahci.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include "../cpu/timer.h"
#include "../cpu/paging.h"
#include "../../sys/io_ports.h"
#include "serial.h"
#include "pci.h"

// ============================================================================
// DEBUG CONFIGURATION
// ============================================================================
#define AHCI_DEBUG_ENABLED     1
#define AHCI_DEBUG_INIT        1
#define AHCI_DEBUG_RW          0

// ============================================================================
// GLOBAL STATE
// ============================================================================
static ahci_hba_t ahci_controllers[4];
static int ahci_controller_count = 0;

// ============================================================================
// MMIO ACCESS
// ============================================================================
static inline void ahci_write(uint8_t* mmio, uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(mmio + reg) = value;
}

static inline uint32_t ahci_read(uint8_t* mmio, uint32_t reg) {
    return *(volatile uint32_t*)(mmio + reg);
}

static inline void ahci_write_port(ahci_port_t* port, uint32_t reg, uint32_t value) {
    ahci_write((uint8_t*)port->base, reg, value);
}

static inline uint32_t ahci_read_port(ahci_port_t* port, uint32_t reg) {
    return ahci_read((uint8_t*)port->base, reg);
}

// ============================================================================
// PORT MANAGEMENT
// ============================================================================

static void ahci_stop_cmd(ahci_port_t* port) {
    ahci_write_port(port, AHCI_PORT_CMD, ahci_read_port(port, AHCI_PORT_CMD) & ~AHCI_PORT_CMD_ST);
    
    // Wait for CR to clear
    for (int i = 0; i < 500; i++) {
        if (!(ahci_read_port(port, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR)) break;
    }
    
    ahci_write_port(port, AHCI_PORT_CMD, ahci_read_port(port, AHCI_PORT_CMD) & ~AHCI_PORT_CMD_FRE);
    
    // Wait for FR to clear
    for (int i = 0; i < 500; i++) {
        if (!(ahci_read_port(port, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR)) break;
    }
}

static void ahci_start_cmd(ahci_port_t* port) {
    // Wait for CR to clear
    for (int i = 0; i < 500; i++) {
        if (!(ahci_read_port(port, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR)) break;
    }
    
    // Enable FIS receive
    ahci_write_port(port, AHCI_PORT_CMD, ahci_read_port(port, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
    
    // Wait for FR to set
    for (int i = 0; i < 500; i++) {
        if (ahci_read_port(port, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) break;
    }
    
    // Start command engine
    ahci_write_port(port, AHCI_PORT_CMD, ahci_read_port(port, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
    
    // Wait for CR to set
    for (int i = 0; i < 500; i++) {
        if (ahci_read_port(port, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) break;
    }
}

static int ahci_port_rebase(ahci_port_t* port, int num_cmd_slots) {
    // Stop command engine
    ahci_stop_cmd(port);
    
    // Allocate command list (1KB aligned)
    port->cmd_list = (ahci_cmd_header_t*)kmalloc(sizeof(ahci_cmd_header_t) * num_cmd_slots + 1024);
    if (!port->cmd_list) return -1;
    
    port->cmd_list_phys = (uint32_t)port->cmd_list;
    if (port->cmd_list_phys & 0x3FF) {
        port->cmd_list_phys = (port->cmd_list_phys + 1024) & ~0x3FF;
        port->cmd_list = (ahci_cmd_header_t*)port->cmd_list_phys;
    }
    memset(port->cmd_list, 0, sizeof(ahci_cmd_header_t) * num_cmd_slots);
    
    // Set command list base
    ahci_write_port(port, AHCI_PORT_CLB, port->cmd_list_phys);
    ahci_write_port(port, AHCI_PORT_CLBU, 0);
    
    // Allocate FIS (256B aligned)
    port->fis = (ahci_fis_t*)kmalloc(sizeof(ahci_fis_t) + 256);
    if (!port->fis) return -1;
    
    port->fis_phys = (uint32_t)port->fis;
    if (port->fis_phys & 0xFF) {
        port->fis_phys = (port->fis_phys + 256) & ~0xFF;
        port->fis = (ahci_fis_t*)port->fis_phys;
    }
    memset(port->fis, 0, sizeof(ahci_fis_t));
    
    // Set FIS base
    ahci_write_port(port, AHCI_PORT_FB, port->fis_phys);
    ahci_write_port(port, AHCI_PORT_FBU, 0);
    
    // Allocate command table (128B aligned)
    port->cmd_table = (ahci_cmd_table_t*)kmalloc(sizeof(ahci_cmd_table_t) + 128);
    if (!port->cmd_table) return -1;
    
    port->cmd_table_phys = (uint32_t)port->cmd_table;
    if (port->cmd_table_phys & 0x7F) {
        port->cmd_table_phys = (port->cmd_table_phys + 128) & ~0x7F;
        port->cmd_table = (ahci_cmd_table_t*)port->cmd_table_phys;
    }
    memset(port->cmd_table, 0, sizeof(ahci_cmd_table_t));
    
    // Set command table for slot 0
    port->cmd_list[0].cmd_table_base = port->cmd_table_phys;
    port->cmd_list[0].cmd_table_baseu = 0;
    
    // Clear interrupt status
    ahci_write_port(port, AHCI_PORT_IS, ahci_read_port(port, AHCI_PORT_IS));
    
    // Enable interrupts
    ahci_write_port(port, AHCI_PORT_IE, 0xFFFFFFFF);
    
    // Start command engine
    ahci_start_cmd(port);
    
    return 0;
}

static int ahci_find_cmd_slot(ahci_port_t* port) {
    // Check command issue register for free slot
    uint32_t ci = ahci_read_port(port, AHCI_PORT_CI);
    
    for (int i = 0; i < 32; i++) {
        if (!(ci & (1 << i))) {
            return i;
        }
    }
    
    return -1;
}

// ============================================================================
// COMMAND EXECUTION
// ============================================================================

int ahci_poll_completion(ahci_port_t* port, uint32_t slot, uint32_t timeout_ms) {
    uint32_t start = get_tick_count();
    
    while ((get_tick_count() - start) < timeout_ms) {
        // Check if command completed
        if (!(ahci_read_port(port, AHCI_PORT_CI) & (1 << slot))) {
            // Check for errors
            if (ahci_read_port(port, AHCI_PORT_IS) & AHCI_PORT_IS_TFES) {
                return -1; // Task file error
            }
            return 0; // Success
        }
    }
    
    return -2; // Timeout
}

int ahci_identify(ahci_port_t* port, void* buffer) {
    int slot = ahci_find_cmd_slot(port);
    if (slot < 0) return -1;
    
    // Setup command header
    ahci_cmd_header_t* cmd = &port->cmd_list[slot];
    cmd->dw0_flags = 5 << 0;    // Command FIS length (5 DWORDs)
    cmd->dw0_prdtl = 1;         // One PRD entry
    cmd->cmd_table_base = port->cmd_table_phys + slot * sizeof(ahci_cmd_table_t);
    
    // Setup command table
    ahci_cmd_table_t* table = (ahci_cmd_table_t*)((uint32_t)port->cmd_table + slot * sizeof(ahci_cmd_table_t));
    memset(table, 0, sizeof(ahci_cmd_table_t));
    
    // Setup FIS
    uint8_t* cfis = table->cfis;
    cfis[0] = AHCI_FIS_REG_H2D;
    cfis[1] = 0x80;             // Command bit
    cfis[2] = AHCI_CMD_IDENTIFY;
    
    // Setup PRD
    table->prdt[0].dba = (uint32_t)buffer;
    table->prdt[0].dbau = 0;
    table->prdt[0].dbc = 511;   // 512 bytes - 1
    table->prdt[0].reserved = 0;
    
    // Clear interrupt status
    ahci_write_port(port, AHCI_PORT_IS, ahci_read_port(port, AHCI_PORT_IS));
    
    // Issue command
    ahci_write_port(port, AHCI_PORT_CI, 1 << slot);
    
    // Wait for completion
    return ahci_poll_completion(port, slot, 5000);
}

int ahci_read_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, void* buffer) {
    if (count == 0 || count > 256) return -1;
    
    int slot = ahci_find_cmd_slot(port);
    if (slot < 0) return -1;
    
    // Calculate PRD count (each PRD can hold up to 4MB)
    uint32_t bytes = count * AHCI_SECTOR_SIZE;
    int prdt_count = (bytes + 0x3FFFFF) / 0x400000; // 4MB per PRD
    if (prdt_count > 8) prdt_count = 8;
    
    // Setup command header
    ahci_cmd_header_t* cmd = &port->cmd_list[slot];
    cmd->dw0_flags = 5 << 0;    // Command FIS length
    cmd->dw0_flags |= (1 << 6); // Read operation
    cmd->dw0_prdtl = prdt_count;
    cmd->cmd_table_base = port->cmd_table_phys + slot * sizeof(ahci_cmd_table_t);
    
    // Setup command table
    ahci_cmd_table_t* table = (ahci_cmd_table_t*)((uint32_t)port->cmd_table + slot * sizeof(ahci_cmd_table_t));
    memset(table, 0, sizeof(ahci_cmd_table_t));
    
    // Setup FIS
    uint8_t* cfis = table->cfis;
    cfis[0] = AHCI_FIS_REG_H2D;
    cfis[1] = 0x80;             // Command bit
    cfis[2] = AHCI_CMD_READ_DMA_EXT;
    cfis[3] = 0;                // Features
    cfis[4] = (lba >> 0) & 0xFF;
    cfis[5] = (lba >> 8) & 0xFF;
    cfis[6] = (lba >> 16) & 0xFF;
    cfis[7] = (lba >> 24) & 0xFF;
    cfis[8] = 0;                // Device (LBA mode)
    cfis[9] = (lba >> 32) & 0xFF;
    cfis[10] = (lba >> 40) & 0xFF;
    cfis[11] = (count >> 0) & 0xFF;
    cfis[12] = (count >> 8) & 0xFF;
    
    // Setup PRDs
    uint32_t remaining = bytes;
    uint8_t* buf = (uint8_t*)buffer;
    
    for (int i = 0; i < prdt_count && remaining > 0; i++) {
        uint32_t chunk = remaining > 0x400000 ? 0x400000 : remaining;
        table->prdt[i].dba = (uint32_t)buf;
        table->prdt[i].dbau = 0;
        table->prdt[i].dbc = chunk - 1;
        
        buf += chunk;
        remaining -= chunk;
    }
    
    // Clear interrupt status
    ahci_write_port(port, AHCI_PORT_IS, ahci_read_port(port, AHCI_PORT_IS));
    
    // Issue command
    ahci_write_port(port, AHCI_PORT_CI, 1 << slot);
    
    // Wait for completion
    int result = ahci_poll_completion(port, slot, 10000);
    
#if AHCI_DEBUG_RW
    if (result == 0) {
        s_printf("[AHCI] Read %d sectors at LBA %x success\n", count, (uint32_t)lba);
    } else {
        s_printf("[AHCI] Read failed: %d\n", result);
    }
#endif
    
    return result;
}

int ahci_write_sectors(ahci_port_t* port, uint64_t lba, uint32_t count, const void* buffer) {
    if (count == 0 || count > 256) return -1;
    
    int slot = ahci_find_cmd_slot(port);
    if (slot < 0) return -1;
    
    // Calculate PRD count
    uint32_t bytes = count * AHCI_SECTOR_SIZE;
    int prdt_count = (bytes + 0x3FFFFF) / 0x400000;
    if (prdt_count > 8) prdt_count = 8;
    
    // Setup command header
    ahci_cmd_header_t* cmd = &port->cmd_list[slot];
    cmd->dw0_flags = 5 << 0;    // Command FIS length
    cmd->dw0_flags |= (1 << 6); // Write operation
    cmd->dw0_prdtl = prdt_count;
    cmd->cmd_table_base = port->cmd_table_phys + slot * sizeof(ahci_cmd_table_t);
    
    // Setup command table
    ahci_cmd_table_t* table = (ahci_cmd_table_t*)((uint32_t)port->cmd_table + slot * sizeof(ahci_cmd_table_t));
    memset(table, 0, sizeof(ahci_cmd_table_t));
    
    // Setup FIS
    uint8_t* cfis = table->cfis;
    cfis[0] = AHCI_FIS_REG_H2D;
    cfis[1] = 0x80;             // Command bit
    cfis[2] = AHCI_CMD_WRITE_DMA_EXT;
    cfis[3] = 0;                // Features
    cfis[4] = (lba >> 0) & 0xFF;
    cfis[5] = (lba >> 8) & 0xFF;
    cfis[6] = (lba >> 16) & 0xFF;
    cfis[7] = (lba >> 24) & 0xFF;
    cfis[8] = 0;                // Device (LBA mode)
    cfis[9] = (lba >> 32) & 0xFF;
    cfis[10] = (lba >> 40) & 0xFF;
    cfis[11] = (count >> 0) & 0xFF;
    cfis[12] = (count >> 8) & 0xFF;
    
    // Setup PRDs
    uint32_t remaining = bytes;
    const uint8_t* buf = (const uint8_t*)buffer;
    
    for (int i = 0; i < prdt_count && remaining > 0; i++) {
        uint32_t chunk = remaining > 0x400000 ? 0x400000 : remaining;
        table->prdt[i].dba = (uint32_t)buf;
        table->prdt[i].dbau = 0;
        table->prdt[i].dbc = chunk - 1;
        
        buf += chunk;
        remaining -= chunk;
    }
    
    // Clear interrupt status
    ahci_write_port(port, AHCI_PORT_IS, ahci_read_port(port, AHCI_PORT_IS));
    
    // Issue command
    ahci_write_port(port, AHCI_PORT_CI, 1 << slot);
    
    // Wait for completion
    return ahci_poll_completion(port, slot, 10000);
}

// ============================================================================
// INITIALIZATION
// ============================================================================

int ahci_probe(uint16_t bus, uint16_t dev, uint16_t func) {
    // Check class code (mass storage) and subclass (SATA/AHCI)
    uint32_t class_code = pci_read_config_dword(bus, dev, func, 0x08);
    uint8_t base_class = (class_code >> 24) & 0xFF;
    uint8_t sub_class = (class_code >> 16) & 0xFF;
    uint8_t prog_if = (class_code >> 8) & 0xFF;
    
    // Mass storage (0x01), SATA (0x06), AHCI (0x01)
    if (base_class == 0x01 && sub_class == 0x06 && prog_if == 0x01) {
        return 1;
    }
    
    return 0;
}

int ahci_init(uint16_t bus, uint16_t dev, uint16_t func) {
    if (ahci_controller_count >= 4) return -1;
    
    ahci_hba_t* hba = &ahci_controllers[ahci_controller_count];
    memset(hba, 0, sizeof(ahci_hba_t));
    
    hba->pci_bus = bus;
    hba->pci_dev = dev;
    hba->pci_func = func;
    
#if AHCI_DEBUG_INIT
    serial_write_string("[AHCI] Found controller\n");
#endif
    
    // Get BAR5 (AHCI base)
    uint32_t bar5 = pci_read_config_dword(bus, dev, func, 0x24);
    hba->mmio_base = bar5 & ~0xF;
    hba->mmio_size = 0x1100; // Standard AHCI size
    
    // Enable bus mastering and memory access
    uint32_t cmd = pci_read_config_dword(bus, dev, func, 0x04);
    cmd |= 0x06; // BME | MSE
    pci_write_config_dword(bus, dev, func, 0x04, cmd);
    
    // Map MMIO
    hba->mmio = (uint8_t*)hba->mmio_base;
    paging_map_region(hba->mmio_base, hba->mmio_base, hba->mmio_size, 0x03);
    
    // Read capabilities
    hba->cap = ahci_read(hba->mmio, AHCI_GHC_CAP);
    hba->cap2 = ahci_read(hba->mmio, AHCI_GHC_CAP2);
    hba->version = ahci_read(hba->mmio, AHCI_GHC_VS);
    hba->pi = ahci_read(hba->mmio, AHCI_GHC_PI);
    
#if AHCI_DEBUG_INIT
    serial_write_string("[AHCI] Version initialized\n");
#endif
    
    // Enable AHCI
    ahci_write(hba->mmio, AHCI_GHC_GHC, AHCI_GHC_AE | AHCI_GHC_IE);
    
    // Number of command slots
    int num_cmd_slots = ((hba->cap >> 8) & 0x1F) + 1;
    
    // Initialize ports
    hba->port_count = 0;
    for (int i = 0; i < 32; i++) {
        if (hba->pi & (1 << i)) {
            ahci_port_t* port = &hba->ports[hba->port_count];
            port->base = (uint32_t)(hba->mmio + 0x100 + i * 0x80);
            port->number = i;
            
            // Check device status
            uint32_t ssts = ahci_read_port(port, AHCI_PORT_SSTS);
            uint32_t sig = ahci_read_port(port, AHCI_PORT_SIG);
            
            port->signature = sig;
            
            // Check if device is present
            if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PRESENT) {
                // Determine device type
                if (sig == AHCI_SIG_ATA) {
                    port->type = 1; // SATA
                } else if (sig == AHCI_SIG_ATAPI) {
                    port->type = 2; // SATAPI
                } else {
                    port->type = 0;
                }
                
                if (port->type > 0) {
                    // Initialize port
                    ahci_port_rebase(port, num_cmd_slots);
                    
#if AHCI_DEBUG_INIT
                    serial_write_string("[AHCI] Port initialized\n");
#endif
                    
                    hba->port_count++;
                }
            }
        }
    }
    
    ahci_controller_count++;
    return 0;
}

void ahci_init_all(void) {
    // Scan PCI bus for AHCI controllers
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                if (ahci_probe(bus, dev, func)) {
                    ahci_init(bus, dev, func);
                }
            }
        }
    }
}

int ahci_get_port_count(void) {
    int count = 0;
    for (int i = 0; i < ahci_controller_count; i++) {
        count += ahci_controllers[i].port_count;
    }
    return count;
}

ahci_port_t* ahci_get_port(int port_num) {
    int count = 0;
    for (int i = 0; i < ahci_controller_count; i++) {
        for (int j = 0; j < ahci_controllers[i].port_count; j++) {
            if (count == port_num) {
                return &ahci_controllers[i].ports[j];
            }
            count++;
        }
    }
    return 0;
}

int ahci_get_port_type(ahci_port_t* port) {
    return port->type;
}

int ahci_port_has_device(ahci_port_t* port) {
    return port->type > 0;
}

const char* ahci_port_type_str(int type) {
    switch (type) {
        case 0: return "None";
        case 1: return "SATA";
        case 2: return "SATAPI";
        default: return "Unknown";
    }
}

uint64_t ahci_get_capacity(ahci_port_t* port) {
    uint16_t identify[256];
    
    if (ahci_identify(port, identify) != 0) {
        return 0;
    }
    
    // Get LBA count from identify data (words 60-61)
    uint64_t lba_count = identify[60] | ((uint64_t)identify[61] << 16);
    
    // Check for LBA48 support (word 83)
    if (identify[83] & 0x400) {
        // Use words 100-103 for LBA48
        lba_count = (uint64_t)identify[100] |
                    ((uint64_t)identify[101] << 16) |
                    ((uint64_t)identify[102] << 32) |
                    ((uint64_t)identify[103] << 48);
    }
    
    return lba_count;
}
