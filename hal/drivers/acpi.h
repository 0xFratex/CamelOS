// hal/drivers/acpi.h - ACPI Table Parsing and Power Management for CamelOS
// Provides RSDP/RSDT/FADT/MADT/HPET parsing and power-off/reboot support
#ifndef ACPI_H
#define ACPI_H

#include "../../include/types.h"

// ========================================================================
// ACPI Table Header (common to all ACPI tables)
// ========================================================================
typedef struct {
    char        signature[4];    // Table signature (e.g. "RSDT", "FACP")
    uint32_t    length;          // Total table length in bytes
    uint8_t     revision;        // ACPI revision
    uint8_t     checksum;        // Entire table must sum to zero
    char        oem_id[6];       // OEM identification
    char        oem_table_id[8]; // OEM table identification
    uint32_t    oem_revision;    // OEM revision
    uint32_t    creator_id;      // ASL compiler vendor ID
    uint32_t    creator_revision;// ASL compiler revision
} __attribute__((packed)) acpi_header_t;

// ========================================================================
// RSDP - Root System Description Pointer
// ========================================================================
typedef struct {
    char        signature[8];    // "RSD PTR "
    uint8_t     checksum;        // Checksum for first 20 bytes
    char        oem_id[6];       // OEM identification
    uint8_t     revision;        // 0 for ACPI 1.0, 2 for ACPI 2.0+
    uint32_t    rsdt_address;    // Physical address of RSDT (32-bit)
    // ACPI 2.0+ fields below
    uint32_t    length;          // Total RSDP length
    uint64_t    xsdt_address;    // Physical address of XSDT (64-bit)
    uint8_t     extended_checksum;// Checksum for entire table
    uint8_t     reserved[3];
} __attribute__((packed)) rsdp_t;

// ========================================================================
// RSDT - Root System Description Table
// ========================================================================
typedef struct {
    acpi_header_t header;
    // Followed by uint32_t entry[] — physical addresses of other tables
} __attribute__((packed)) rsdt_t;

// ========================================================================
// FADT - Fixed ACPI Description Table (signature "FACP")
// ========================================================================
typedef struct {
    acpi_header_t header;
    uint32_t    firmware_ctrl;   // FACS physical address
    uint32_t    dsdt;            // DSDT physical address
    uint8_t     reserved1;       // INT_MODEL (ignored in ACPI 2.0+)
    uint8_t     preferred_pm_profile;
    uint16_t    sci_int;         // SCI interrupt vector
    uint32_t    smi_cmd;         // SMI command port
    uint8_t     acpi_enable;     // Value to write to smi_cmd to enable ACPI
    uint8_t     acpi_disable;    // Value to write to smi_cmd to disable ACPI
    uint8_t     s4bios_req;      // S4 BIOS request
    uint8_t     pstate_cnt;      // Processor state control
    uint32_t    pm1a_evt_blk;    // PM1a Event Register Block I/O port
    uint32_t    pm1b_evt_blk;    // PM1b Event Register Block I/O port
    uint32_t    pm1a_cnt_blk;    // PM1a Control Register Block I/O port
    uint32_t    pm1b_cnt_blk;    // PM1b Control Register Block I/O port
    uint32_t    pm2_cnt_blk;     // PM2 Control Register Block I/O port
    uint32_t    pm_tmr_blk;      // PM Timer Register Block I/O port
    uint32_t    gpe0_blk;        // GPE0 Register Block I/O port
    uint32_t    gpe1_blk;        // GPE1 Register Block I/O port
    uint8_t     pm1_evt_len;     // PM1 Event Register Block length
    uint8_t     pm1_cnt_len;     // PM1 Control Register Block length
    uint8_t     pm2_cnt_len;     // PM2 Control Register Block length
    uint8_t     pm_tmr_len;      // PM Timer Register Block length
    uint8_t     gpe0_blk_len;    // GPE0 Register Block length
    uint8_t     gpe1_blk_len;    // GPE1 Register Block length
    uint8_t     gpe1_base;       // GPE1 base offset
    uint8_t     cst_cnt;         // _CST support
    uint16_t    p_lvl2_lat;      // C2 latency
    uint16_t    p_lvl3_lat;      // C3 latency
    uint16_t    flush_size;      // Cache flush size
    uint16_t    flush_stride;    // Cache flush stride
    uint8_t     duty_offset;     // Duty cycle offset
    uint8_t     duty_width;      // Duty cycle width
    uint8_t     day_alrm;        // Day alarm index
    uint8_t     mon_alrm;        // Month alarm index
    uint8_t     century;         // Century RTC register index
    uint16_t    iapc_boot_arch;  // IA-PC boot architecture flags
    uint8_t     reserved2;       // Reserved
    uint32_t    flags;           // Fixed feature flags
    // ACPI 2.0+ reset register and other extended fields follow
    uint8_t     reset_reg[12];   // Reset register (Generic Address Structure)
    uint8_t     reset_value;     // Reset value to write to reset_reg
} __attribute__((packed)) fadt_t;

