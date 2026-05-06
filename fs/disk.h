#ifndef FS_DISK_H
#define FS_DISK_H

#include "types.h"

#define DISK_BLOCK_SIZE 512

extern uint32_t disk_total_blocks;

void disk_init(void);
void disk_set_drive(int drive_id);
void disk_flush_cache(void);
int disk_read_block(uint32_t block, void* buffer);
int disk_write_block(uint32_t block, const void* buffer);

#endif
