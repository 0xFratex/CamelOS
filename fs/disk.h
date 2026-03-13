#ifndef DISK_H
#define DISK_H

// Baremetal typedefs
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef unsigned int uint32_t;
typedef signed int int32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;

#define DISK_BLOCK_SIZE 512

extern uint32_t disk_total_blocks;

void disk_init(void);
void disk_set_drive(int drive_id);
int disk_read_block(uint32_t block, void* buffer);
int disk_write_block(uint32_t block, const void* buffer);

#endif
