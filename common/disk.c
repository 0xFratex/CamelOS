// common/disk.c - Updated for PFS32 compatibility
#include "disk.h"
#include "memory.h"

// For freestanding environment, define basic types ourselves
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

uint32_t disk_total_blocks = 0;

// Disk initialization
void disk_init(void) {
    // Initialize disk subsystem
    disk_total_blocks = 1024; // Example: 512KB disk with 512-byte blocks
}

// Read a block from disk
int disk_read_block(uint32_t lba, void* buffer) {
    // Simple implementation - in real OS would use ATA/IDE
    if (lba >= disk_total_blocks) {
        return -1; // Invalid block
    }
    
    // For now, just clear the buffer
    memset(buffer, 0, DISK_BLOCK_SIZE);
    return 0;
}

// Write a block to disk  
int disk_write_block(uint32_t lba, const void* buffer) {
    // Simple implementation - in real OS would use ATA/IDE
    if (lba >= disk_total_blocks) {
        return -1; // Invalid block
    }
    
    // For now, just return success
    return 0;
}
