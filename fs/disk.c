#include "disk.h"
#include "../hal/drivers/ata.h"
#include "../include/string.h"

static int fs_drive_id = 0;
uint32_t disk_total_blocks = 0;

int disk_get_drive(void) {
    return fs_drive_id;
}

#define DISK_CACHE_SIZE 16
static uint32_t disk_cache_block[DISK_CACHE_SIZE];
static uint8_t disk_cache_data[DISK_CACHE_SIZE][512];
static int disk_cache_dirty[DISK_CACHE_SIZE];
static int disk_cache_valid[DISK_CACHE_SIZE];
static int disk_cache_lru[DISK_CACHE_SIZE];
static int disk_access_counter = 0;

void disk_init(void) {
    for(int i=0; i<DISK_CACHE_SIZE; i++) {
        disk_cache_valid[i] = 0;
        disk_cache_dirty[i] = 0;
        disk_cache_lru[i] = 0;
    }
    disk_access_counter = 0;

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

void disk_flush_cache(void) {
    for(int i=0; i<DISK_CACHE_SIZE; i++) {
        if(disk_cache_valid[i] && disk_cache_dirty[i]) {
            int retries = 3;
            int ok = 0;
            while(retries > 0) {
                if(ata_write_sector(fs_drive_id, disk_cache_block[i], disk_cache_data[i]) == 0) {
                    ok = 1;
                    break;
                }
                retries--;
            }
            if(ok) {
                disk_cache_dirty[i] = 0;
            }
            /* If all retries failed, leave the entry dirty so it will be
               retried on the next flush attempt.  This prevents silent
               data loss when an ATA write fails. */
        }
    }
}

void disk_set_drive(int drive_id) {
    if(drive_id < 0 || drive_id > 1) return;
    
    disk_flush_cache();

    // Invalidate all cache entries after switching drives to prevent
    // stale data from the previous drive being served for the same block numbers
    for(int i = 0; i < DISK_CACHE_SIZE; i++) {
        disk_cache_valid[i] = 0;
        disk_cache_dirty[i] = 0;
        disk_cache_lru[i] = 0;
    }
    
    fs_drive_id = drive_id;
    
    if(ide_devices[drive_id].present) {
        disk_total_blocks = ide_devices[drive_id].sectors;
    } else {
        ata_identify_device(drive_id);
        disk_total_blocks = ide_devices[drive_id].sectors;
    }
}

static int disk_cache_lookup(uint32_t block) {
    for(int i=0; i<DISK_CACHE_SIZE; i++) {
        if(disk_cache_valid[i] && disk_cache_block[i] == block) {
            disk_cache_lru[i] = disk_access_counter++;
            return i;
        }
    }
    return -1;
}

static int disk_cache_evict(void) {
    int victim = 0;
    int min_lru = disk_cache_lru[0];
    
    for(int i=1; i<DISK_CACHE_SIZE; i++) {
        if(disk_cache_lru[i] < min_lru) {
            min_lru = disk_cache_lru[i];
            victim = i;
        }
    }
    
    if(disk_cache_valid[victim] && disk_cache_dirty[victim]) {
        ata_write_sector(fs_drive_id, disk_cache_block[victim], disk_cache_data[victim]);
    }
    
    disk_cache_valid[victim] = 0;
    disk_cache_dirty[victim] = 0;
    
    return victim;
}

int disk_read_block(uint32_t block, void* buffer) {
    if(block >= disk_total_blocks) return -1;
    
    int cache_idx = disk_cache_lookup(block);
    if(cache_idx >= 0) {
        memcpy(buffer, disk_cache_data[cache_idx], 512);
        return 0;
    }
    
    cache_idx = disk_cache_evict();
    disk_cache_block[cache_idx] = block;
    disk_cache_valid[cache_idx] = 1;
    disk_cache_dirty[cache_idx] = 0;
    disk_cache_lru[cache_idx] = disk_access_counter++;
    
    int ret = ata_read_sector(fs_drive_id, block, disk_cache_data[cache_idx]);
    if(ret != 0) {
        disk_cache_valid[cache_idx] = 0;
        return ret;
    }
    
    memcpy(buffer, disk_cache_data[cache_idx], 512);
    return 0;
}

int disk_write_block(uint32_t block, const void* buffer) {
    if(block >= disk_total_blocks) return -1;
    
    int cache_idx = disk_cache_lookup(block);
    if(cache_idx < 0) {
        cache_idx = disk_cache_evict();
        disk_cache_block[cache_idx] = block;
        disk_cache_valid[cache_idx] = 1;
        disk_cache_lru[cache_idx] = disk_access_counter++;
    }
    
    memcpy(disk_cache_data[cache_idx], buffer, 512);
    disk_cache_dirty[cache_idx] = 1;
    
    return 0;
}
