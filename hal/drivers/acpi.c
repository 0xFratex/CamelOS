// hal/drivers/acpi.c - ACPI Table Parsing and Power Management for CamelOS
// Implements RSDP/RSDT discovery, FADT/MADT/HPET table parsing,
// and power management (shutdown/reboot) using ACPI registers.
//
// Physical memory access: CamelOS identity-maps the first 128MB of physical
// memory, so physical addresses < 0x08000000 can be directly dereferenced.

#include "acpi.h"
#include "../../common/ports.h"
#include "serial.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include <stdint.h>

// ========================================================================
// Internal state
// ========================================================================
static rsdp_t*  found_rsdp = (rsdp_t*)0;
static rsdt_t*  found_rsdt = (rsdt_t*)0;
static fadt_t*  found_fadt = (fadt_t*)0;
static madt_t*  found_madt = (madt_t*)0;
static hpet_t*  found_hpet = (hpet_t*)0;
static int      acpi_available = 0;

// ========================================================================
// Helpers
// ========================================================================

/** Compute ACPI checksum: sum of all bytes must be zero. */
static int acpi_checksum(const void* table, uint32_t length) {
    const uint8_t* p = (const uint8_t*)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += p[i];
    }
    return (sum == 0) ? 0 : -1;
}

/** Compare 4-char signature. */
static int sig_match(const char* a, const char* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/** Read a uint16 from an I/O port. */
static inline uint16_t inw_safe(uint16_t port) {
    return inw(port);
}

/** Write a uint16 to an I/O port. */
static inline void outw_safe(uint16_t port, uint16_t data) {
    outw(port, data);
}

// ========================================================================
// RSDP Search
// ========================================================================

/**
 * Search for the RSDP in a memory region.
 * The RSDP is 16-byte aligned and its signature is "RSD PTR ".
 */
static rsdp_t* scan_for_rsdp(uint32_t start, uint32_t end) {
    // Align start to 16 bytes
    start = (start + 0xF) & ~0xF;

    for (uint32_t addr = start; addr < end; addr += 16) {
        const char* ptr = (const char*)(uintptr_t)addr;

        // Check signature "RSD PTR "
        if (ptr[0] != 'R' || ptr[1] != 'S' || ptr[2] != 'D' ||
            ptr[3] != ' ' || ptr[4] != 'P' || ptr[5] != 'T' ||
            ptr[6] != 'R' || ptr[7] != ' ') {
            continue;
        }

        rsdp_t* rsdp = (rsdp_t*)(uintptr_t)addr;

        // Verify checksum of first 20 bytes (ACPI 1.0)
        if (acpi_checksum(rsdp, 20) != 0) {
            s_printf("[ACPI] RSDP at 0x%x has bad checksum\n", addr);
            continue;
        }

        // For ACPI 2.0+, verify extended checksum if revision >= 2
        if (rsdp->revision >= 2 && rsdp->length > 20) {
            if (acpi_checksum(rsdp, rsdp->length) != 0) {
                s_printf("[ACPI] RSDP at 0x%x has bad extended checksum\n", addr);
                continue;
            }
        }

        s_printf("[ACPI] RSDP found at 0x%x (rev %d)\n", addr, rsdp->revision);
        return rsdp;
    }

    return (rsdp_t*)0;
}

/**
 * Find the RSDP by searching the standard locations:
 *   1. EBDA (Extended BIOS Data Area) — first 1KB of the segment at 0x40E
 *   2. BIOS ROM area 0xE0000 - 0xFFFFF
 */
static rsdp_t* find_rsdp(void) {
    rsdp_t* rsdp;

    // 1. Search EBDA
    // The EBDA segment address is stored at physical 0x40E (16-bit real-mode segment)
    uint16_t ebda_segment = *(uint16_t*)(uintptr_t)0x40E;
    if (ebda_segment != 0) {
        uint32_t ebda_physical = (uint32_t)ebda_segment << 4;
        // Search first 1KB of EBDA
        uint32_t ebda_end = ebda_physical + 1024;
        if (ebda_end > 0xA0000) ebda_end = 0xA0000;  // Don't go past conventional memory

        rsdp = scan_for_rsdp(ebda_physical, ebda_end);
        if (rsdp) return rsdp;
    }

    // 2. Search BIOS ROM area: 0xE0000 - 0xFFFFF
    rsdp = scan_for_rsdp(0xE0000, 0x100000);
    return rsdp;
}

// ========================================================================
// Table Parsing
// ========================================================================

/**
 * Walk the RSDT entries and find tables by signature.
 * The RSDT contains an array of 32-bit physical addresses to other tables.
 */
static void parse_rsdt(rsdt_t* rsdt) {
    if (!rsdt) return;

    // Calculate number of entries in the RSDT
    uint32_t entry_count = (rsdt->header.length - sizeof(acpi_header_t)) / sizeof(uint32_t);

    s_printf("[ACPI] RSDT at 0x%x has %d entries\n", (uint32_t)(uintptr_t)rsdt, entry_count);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t table_addr = ((uint32_t*)((uintptr_t)rsdt + sizeof(acpi_header_t)))[i];
        if (table_addr == 0) continue;

        acpi_header_t* header = (acpi_header_t*)(uintptr_t)table_addr;

        // Validate the table with checksum
        if (acpi_checksum(header, header->length) != 0) {
            s_printf("[ACPI] Table at 0x%x (%.4s) has bad checksum\n",
                     table_addr, header->signature);
            continue;
        }

        s_printf("[ACPI] Found table: %.4s at 0x%x (len=%d)\n",
                 header->signature, table_addr, header->length);

        // Check for known tables
        if (sig_match(header->signature, "FACP")) {
            found_fadt = (fadt_t*)(uintptr_t)table_addr;
            s_printf("[ACPI]   -> FADT: PM1a_EVT=0x%x PM1a_CNT=0x%x SMI_CMD=0x%x\n",
                     found_fadt->pm1a_evt_blk, found_fadt->pm1a_cnt_blk,
                     found_fadt->smi_cmd);
        }
        else if (sig_match(header->signature, "APIC")) {
            found_madt = (madt_t*)(uintptr_t)table_addr;
            s_printf("[ACPI]   -> MADT: Local APIC addr=0x%x flags=0x%x\n",
                     found_madt->local_apic_addr, found_madt->flags);
        }
        else if (sig_match(header->signature, "HPET")) {
            found_hpet = (hpet_t*)(uintptr_t)table_addr;
            s_printf("[ACPI]   -> HPET: base=0x%x\n",
                     (uint32_t)found_hpet->address);
        }
    }
}

