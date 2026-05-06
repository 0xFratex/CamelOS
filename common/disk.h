// common/disk.h - Updated for PFS32 compatibility
#ifndef COMMON_DISK_H
#define COMMON_DISK_H

#include "types.h"

#define DISK_BLOCK_SIZE 512

extern uint32_t disk_total_blocks;

// Disk initialization and I/O functions
void disk_init(void);
int disk_read_block(uint32_t lba, void* buffer);
int disk_write_block(uint32_t lba, const void* buffer);

// Additional functions needed by pfs32
uint32_t get_tick_count(void);

#endif
