#include "../common/ports.h"
#include "ata.h"
#include "../include/string.h"
#include "../hal/drivers/serial.h" // for s_printf

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

int ata_wait_bsy() {
    int t = 100000;
    while(t--) {
        if(!(inb(ATA_STATUS) & 0x80)) return 1;
        ata_delay();
    }
    return 0;
}

int ata_wait_drq() {
    int t = 100000;
    while(t--) {
        uint8_t status = inb(ATA_STATUS);
        if(status & 0x08) return 1;
        if(status & 0x01) return 0;
        ata_delay();
    }
    return 0;
}

// --- LBA28 read (original) ---
static int ata_read_sector_lba28(int drive, uint32_t lba, uint8_t* buffer) {
    if (drive > 1) return 1;
    if(!ata_wait_bsy()) return 1;
    
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

// --- LBA48 read ---
static int ata_read_sector_lba48(int drive, uint64_t lba, uint8_t* buffer) {
    if (drive > 1) return 1;
    if(!ata_wait_bsy()) return 1;
    
    outb(ATA_DRIVE, 0x40 | ((drive&1)<<4)); // LBA48 mode
    outb(ATA_SEC_CNT, 1);
    // Send high bytes first
    outb(ATA_LBA_LO, (uint8_t)(lba >> 24));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 32));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 40));
    // Then low bytes
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_CMD, 0x24); // READ SECTOR(S) EXT
    
    if(!ata_wait_drq()) return 1;
    
    uint16_t* b = (uint16_t*)buffer;
    for(int i=0; i<256; i++) b[i] = inw(ATA_DATA);
    return 0;
}

// --- LBA28 write ---
static int ata_write_sector_lba28(int drive, uint32_t lba, const uint8_t* buffer) {
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

// --- LBA48 write ---
static int ata_write_sector_lba48(int drive, uint64_t lba, const uint8_t* buffer) {
    if (drive > 1) return 1;
    if(!ata_wait_bsy()) return 1;
    
    outb(ATA_DRIVE, 0x40 | ((drive&1)<<4));
    outb(ATA_SEC_CNT, 1);
    outb(ATA_LBA_LO, (uint8_t)(lba >> 24));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 32));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 40));
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_CMD, 0x34); // WRITE SECTOR(S) EXT
    
    if(!ata_wait_drq()) return 1;
    
    uint16_t* b = (uint16_t*)buffer;
    for(int i=0; i<256; i++) outw(ATA_DATA, b[i]);
    
    outb(ATA_CMD, 0xE7); // Cache Flush
    if(!ata_wait_bsy()) return 1;
    return 0;
}

// --- Public entry points with retries ---
int ata_read_sector(int drive, uint64_t lba, uint8_t* buffer) {
    if (drive > 1) return 1;
    int retries = 3;
    int ret = 1;
    while(retries--) {
        if (ide_devices[drive].lba48)
            ret = ata_read_sector_lba48(drive, lba, buffer);
        else
            ret = ata_read_sector_lba28(drive, (uint32_t)lba, buffer);
        if (ret == 0) break;
        ata_delay();
    }
    return ret;
}

int ata_write_sector(int drive, uint64_t lba, const uint8_t* buffer) {
    if (drive > 1) return 1;
    int retries = 3;
    int ret = 1;
    while(retries--) {
        if (ide_devices[drive].lba48)
            ret = ata_write_sector_lba48(drive, lba, buffer);
        else
            ret = ata_write_sector_lba28(drive, (uint32_t)lba, buffer);
        if (ret == 0) break;
        ata_delay();
    }
    return ret;
}

void ata_swap_string(char* str, int len) {
    for(int i=0; i<len; i+=2) {
        char tmp = str[i];
        str[i] = str[i+1];
        str[i+1] = tmp;
    }
}

void ata_identify_device(int drive) {
    // Initialize device to safe defaults
    ide_devices[drive].present = 0;
    ide_devices[drive].sectors = 0;
    ide_devices[drive].lba48 = 0;
    ide_devices[drive].model[0] = 0;

    outb(ATA_DRIVE, drive == 0 ? 0xA0 : 0xB0);
    outb(ATA_SEC_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC);
    
    if (inb(ATA_STATUS) == 0) return;
    
    // Wait BSY with manual loop
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
    
    // Basic validation
    if ((data[0] & 0x7F) == 0 && data[1] == 0 && data[2] == 0) return;

    ide_devices[drive].present = 1;
    // Check LBA48 support (Word 83, Bit 10)
    int lba48_supported = ((data[83] & 0xC000) == 0x4000) && ((data[83] & (1 << 10)) != 0);
    ide_devices[drive].lba48 = lba48_supported;

    if (lba48_supported) {
        uint64_t lba48_sectors = (uint64_t)data[100] | ((uint64_t)data[101] << 16) | 
                                 ((uint64_t)data[102] << 32) | ((uint64_t)data[103] << 48);
        if (lba48_sectors > 0 && lba48_sectors < 0x1000000000ULL) {
            ide_devices[drive].sectors = lba48_sectors;
        } else {
            ide_devices[drive].sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
            ide_devices[drive].lba48 = 0; // fallback to LBA28
        }
    } else {
        ide_devices[drive].sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
    }
    
    // Extract model string from words 27-46
    char* model = ide_devices[drive].model;
    for(int i=0; i<20; i++) {
        uint16_t w = data[27 + i];
        model[i*2]     = (w >> 8) & 0xFF;
        model[i*2 + 1] = w & 0xFF;
    }
    model[40] = '\0';

    // Strip trailing spaces
    int len = 40;
    while(len > 0 && model[len-1] == ' ') len--;
    model[len] = '\0';

    // Filter out non-printable characters
    int has_printable = 0;
    for(int i = 0; i < len; i++) {
        if (model[i] < 0x20 || model[i] > 0x7E) {
            model[i] = ' ';
        } else {
            has_printable = 1;
        }
    }
    // Strip leading spaces
    while(len > 0 && model[0] == ' ') {
        memmove(model, model + 1, len);
        len--;
    }
    model[len] = '\0';

    if(!has_printable || model[0] == '\0') {
        model[0] = 'D'; model[1] = 'i'; model[2] = 's'; model[3] = 'k';
        model[4] = ' '; model[5] = '0' + drive; model[6] = '\0';
    }

    // Print disk information
    if (ide_devices[drive].present) {
        extern void s_printf(const char* fmt, ...);
        extern void sys_print(const char* str);
        char size_buf[32];
        extern void int_to_str(int, char*);

        s_printf("[ATA] Drive %d: %s", drive, model);
        s_printf(" (%u sectors, ", (unsigned int)ide_devices[drive].sectors);
        unsigned int size_mb = (unsigned int)(ide_devices[drive].sectors / 2048);
        int_to_str((int)size_mb, size_buf);
        s_printf("%s", size_buf);  // FIX: use format string
        s_printf(" MB)\n");

        sys_print("  Disk ");
        char drv_buf[4]; int_to_str(drive, drv_buf); sys_print(drv_buf);
        sys_print(": "); sys_print(model);
        sys_print(" ("); sys_print(size_buf); sys_print(" MB)\n");
    }
}