// common/disk.h - Updated for PFS32 compatibility
#ifndef DISK_H
#define DISK_H

// For freestanding environment, define basic types ourselves
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;

#define DISK_BLOCK_SIZE 512

extern uint32_t disk_total_blocks;

// Disk initialization and I/O functions
void disk_init(void);
int disk_read_block(uint32_t lba, void* buffer);
int disk_write_block(uint32_t lba, const void* buffer);

// Additional functions needed by pfs32
uint32_t get_tick_count(void);

#endif