// ========================================================================
// Public API Implementation
// ========================================================================

void acpi_init(void) {
    s_printf("[ACPI] Initializing ACPI subsystem...\n");

    // Step 1: Find the RSDP
    found_rsdp = find_rsdp();
    if (!found_rsdp) {
        s_printf("[ACPI] RSDP not found — ACPI not available\n");
        acpi_available = 0;
        return;
    }

    s_printf("[ACPI] RSDP revision %d, OEM='%.6s'\n",
             found_rsdp->revision, found_rsdp->oem_id);

    // Step 2: Map the RSDT
    // For ACPI 1.0, use rsdt_address (32-bit).
    // For ACPI 2.0+, we could use xsdt_address, but for simplicity
    // and 32-bit OS compatibility, we use RSDT.
    uint32_t rsdt_phys = found_rsdp->rsdt_address;
    if (rsdt_phys == 0) {
        s_printf("[ACPI] RSDT address is zero\n");
        acpi_available = 0;
        return;
    }

    found_rsdt = (rsdt_t*)(uintptr_t)rsdt_phys;

    // Validate RSDT signature
    if (!sig_match(found_rsdt->header.signature, "RSDT")) {
        s_printf("[ACPI] RSDT signature mismatch: %.4s\n",
                 found_rsdt->header.signature);
        acpi_available = 0;
        return;
    }

    // Validate RSDT checksum
    if (acpi_checksum(found_rsdt, found_rsdt->header.length) != 0) {
        s_printf("[ACPI] RSDT checksum failed\n");
        acpi_available = 0;
        return;
    }

    s_printf("[ACPI] RSDT at 0x%x (len=%d, rev=%d, OEM='%.6s')\n",
             rsdt_phys, found_rsdt->header.length,
             found_rsdt->header.revision, found_rsdt->header.oem_id);

    // Step 3: Walk RSDT to find FADT, MADT, HPET
    parse_rsdt(found_rsdt);

    acpi_available = 1;
    s_printf("[ACPI] Initialization complete: FADT=%s MADT=%s HPET=%s\n",
             found_fadt ? "yes" : "no",
             found_madt ? "yes" : "no",
             found_hpet ? "yes" : "no");
}