// ========================================================================
// MADT - Multiple APIC Description Table (signature "APIC")
// ========================================================================
typedef struct {
    acpi_header_t header;
    uint32_t    local_apic_addr; // Physical address of local APIC
    uint32_t    flags;           // MADT flags (bit 0: PCAT_COMPAT)
    // Followed by variable-length interrupt controller structures
} __attribute__((packed)) madt_t;

// MADT entry types
#define MADT_TYPE_LOCAL_APIC     0
#define MADT_TYPE_IO_APIC        1
#define MADT_TYPE_INT_SRC_OVERRIDE 2
#define MADT_TYPE_NMI            3

// MADT Local APIC entry
typedef struct {
    uint8_t     type;            // 0
    uint8_t     length;          // 8
    uint8_t     processor_id;    // ACPI processor ID
    uint8_t     apic_id;         // Local APIC ID
    uint32_t    flags;           // bit 0: enabled
} __attribute__((packed)) madt_local_apic_t;

// MADT I/O APIC entry
typedef struct {
    uint8_t     type;            // 1
    uint8_t     length;          // 12
    uint8_t     ioapic_id;       // I/O APIC ID
    uint8_t     reserved;
    uint32_t    ioapic_addr;     // Physical address
    uint32_t    gsi_base;        // Global System Interrupt base
} __attribute__((packed)) madt_ioapic_t;

// ========================================================================
// HPET - High Precision Event Timer (signature "HPET")
// ========================================================================
typedef struct {
    acpi_header_t header;
    uint8_t     hardware_rev_id;
    uint8_t     comparator_count:5;
    uint8_t     counter_size:1;
    uint8_t     reserved1:1;
    uint8_t     legacy_replacement:1;
    uint16_t    pci_vendor_id;
    uint8_t     address_space_id;
    uint8_t     register_bit_width;
    uint8_t     register_bit_offset;
    uint8_t     reserved2;
    uint64_t    address;         // HPET base address
    uint8_t     hpet_number;
    uint16_t    minimum_tick;
    uint8_t     page_protection;
} __attribute__((packed)) hpet_t;

// ========================================================================
// ACPI Sleep Type values for PM1a_CNT
// ========================================================================
#define ACPI_SLP_TYP_S0   0x0000   // Working state
#define ACPI_SLP_TYP_S1   0x0200   // Power-on suspend
#define ACPI_SLP_TYP_S3   0x0500   // Suspend-to-RAM
#define ACPI_SLP_TYP_S4   0x0600   // Suspend-to-disk
#define ACPI_SLP_TYP_S5   0x0700   // Soft-off (shutdown)
#define ACPI_SLP_EN       0x2000   // Sleep enable bit

// ========================================================================
// FADT Flags bits
// ========================================================================
#define FADT_FLAG_TMR_VAL_EXT  (1 << 8)  // PM timer is 32-bit (vs 24-bit)

// ========================================================================
// Public API
// ========================================================================

/**
 * acpi_init - Initialize the ACPI subsystem.
 * Searches for the RSDP, parses RSDT, and locates key tables (FADT, MADT, HPET).
 * Must be called after paging/mapping is set up.
 */
void acpi_init(void);

/**
 * acpi_enable - Transition from ACPI-disabled (legacy) to ACPI mode.
 * Writes the ACPI_ENABLE value to the SMI_CMD port as specified by FADT.
 * Returns 0 on success, -1 if FADT not found or ACPI already enabled.
 */
int acpi_enable(void);

/**
 * acpi_shutdown - Power off the system via ACPI S5 sleep state.
 * Writes (SLP_TYP_S5 | SLP_EN) to PM1a_CNT register.
 * Does not return on success.
 * Returns -1 if FADT/PM registers not available.
 */
int acpi_shutdown(void);

/**
 * acpi_reboot - Reset the system.
 * First tries the ACPI reset register (if available in FADT 2.0+),
 * then falls back to keyboard controller reset (port 0x64, data 0xFE).
 * Does not return on success.
 * Returns -1 if no reset mechanism available.
 */
int acpi_reboot(void);

/**
 * acpi_get_table_signature - Find an ACPI table by its 4-char signature.
 * @signature: 4-character table signature (e.g. "FACP", "APIC", "HPET")
 * Returns: pointer to the table's header if found, NULL otherwise.
 * The returned pointer is a direct mapped kernel address.
 */
const char* acpi_get_table_signature(const char* signature);

/**
 * acpi_get_rsdp - Get a pointer to the found RSDP structure.
 * Returns NULL if ACPI not initialized or RSDP not found.
 */
const rsdp_t* acpi_get_rsdp(void);

/**
 * acpi_get_fadt - Get a pointer to the FADT structure.
 * Returns NULL if not found.
 */
const fadt_t* acpi_get_fadt(void);

/**
 * acpi_get_madt - Get a pointer to the MADT structure.
 * Returns NULL if not found.
 */
const madt_t* acpi_get_madt(void);

/**
 * acpi_get_hpet - Get a pointer to the HPET structure.
 * Returns NULL if not found.
 */
const hpet_t* acpi_get_hpet(void);

/**
 * acpi_is_available - Check if ACPI was successfully initialized.
 * Returns 1 if ACPI tables were found and parsed, 0 otherwise.
 */
int acpi_is_available(void);

#endif /* ACPI_H */
