#include "disk.h"
#include "../hal/drivers/ata.h"

// The currently active drive for the Filesystem (0 = Master, 1 = Slave)
static int fs_drive_id = 0;

uint32_t disk_total_blocks = 0;

void disk_init(void) {
    // Identify master by default
    ata_identify_device(0);
    if(ide_devices[0].present) {
        disk_total_blocks = ide_devices[0].sectors;
        fs_drive_id = 0;
    } else {
        ata_identify_device(1);
        if(ide_devices[1].present) {
            disk_total_blocks = ide_devices[1].sectors;
            fs_drive_id = 1;
        }
    }
}

// Allow Installer/Kernel to change the active disk
void disk_set_drive(int drive_id) {
    fs_drive_id = drive_id;
    if(ide_devices[drive_id].present) {
        disk_total_blocks = ide_devices[drive_id].sectors;
    } else {
        // Probe it just in case
        ata_identify_device(drive_id);
        disk_total_blocks = ide_devices[drive_id].sectors;
    }
}

int disk_read_block(uint32_t block, void* buffer) {
    return ata_read_sector(fs_drive_id, block, (uint8_t*)buffer);
}

int disk_write_block(uint32_t block, const void* buffer) {
    return ata_write_sector(fs_drive_id, block, (const uint8_t*)buffer);
}
