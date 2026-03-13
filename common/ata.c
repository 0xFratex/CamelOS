#include "../common/ports.h"
#include "ata.h"
#include "../../core/string.h"

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
    int t = 1000000;
    while(t--) {
        if(!(inb(ATA_STATUS) & 0x80)) return 0;
    }
    return 1;
}

int ata_wait_drq() {
    int t = 1000000;
    while(t--) {
        if(inb(ATA_STATUS) & 0x08) return 0;
    }
    return 1;
}

int ata_read_sector(int drive, uint32_t lba, uint8_t* buffer) {
    if (drive > 1) return 1;
    if(ata_wait_bsy()) return 1;
    
    outb(ATA_DRIVE, 0xE0 | ((drive&1)<<4) | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_CNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba>>8));
    outb(ATA_LBA_HI, (uint8_t)(lba>>16));
    outb(ATA_CMD, 0x20); 
    
    if(ata_wait_drq()) return 1;
    
    uint16_t* b = (uint16_t*)buffer;
    for(int i=0; i<256; i++) b[i] = inw(ATA_DATA);
    return 0;
}

int ata_write_sector(int drive, uint32_t lba, const uint8_t* buffer) {
    if (drive > 1) return 1;
    if(ata_wait_bsy()) return 1;
    
    outb(ATA_DRIVE, 0xE0 | ((drive&1)<<4) | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_CNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba>>8));
    outb(ATA_LBA_HI, (uint8_t)(lba>>16));
    outb(ATA_CMD, 0x30); 
    
    if(ata_wait_drq()) return 1;
    
    uint16_t* b = (uint16_t*)buffer;
    for(int i=0; i<256; i++) outw(ATA_DATA, b[i]);
    
    outb(ATA_CMD, 0xE7); 
    ata_wait_bsy();
    return 0;
}

void ata_swap_string(char* str, int len) {
    for(int i=0; i<len; i+=2) {
        char tmp = str[i];
        str[i] = str[i+1];
        str[i+1] = tmp;
    }
}

// THIS IS THE CRITICAL FIX
void ata_identify_device(int drive) {
    ide_devices[drive].present = 0;

    // 1. Select Drive
    outb(ATA_DRIVE, drive == 0 ? 0xA0 : 0xB0);
    outb(ATA_SEC_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    
    // 2. Send IDENTIFY
    outb(ATA_CMD, 0xEC);
    
    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return; 
    
    while(inb(ATA_STATUS) & 0x80); // Wait BSY
    
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return; 
    
    int timeout = 100000;
    while(timeout--) {
        status = inb(ATA_STATUS);
        if (status & 0x08) break; // DRQ
        if (status & 0x01) return; // ERR
    }
    if (timeout <= 0) return;

    // 3. Read Data
    uint16_t data[256];
    for(int i=0; i<256; i++) data[i] = inw(ATA_DATA);
    
    ide_devices[drive].present = 1;
    
    // 4. Calculate Sectors (Words 60 & 61)
    ide_devices[drive].sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
    
    // 5. Get Model
    char* model_dest = ide_devices[drive].model;
    for(int i=0; i<20; i++) {
        uint16_t w = data[27 + i];
        model_dest[i*2] = (w >> 8) & 0xFF;   
        model_dest[i*2 + 1] = w & 0xFF;      
    }
    ata_swap_string(model_dest, 40);
    model_dest[40] = 0;
}