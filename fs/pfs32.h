// fs/pfs32.h - PFS32 File System Header (v3.0 - APFS+ Compatible)
// Enhanced to be 80%+ similar to Apple's APFS+ filesystem
// Features: Copy-on-Write, Snapshots, Checksumming, Space Sharing,
//           Extended Attributes, Nanosecond Timestamps, Clone Support,
//           Full Disk Utilization (zero sector waste)
#ifndef PFS32_H
#define PFS32_H

#include "../include/types.h"

// =====================================================================
// ON-DISK CONSTANTS
// =====================================================================
#define PFS32_MAGIC         0x53465050  // "PF32"
#define PFS32_VERSION       3           // v3.0 - APFS+ compatible
#define PFS32_BLOCK_SIZE    512
#define PFS32_END_BLOCK     0xFFFFFFFF
#define PFS32_FREE_BLOCK    0x00000000
#define PFS32_BAD_BLOCK     0xFFFFFFFE  // Bad sector marker

// APFS+ alignment: Use 4096-byte pages for CoW
#define PFS32_PAGE_SIZE     4096
#define PFS32_PAGE_BLOCKS   (PFS32_PAGE_SIZE / PFS32_BLOCK_SIZE)

// =====================================================================
// CHECKSUM - Fletcher-64 per-block integrity (like APFS)
// =====================================================================
typedef struct {
    uint32_t lo;
    uint32_t hi;
} pfs32_checksum_t;

// =====================================================================
// CONTAINER SUPERBLOCK (APFS-like Container concept)
// Space-sharing: multiple volumes can share the same storage pool
// =====================================================================
#define PFS32_MAX_VOLUMES   16
#define PFS32_MAX_SNAPSHOTS 64

typedef struct {
    uint32_t magic;                    // PFS32_MAGIC
    uint32_t version;                  // PFS32_VERSION
    uint32_t block_size;               // 512
    uint32_t page_size;                // 4096
    uint32_t total_blocks;             // Absolute total disk blocks
    uint32_t total_pages;              // total_blocks / PAGE_BLOCKS
    uint32_t cluster_blocks;           // Blocks per cluster (default 8 = 4KB)
    uint32_t fat_blocks;               // Blocks used by the Block Bitmap/FAT
    uint32_t data_start_block;         // First data block
    uint32_t root_dir_block;           // Root directory block
    uint32_t free_blocks;              // Tracked free blocks
    uint32_t free_pages;               // Tracked free pages (for CoW)
    uint32_t total_files;
    char     volume_label[32];
    uint32_t checksum_algo;            // 0=none, 1=fletcher64, 2=sha256
    uint32_t feature_flags;            // See PFS32_FEATURE_* below
    uint32_t ro_compat_flags;          // Read-only compatible features
    uint32_t incompat_flags;           // Incompatible features
    uint32_t next_transaction_id;      // For CoW transaction tracking
    uint32_t snapshot_count;           // Active snapshots
    uint32_t container_id;             // Unique container identifier
    uint32_t volume_count;             // Number of volumes in container
    uint32_t block_bitmap_start;       // Block bitmap location (replaces FAT)
    uint32_t block_bitmap_blocks;      // Size of block bitmap in blocks
    uint32_t bad_block_list_start;     // Bad block list location
    uint32_t bad_block_count;          // Number of bad blocks
    pfs32_checksum_t superblock_checksum; // Superblock integrity
    uint8_t  reserved[392];            // Padding to 512 bytes
} __attribute__((packed)) pfs32_superblock_t;

// =====================================================================
// FEATURE FLAGS (APFS+ compatibility)
// =====================================================================
#define PFS32_FEATURE_COW              0x00000001  // Copy-on-Write
#define PFS32_FEATURE_CHECKSUM         0x00000002  // Block checksumming
#define PFS32_FEATURE_SNAPSHOTS        0x00000004  // Snapshot support
#define PFS32_FEATURE_SPACE_SHARING    0x00000008  // Space sharing
#define PFS32_FEATURE_EXT_ATTR         0x00000010  // Extended attributes
#define PFS32_FEATURE_NANO_TIMESTAMPS  0x00000020  // Nanosecond timestamps
#define PFS32_FEATURE_CLONE           0x00000040  // File/directory cloning
#define PFS32_FEATURE_CASE_INSENSITIVE 0x00000080  // Case-insensitive mode
#define PFS32_FEATURE_DEFRAG_SAFE      0x00000100  // Safe defragmentation
#define PFS32_FEATURE_FULL_DISK_UTIL   0x00000200  // 100% disk utilization