int acpi_enable(void) {
    if (!found_fadt) return -1;

    // If SMI_CMD is 0, ACPI is likely already enabled (hardware reduced)
    if (found_fadt->smi_cmd == 0) {
        s_printf("[ACPI] SMI_CMD is 0 — ACPI likely already enabled\n");
        return 0;
    }

    // Check if ACPI is already enabled by reading PM1a_CNT
    // If SCI_EN (bit 0) is set, ACPI is already active
    if (found_fadt->pm1a_cnt_blk != 0) {
        uint16_t pm1a = inw_safe(found_fadt->pm1a_cnt_blk);
        if (pm1a & 0x0001) {
            s_printf("[ACPI] Already enabled (SCI_EN=1)\n");
            return 0;
        }
    }

    // Write ACPI_ENABLE value to SMI_CMD port
    s_printf("[ACPI] Enabling ACPI: writing 0x%x to SMI_CMD 0x%x\n",
             found_fadt->acpi_enable, found_fadt->smi_cmd);
    outb(found_fadt->smi_cmd, found_fadt->acpi_enable);

    // Wait for SCI_EN to be set (with timeout)
    if (found_fadt->pm1a_cnt_blk != 0) {
        for (int i = 0; i < 1000000; i++) {
            uint16_t pm1a = inw_safe(found_fadt->pm1a_cnt_blk);
            if (pm1a & 0x0001) {
                s_printf("[ACPI] Enabled successfully\n");
                return 0;
            }
        }
    }

    s_printf("[ACPI] Enable timeout — SCI_EN not set\n");
    return -1;
}

int acpi_shutdown(void) {
    if (!found_fadt) {
        s_printf("[ACPI] Cannot shutdown: FADT not found\n");
        return -1;
    }

    if (found_fadt->pm1a_cnt_blk == 0) {
        s_printf("[ACPI] Cannot shutdown: PM1a_CNT not available\n");
        return -1;
    }

    s_printf("[ACPI] Initiating S5 shutdown...\n");

    // Ensure ACPI is enabled first
    acpi_enable();

    // Disable all GPEs
    if (found_fadt->pm1a_evt_blk != 0) {
        // Clear all PM1 status bits by writing 1s (write-1-to-clear)
        outw_safe(found_fadt->pm1a_evt_blk, 0xFFFF);
        // Disable all PM1 events
        outw_safe(found_fadt->pm1a_evt_blk + 2, 0x0000);
    }

    if (found_fadt->gpe0_blk != 0 && found_fadt->gpe0_blk_len > 0) {
        // Clear GPE0 status
        for (int i = 0; i < found_fadt->gpe0_blk_len; i += 2) {
            outw_safe(found_fadt->gpe0_blk + i, 0xFFFF);
        }
        // Disable GPE0 enables
        int gpe0_en_offset = (found_fadt->gpe0_blk_len / 2);
        for (int i = 0; i < found_fadt->gpe0_blk_len / 2; i += 2) {
            outw_safe(found_fadt->gpe0_blk + gpe0_en_offset + i, 0x0000);
        }
    }

    // Write SLP_TYP (S5) | SLP_EN to PM1a_CNT
    // S5 sleep type is typically 0x7 << 10 = 0x1C00, but we use the
    // defined constant that maps to the standard value.
    // Some systems need the SLP_TYP value from the DSDT; we use the
    // standard S5 value (0) shifted into position with SLP_EN.
    //
    // The SLP_TYP bits occupy bits 10-12 in PM1a_CNT.
    // For S5, the typical value is 0 (derived from \_S5 package),
    // but we use 0x1C00 (SLP_TYP=7) as a common fallback.
    // Real implementations should parse \_S5 from DSDT.
    uint16_t slp_typ_s5 = (ACPI_SLP_TYP_S5 >> 10) & 0x7;  // Extract SLP_TYP value
    uint16_t pm1a_value = (slp_typ_s5 << 10) | ACPI_SLP_EN;

    s_printf("[ACPI] Writing 0x%04x to PM1a_CNT (0x%x)\n",
             pm1a_value, found_fadt->pm1a_cnt_blk);
    outw_safe(found_fadt->pm1a_cnt_blk, pm1a_value);

    // Also try PM1b_CNT if available
    if (found_fadt->pm1b_cnt_blk != 0) {
        outw_safe(found_fadt->pm1b_cnt_blk, pm1a_value);
    }

    // If we get here, shutdown failed
    s_printf("[ACPI] Shutdown failed — system did not power off\n");
    return -1;
}

