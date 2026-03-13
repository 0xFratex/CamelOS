// installer/partition_tool.h - Partition Management Tool Header
#ifndef PARTITION_TOOL_H
#define PARTITION_TOOL_H

#include "../include/types.h"

// Partition types
#define PART_TYPE_EMPTY     0x00
#define PART_TYPE_FAT12     0x01
#define PART_TYPE_FAT16     0x06
#define PART_TYPE_NTFS      0x07
#define PART_TYPE_FAT32     0x0B
#define PART_TYPE_FAT32_LBA 0x0C
#define PART_TYPE_EXTENDED  0x0F
#define PART_TYPE_EXT4      0x83
#define PART_TYPE_PFS32     0x7F
#define PART_TYPE_CAMEL     0x7F  // Camel OS native

// Partition flags
#define PART_FLAG_BOOT      0x80
#define PART_FLAG_ACTIVE    0x80

// Partition entry (MBR)
typedef struct {
    uint8_t status;         // 0x80 = active/bootable
    uint8_t chs_start[3];   // CHS start (legacy)
    uint8_t type;           // Partition type
    uint8_t chs_end[3];     // CHS end (legacy)
    uint32_t lba_start;     // LBA start sector
    uint32_t lba_length;    // Length in sectors
} __attribute__((packed)) PartitionEntry;

// MBR structure
typedef struct {
    uint8_t bootstrap[446];
    PartitionEntry partitions[4];
    uint16_t signature;     // 0x55AA
} __attribute__((packed)) MBR;

// Partition info structure for UI
typedef struct {
    int index;              // 0-3
    uint8_t type;
    char type_name[32];
    uint32_t lba_start;
    uint32_t lba_length;
    uint32_t size_mb;
    uint32_t size_gb;
    int is_bootable;
    int is_camel_os;
    int is_empty;
    char label[32];
} PartitionInfo;

// Partition operation
typedef enum {
    PART_OP_NONE,
    PART_OP_CREATE,
    PART_OP_DELETE,
    PART_OP_FORMAT,
    PART_OP_RESIZE,
    PART_OP_SET_ACTIVE
} PartitionOperation;

// Partition tool state
typedef struct {
    int drive_index;
    MBR mbr;
    int has_mbr;
    PartitionInfo partitions[4];
    int partition_count;
    
    // Selection
    int selected_partition;
    int hover_partition;
    
    // Operations
    PartitionOperation pending_op;
    int operation_target;
    int operation_progress;
    char operation_status[64];
    
    // Free space calculation
    uint32_t total_sectors;
    uint32_t used_sectors;
    uint32_t free_sectors;
} PartitionTool;

// Public API
void partition_tool_init(int drive_index);
void partition_tool_refresh(void);

// Read/Write
int partition_tool_read_mbr(int drive_index);
int partition_tool_write_mbr(int drive_index);

// Operations
int partition_tool_create(int index, uint8_t type, uint32_t start, uint32_t size);
int partition_tool_delete(int index);
int partition_tool_format(int index, uint8_t fs_type);
int partition_tool_set_active(int index);

// Queries
PartitionInfo* partition_tool_get_info(int index);
int partition_tool_get_free_space(uint32_t* start, uint32_t* length);
int partition_tool_find_camel_partition(void);

// Rendering
void partition_tool_render_bar(int x, int y, int w, int h, int selected);
void partition_tool_render_details(int x, int y, int index);

// Utilities
const char* partition_type_name(uint8_t type);
void partition_format_size(uint32_t sectors, char* out);

#endif // PARTITION_TOOL_H
