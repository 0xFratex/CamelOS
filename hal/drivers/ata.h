#ifndef ATA_H
#define ATA_H

#include "../../common/ports.h"
#include "../../include/types.h"

int ata_wait_busy(void);
int ata_wait_drq(void);
int ata_read_sector(int drive, uint64_t lba, uint8_t* buffer);
int ata_write_sector(int drive, uint64_t lba, const uint8_t* data);
void ata_io_wait(void);
void ata_identify_device(int drive);

typedef struct {
    uint64_t sectors;
    char model[41];
    int present;
    int lba48;          // Added field
} ide_device_t;

extern ide_device_t ide_devices[2];

#endif