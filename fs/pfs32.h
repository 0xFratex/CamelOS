// fs/pfs32.h - PFS32 File System Header (v2.0 Enhanced)
#ifndef PFS32_H
#define PFS32_H

#include "../include/types.h"

// Constants
#define PFS32_MAGIC 0x53465050  // "PF32"
#define PFS32_VERSION 2         // Bumped to 2.0
#define PFS32_BLOCK_SIZE 512
#define PFS32_END_BLOCK 0xFFFFFFFF
#define PFS32_FREE_BLOCK 0x00000000

// Attributes
#define PFS32_ATTR_READONLY  0x01
#define PFS32_ATTR_HIDDEN    0x02
#define PFS32_ATTR_SYSTEM    0x04
#define PFS32_ATTR_VOLUME    0x08
#define PFS32_ATTR_DIRECTORY 0x10
#define PFS32_ATTR_ARCHIVE   0x20
#define PFS32_ATTR_SYMLINK   0x40 // API-003

// Permissions (Revised for SEC-002)
// 8-bit packed: [Owner 3][Group 3][World 2]
// Owner: Bits 7-5 (RWX)
// Group: Bits 4-2 (RWX)
// World: Bits 1-0 (RX) - Write not supported for World in this packed format
#define PFS_PERM_READ  0x04
#define PFS_PERM_WRITE 0x02
#define PFS_PERM_EXEC  0x01

// Standardized Error Codes
#define PFS_OK           0
#define PFS_ERR_IO      -1
#define PFS_ERR_NO_FS   -2
#define PFS_ERR_FULL    -3
#define PFS_ERR_NOT_FOUND -4
#define PFS_ERR_EXISTS  -5
#define PFS_ERR_ACCESS  -6
#define PFS_ERR_NOT_EMPTY -7
#define PFS_ERR_PARAM   -8

// Superblock
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t fat_blocks;
    uint32_t data_start_block;
    uint32_t root_dir_block;
    uint32_t free_blocks;      
    uint32_t total_files;      
    char volume_label[32];
    uint8_t reserved[480];
} __attribute__((packed)) pfs32_superblock_t;

// Directory Entry (Modified for SEC-002 and FEAT-003)
typedef struct {
    char filename[40];
    uint32_t file_size;
    uint32_t start_block;
    uint8_t attributes;
    uint8_t uid;
    uint8_t permissions;   // New bit format
    uint8_t gid;           // Replaced res_byte with gid
    uint32_t create_time;  // Unix Timestamp
    uint32_t modify_time;  // Unix Timestamp
    uint32_t access_time;  // Unix Timestamp
} __attribute__((packed)) pfs32_direntry_t;

// Statistics Structure (DIAG-002)
typedef struct {
    uint32_t disk_reads;
    uint32_t disk_writes;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t alloc_retries;
} pfs32_stats_t;

// Core Functions
int pfs32_init(uint32_t disk_start, uint32_t disk_size);
int pfs32_format(const char* volume_label, uint32_t total_blocks);
int pfs32_sync(void);
int pfs32_fsck(int repair); // DIAG-001

// File Operations
int pfs32_create_file(const char* path);
int pfs32_create_directory(const char* path);
int pfs32_delete(const char* path);
int pfs32_rename(const char* oldpath, const char* newpath);
int pfs32_truncate(const char* path, uint32_t new_size); // FEAT-001
int pfs32_copy(const char* src, const char* dst);        // FEAT-002

int pfs32_read_file(const char* path, uint8_t* buffer, uint32_t max_size);
int pfs32_write_file(const char* path, uint8_t* data, uint32_t size);

// Directory Operations
int pfs32_listdir(uint32_t dir_block, pfs32_direntry_t* entries, uint32_t max_entries);
int pfs32_stat(const char* path, pfs32_direntry_t* entry);
int pfs32_get_stats(pfs32_stats_t* out_stats); // DIAG-002

// Internals exposed
int get_dir_block(const char* path, uint32_t* block);
// Helpers
uint32_t pfs32_time_now(void);

#endif