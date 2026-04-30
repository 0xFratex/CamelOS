#ifndef DMG_MOUNT_H
#define DMG_MOUNT_H

#include "../include/types.h"

// DMG Constants
#define DMG_KOLY_MAGIC      0x6b6f6c79  // "koly"
#define DMG_KOLY_SIZE       512
#define DMG_MAX_PARTITIONS  8
#define DMG_MAX_BLKMAP     256

// Compression types
#define DMG_COMP_RAW        0x00000001  // Raw/uncompressed
#define DMG_COMP_ZLIB       0x80000005  // zlib compressed
#define DMG_COMP_BZIP2      0x80000006  // bzip2
#define DMG_COMP_LZFSE      0x80000007  // LZFSE (Apple)
#define DMG_COMP_CRC        0x00000002  // CRC only (pass-through)

// Checksum types
#define DMG_CSUM_NONE       0
#define DMG_CSUM_CRC32      1
#define DMG_CSUM_SHA256     2

// Block map entry
typedef struct {
    uint32_t sector_number;       // Logical sector in output
    uint32_t compression_type;    // DMG_COMP_*
    uint32_t compressed_offset;   // Offset in DMG file
    uint32_t compressed_length;   // Compressed size
    uint32_t uncompressed_length; // Uncompressed size
} dmg_blkmap_entry_t;

// Partition info (from plist)
typedef struct {
    char name[64];
    char id[32];
    int block_map_start;
    int block_map_count;
    dmg_blkmap_entry_t* blocks;
} dmg_partition_t;

// KOLY trailer structure
typedef struct {
    uint32_t signature;           // 'koly'
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint64_t running_data_fork_offset;
    uint64_t data_fork_offset;
    uint64_t data_fork_length;
    uint64_t rsrc_fork_offset;
    uint64_t rsrc_fork_length;
    uint32_t segment_number;
    uint32_t segment_count;
    uint32_t segment_id[4];       // 128-bit segment ID
    uint32_t data_checksum_type;
    uint32_t data_checksum_size;
    uint32_t data_checksum[32];   // Up to 128 bytes
    uint64_t xml_plist_offset;
    uint64_t xml_plist_length;
    uint32_t master_checksum_type;
    uint32_t master_checksum_size;
    uint32_t master_checksum[32];
    uint32_t image_variant;
    uint64_t sector_count;
    // ... padding to 512
} dmg_koly_t;

// Mounted DMG state
typedef struct {
    char path[256];               // Path to .dmg file on PFS32
    dmg_koly_t koly;
    dmg_partition_t partitions[DMG_MAX_PARTITIONS];
    int partition_count;
    uint32_t total_sectors;
    int mounted;
    int writable;                 // 0 = read-only mount
    uint8_t* decompress_buf;      // Buffer for decompression
    uint32_t decompress_buf_size;
} dmg_mount_t;

// Maximum simultaneous mounted DMGs
#define DMG_MAX_MOUNTED 4

// Initialize DMG subsystem
void dmg_init_system(void);

// Mount a .dmg file
// Returns mount ID on success, -1 on failure
int dmg_mount(const char* path);

// Unmount a DMG
int dmg_unmount(int mount_id);

// Read from a mounted DMG (like reading a sector from a virtual disk)
int dmg_read_sector(int mount_id, uint32_t sector, uint8_t* buffer);

// Extract a .app bundle from a mounted DMG to /Applications/
// app_name: name of the .app bundle inside the DMG (e.g., "Safari.app")
int dmg_extract_app(int mount_id, const char* app_name);

// List .app bundles in a mounted DMG
int dmg_list_apps(int mount_id, char* app_names, int max_count, int max_name_len);

// Get mount info
const dmg_mount_t* dmg_get_mount_info(int mount_id);

// Drag-to-Applications install:
// Convenience function that mounts, extracts, and unmounts in one call
int dmg_install_to_applications(const char* dmg_path);

#endif