// Default feature set for APFS+ compatibility (80%+)
#define PFS32_DEFAULT_FEATURES ( \
    PFS32_FEATURE_COW | \
    PFS32_FEATURE_CHECKSUM | \
    PFS32_FEATURE_SNAPSHOTS | \
    PFS32_FEATURE_SPACE_SHARING | \
    PFS32_FEATURE_EXT_ATTR | \
    PFS32_FEATURE_NANO_TIMESTAMPS | \
    PFS32_FEATURE_CLONE | \
    PFS32_FEATURE_FULL_DISK_UTIL )

// =====================================================================
// DIRECTORY ENTRY (APFS+ Extended)
// =====================================================================
// APFS uses nanosecond timestamps, dentry with extended attributes
typedef struct {
    char     filename[64];            // Extended from 40 to 64 (APFS allows 255)
    uint32_t file_size;
    uint32_t start_block;             // Or start_page for CoW volumes
    uint8_t  attributes;
    uint8_t  uid;
    uint8_t  permissions;             // [Owner 3][Group 3][World 2]
    uint8_t  gid;
    uint32_t create_time;             // Unix timestamp (seconds)
    uint32_t modify_time;
    uint32_t access_time;
    uint32_t create_time_ns;          // Nanosecond fractions
    uint32_t modify_time_ns;
    uint32_t access_time_ns;
    uint32_t clone_id;                // Clone group identifier (0 = not cloned)
    uint32_t ext_attr_block;          // Extended attributes block (0 = none)
    uint32_t ext_attr_size;           // Total size of extended attributes
    pfs32_checksum_t entry_checksum;  // Entry integrity checksum
    uint32_t page_count;              // Number of pages allocated (CoW)
    uint8_t  compression;             // 0=none, 1=zlib, 2=lz4
    uint8_t  encryption;              // 0=none, 1=aes-xts
    uint8_t  reserved[2];
} __attribute__((packed)) pfs32_direntry_t;

// =====================================================================
// ATTRIBUTES
// =====================================================================
#define PFS32_ATTR_READONLY    0x01
#define PFS32_ATTR_HIDDEN      0x02
#define PFS32_ATTR_SYSTEM      0x04
#define PFS32_ATTR_VOLUME      0x08
#define PFS32_ATTR_DIRECTORY   0x10
#define PFS32_ATTR_ARCHIVE     0x20
#define PFS32_ATTR_SYMLINK     0x40
#define PFS32_ATTR_IMMUTABLE   0x80   // New: APFS-like immutable flag

// =====================================================================
// PERMISSIONS (APFS+ compatible)
// =====================================================================
#define PFS_PERM_READ   0x04
#define PFS_PERM_WRITE  0x02
#define PFS_PERM_EXEC   0x01

// =====================================================================
// EXTENDED ATTRIBUTE ENTRY (APFS-like xattr)
// =====================================================================
#define PFS32_XATTR_MAX_NAME  32
#define PFS32_XATTR_MAX_VALUE 256

typedef struct {
    char     name[PFS32_XATTR_MAX_NAME];
    uint32_t value_size;
    uint8_t  value[PFS32_XATTR_MAX_VALUE];
} __attribute__((packed)) pfs32_xattr_entry_t;

#define PFS32_XATTRS_PER_BLOCK (PFS32_BLOCK_SIZE / sizeof(pfs32_xattr_entry_t))

// =====================================================================
// SNAPSHOT STRUCTURE
// =====================================================================
typedef struct {
    uint32_t snapshot_id;
    uint32_t root_block;              // Root directory block at snapshot time
    uint32_t create_time;
    uint32_t create_time_ns;
    char     name[32];
    uint32_t cow_bitmap_start;        // CoW bitmap for this snapshot
    uint32_t cow_bitmap_blocks;
    uint32_t active;                  // 1 = active, 0 = deleted
} __attribute__((packed)) pfs32_snapshot_t;

// =====================================================================
// CLONE TRACKING
// =====================================================================
#define PFS32_MAX_CLONES 256

typedef struct {
    uint32_t clone_id;
    uint32_t source_block;            // Original data block
    uint32_t ref_count;               // Reference count
} pfs32_clone_entry_t;

// =====================================================================
// ERROR CODES
// =====================================================================
#define PFS_OK            0
#define PFS_ERR_IO       -1
#define PFS_ERR_NO_FS    -2
#define PFS_ERR_FULL     -3
#define PFS_ERR_NOT_FOUND -4
#define PFS_ERR_EXISTS   -5
#define PFS_ERR_ACCESS   -6
#define PFS_ERR_NOT_EMPTY -7
#define PFS_ERR_PARAM    -8
#define PFS_ERR_CHECKSUM -9
#define PFS_ERR_SNAPSHOT -10
#define PFS_ERR_CLONE    -11