int acpi_reboot(void) {
    s_printf("[ACPI] Initiating reboot...\n");

    // Method 1: Try ACPI reset register (FADT 2.0+)
    if (found_fadt && found_fadt->header.revision >= 2) {
        // The reset register is a Generic Address Structure (GAS) at offset
        // 116 in FADT (after flags field). We stored it as reset_reg[12].
        // GAS format: address_space_id(1), register_bit_width(1),
        //             register_bit_offset(1), access_size(1), address(8)
        uint8_t addr_space_id = found_fadt->reset_reg[0];

        if (addr_space_id != 0) {
            // Extract the 64-bit address from GAS (bytes 4-11)
            uint32_t reset_addr = *(uint32_t*)(uintptr_t)&found_fadt->reset_reg[4];
            uint8_t reset_val = found_fadt->reset_value;

            s_printf("[ACPI] Reset register: space=%d addr=0x%x val=0x%x\n",
                     addr_space_id, reset_addr, reset_val);

            if (addr_space_id == 1) {
                // System I/O space
                outb((uint16_t)reset_addr, reset_val);
            } else if (addr_space_id == 0) {
                // System memory space
                *(volatile uint8_t*)(uintptr_t)reset_addr = reset_val;
            }
            // If we get here, ACPI reset didn't work
            s_printf("[ACPI] ACPI reset did not take effect\n");
        }
    }

    // Method 2: Keyboard controller reset (fallback)
    // Write 0xFE to port 0x64 (keyboard command register) to pulse reset line
    s_printf("[ACPI] Trying keyboard controller reset (port 0x64)\n");

    // Wait for keyboard controller input buffer to be empty
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }

    // Send reset command
    outb(0x64, 0xFE);

    // If we get here, reboot failed
    s_printf("[ACPI] Reboot failed — all methods exhausted\n");
    return -1;
}

const char* acpi_get_table_signature(const char* signature) {
    if (!found_rsdt || !signature) return (const char*)0;

    uint32_t entry_count = (found_rsdt->header.length - sizeof(acpi_header_t)) / sizeof(uint32_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t table_addr = ((uint32_t*)((uintptr_t)found_rsdt + sizeof(acpi_header_t)))[i];
        if (table_addr == 0) continue;

        acpi_header_t* header = (acpi_header_t*)(uintptr_t)table_addr;
        if (sig_match(header->signature, signature)) {
            return (const char*)header;
        }
    }

    return (const char*)0;
}

const rsdp_t* acpi_get_rsdp(void) {
    return found_rsdp;
}

const fadt_t* acpi_get_fadt(void) {
    return found_fadt;
}

const madt_t* acpi_get_madt(void) {
    return found_madt;
}

const hpet_t* acpi_get_hpet(void) {
    return found_hpet;
}

int acpi_is_available(void) {
    return acpi_available;
}
