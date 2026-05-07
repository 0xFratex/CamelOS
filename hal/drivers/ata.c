#include "../common/ports.h"
#include "ata.h"
#include "../include/string.h"

#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1
#define ATA_SEC_CNT 0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD 0x1F7

ide_device_t ide_devices[2];

void ata_delay() { 
    for(int i=0; i<4; i++) inb(0x3F6); 
}

// Fixed: Added timeout return (0 = Error/Timeout, 1 = OK)
int ata_wait_bsy() {
    int t = 100000; // Timeout ~100ms
    while(t--) {
        if(!(inb(ATA_STATUS) & 0x80)) return 1;
        ata_delay();
    }
    return 0; // Timed out
}

int ata_wait_drq() {
    int t = 100000;
    while(t--) {
        if(inb(ATA_STATUS) & 0x08) return 1;
        if(inb(ATA_STATUS) & 0x01) return 0; // Error bit
        ata_delay();
    }
    return 0;
}

int ata_read_sector(int drive, uint32_t lba, uint8_t* buffer) {
    if (drive > 1) return 1;
    if(!ata_wait_bsy()) return 1; // Timeout
    
    outb(ATA_DRIVE, 0xE0 | ((drive&1)<<4) | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_CNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba>>8));
    outb(ATA_LBA_HI, (uint8_t)(lba>>16));
    outb(ATA_CMD, 0x20); 
    
    if(!ata_wait_drq()) return 1;
    
    uint16_t* b = (uint16_t*)buffer;
    for(int i=0; i<256; i++) b[i] = inw(ATA_DATA);
    return 0;
}

int ata_write_sector(int drive, uint32_t lba, const uint8_t* buffer) {
    if (drive > 1) return 1;
    if(!ata_wait_bsy()) return 1;
    
    outb(ATA_DRIVE, 0xE0 | ((drive&1)<<4) | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_CNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba>>8));
    outb(ATA_LBA_HI, (uint8_t)(lba>>16));
    outb(ATA_CMD, 0x30); 
    
    if(!ata_wait_drq()) return 1;
    
    uint16_t* b = (uint16_t*)buffer;
    for(int i=0; i<256; i++) outw(ATA_DATA, b[i]);
    
    outb(ATA_CMD, 0xE7); // Cache Flush
    if(!ata_wait_bsy()) return 1;
    
    return 0;
}

void ata_swap_string(char* str, int len) {
    for(int i=0; i<len; i+=2) {
        char tmp = str[i];
        str[i] = str[i+1];
        str[i+1] = tmp;
    }
}

void ata_identify_device(int drive) {
    // Initialize device to safe defaults first
    ide_devices[drive].present = 0;
    ide_devices[drive].sectors = 0;
    ide_devices[drive].model[0] = 0;

    outb(ATA_DRIVE, drive == 0 ? 0xA0 : 0xB0);
    outb(ATA_SEC_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC);
    
    if (inb(ATA_STATUS) == 0) return;
    
    // Wait BSY with manual loop to avoid immediate fail on slow emulators
    int retry = 10000; while(retry-- && (inb(ATA_STATUS) & 0x80));

    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return;
    
    int t = 100000;
    while(t--) {
        uint8_t s = inb(ATA_STATUS);
        if (s & 0x08) break;
        if (s & 0x01) return;
    }
    
    uint16_t data[256];
    for(int i=0; i<256; i++) data[i] = inw(ATA_DATA);
    
    // Validate identify data: word 0 should have reasonable values
    // Bits 5:0 of word 0 should be non-zero for a real ATA device
    if ((data[0] & 0x7F) == 0 && data[1] == 0 && data[2] == 0) {
        // Data looks like all zeros or garbage — abort
        return;
    }

    ide_devices[drive].present = 1;
    // Check if LBA48 is supported (Word 83, Bit 10)
    // Word 83 bits 15:14 must be 01 (valid bits) per ATA spec
    int lba48_supported = ((data[83] & 0xC000) == 0x4000) && ((data[83] & (1 << 10)) != 0);
    if (lba48_supported) {
        // LBA48 sector count: words 100-103 (little-endian 64-bit)
        uint64_t lba48_sectors = (uint64_t)data[100] | ((uint64_t)data[101] << 16) | 
                                 ((uint64_t)data[102] << 32) | ((uint64_t)data[103] << 48);
        // Sanity check: LBA48 count should be reasonable (non-zero, not absurdly large)
        // Max real disk ~16TB = ~3.4e10 sectors. Reject obviously garbage values.
        if (lba48_sectors > 0 && lba48_sectors < 0x1000000000ULL) {
            ide_devices[drive].sectors = lba48_sectors;
        } else {
            // Fall back to LBA28
            ide_devices[drive].sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
        }
    } else {
        ide_devices[drive].sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
    }
    
    // Extract model string from words 27-46 (40 characters)
    // ATA stores strings with first character in the HIGH byte of each word.
    // We extract high byte first → correct character order, no swap needed.
    char* model = ide_devices[drive].model;
    for(int i=0; i<20; i++) {
        uint16_t w = data[27 + i];
        model[i*2]     = (w >> 8) & 0xFF;  // First char (high byte)
        model[i*2 + 1] = w & 0xFF;         // Second char (low byte)
    }
    // NOTE: Do NOT call ata_swap_string() here — the extraction above
    // already puts characters in the correct order. Swapping would corrupt it.
    model[40] = '\0';  // Null-terminate

    // Strip trailing spaces
    int len = 40;
    while(len > 0 && model[len-1] == ' ') len--;
    model[len] = '\0';

    // Filter out non-printable characters (replace with spaces)
    int has_printable = 0;
    for(int i = 0; i < len; i++) {
        if (model[i] < 0x20 || model[i] > 0x7E) {
            model[i] = ' ';
        } else {
            has_printable = 1;
        }
    }
    // Strip leading spaces too
    while(len > 0 && model[0] == ' ') {
        memmove(model, model + 1, len);
        len--;
    }
    model[len] = '\0';

    // If no printable characters at all, use a default name
    if(!has_printable || model[0] == '\0') {
        model[0] = 'D'; model[1] = 'i'; model[2] = 's'; model[3] = 'k';
        model[4] = ' '; model[5] = '0' + drive; model[6] = '\0';
    }

    // Print disk model and size to both serial and VGA
    if (ide_devices[drive].present) {
        extern void s_printf(const char* fmt, ...);
        extern void sys_print(const char* str);
        char size_buf[32];
        extern void int_to_str(int, char*);

        s_printf("[ATA] Drive %d: %s", drive, model);
        s_printf(" (%u sectors, ", (unsigned int)ide_devices[drive].sectors);
        // Compute size in MB (sectors * 512 / 1048576)
        unsigned int size_mb = (unsigned int)(ide_devices[drive].sectors / 2048);
        int_to_str((int)size_mb, size_buf);
        s_printf(size_buf);
        s_printf(" MB)\n");

        // VGA-visible message
        sys_print("  Disk ");
        char drv_buf[4]; int_to_str(drive, drv_buf); sys_print(drv_buf);
        sys_print(": "); sys_print(model);
        sys_print(" ("); sys_print(size_buf); sys_print(" MB)\n");
    }
}