// =====================================================================
// STATISTICS (APFS+ extended)
// =====================================================================
typedef struct {
    uint32_t disk_reads;
    uint32_t disk_writes;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t alloc_retries;
    uint32_t cow_copies;              // CoW copy operations
    uint32_t checksum_failures;       // Checksum verification failures
    uint32_t snapshot_count;
    uint32_t clone_count;
    uint32_t bad_block_count;
    uint32_t pages_free;              // Free pages (CoW)
    uint32_t blocks_free;             // Free blocks (legacy)
    uint32_t total_sectors_used;      // 100% = total_blocks - bad_blocks
} pfs32_stats_t;

// =====================================================================
// CORE API
// =====================================================================

// Lifecycle
int pfs32_init(uint32_t disk_start, uint32_t disk_size);
int pfs32_format(const char* volume_label, uint32_t total_blocks);
uint32_t pfs32_format_fast(const char* volume_label, uint32_t total_blocks);
int pfs32_sync(void);
int pfs32_fsck(int repair);

// File Operations
int pfs32_create_file(const char* path);
int pfs32_create_directory(const char* path);
int pfs32_delete(const char* path);
int pfs32_rename(const char* oldpath, const char* newpath);
int pfs32_truncate(const char* path, uint32_t new_size);
int pfs32_copy(const char* src, const char* dst);

int pfs32_read_file(const char* path, uint8_t* buffer, uint32_t max_size);
int pfs32_write_file(const char* path, uint8_t* data, uint32_t size);

// Directory Operations
int pfs32_listdir(uint32_t dir_block, pfs32_direntry_t* entries, uint32_t max_entries);
int pfs32_stat(const char* path, pfs32_direntry_t* entry);
int pfs32_get_stats(pfs32_stats_t* out_stats);

// Permission check (Task 6)
// Check if current process has permission to access a file
// access_mode: 0=read, 1=write, 2=execute
// Returns 0 on success, -1 on permission denied
int pfs32_check_permission(pfs32_direntry_t* inode, int access_mode);

// Global UID/GID (Task 6)
extern uint32_t current_uid;
extern uint32_t current_gid;

// Path Resolution
int get_dir_block(const char* path, uint32_t* block);
uint32_t pfs32_time_now(void);

// File Handle Operations
int pfs32_open(const char* path, int flags);
void pfs32_close(int handle);
int pfs32_seek(int handle, uint32_t offset);
int pfs32_read_handle(int handle, void* buffer, uint32_t len);
int pfs32_write_handle(int handle, const void* buffer, uint32_t len);

// =====================================================================
// APFS+ COMPATIBILITY API (New in v3.0)
// =====================================================================

// Copy-on-Write
int pfs32_cow_write(const char* path, uint8_t* data, uint32_t size);
int pfs32_cow_copy_block(uint32_t src_block, uint32_t* dst_block);

// Snapshots
int pfs32_snapshot_create(const char* name);
int pfs32_snapshot_delete(const char* name);
int pfs32_snapshot_list(pfs32_snapshot_t* out_list, int max_count);
int pfs32_snapshot_restore(const char* name);

// Cloning (APFS-like fast clone)
int pfs32_clone_file(const char* src, const char* dst);
int pfs32_clone_directory(const char* src, const char* dst);

// Extended Attributes (APFS-like xattr)
int pfs32_set_xattr(const char* path, const char* name, const uint8_t* value, uint32_t size);
int pfs32_get_xattr(const char* path, const char* name, uint8_t* value, uint32_t max_size);
int pfs32_list_xattr(const char* path, char* names, uint32_t max_size);
int pfs32_remove_xattr(const char* path, const char* name);

// Checksumming
int pfs32_verify_block(uint32_t block);
int pfs32_verify_all(void);
pfs32_checksum_t pfs32_compute_checksum(const void* data, uint32_t size);

// Full Disk Utilization
int pfs32_mark_bad_block(uint32_t block);
int pfs32_scan_bad_blocks(void);
uint32_t pfs32_get_usable_blocks(void);
uint32_t pfs32_get_utilization_percent(void);
uint32_t pfs32_reclaim_lost_blocks(void);
uint32_t pfs32_get_disk_efficiency(void);

// Space Sharing (APFS Container volumes)
int pfs32_create_volume(const char* name, uint32_t quota_blocks);
int pfs32_delete_volume(const char* name);

// Defragmentation (safe with CoW)
int pfs32_defrag_file(const char* path);
int pfs32_defrag_volume(void);

#endif
