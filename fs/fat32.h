/*
 * fat32.h - FAT32 Filesystem Driver for CamelOS
 *
 * Provides FAT32 support for interoperability with other operating systems
 * (USB drives, SD cards, shared partitions). Implements read-first with
 * write support for full filesystem operations.
 *
 * Reference: Microsoft FAT32 Specification (fatgen103.doc)
 */

#ifndef FAT32_H
#define FAT32_H

#include "../include/types.h"

/* ========================================================================
 * ON-DISK CONSTANTS
 * ======================================================================== */

#define FAT32_SECTOR_SIZE          512
#define FAT32_DIR_ENTRY_SIZE       32
#define FAT32_ENTRIES_PER_SECTOR   (FAT32_SECTOR_SIZE / FAT32_DIR_ENTRY_SIZE)

/* FAT cluster markers (lower 28 bits significant) */
#define FAT32_FREE_CLUSTER         0x00000000
#define FAT32_RESERVED_CLUSTER     0x00000001
#define FAT32_BAD_CLUSTER          0x0FFFFFF7
#define FAT32_EOC_MARKER_MIN       0x0FFFFFF8  /* End-of-chain minimum */
#define FAT32_EOC_MARKER           0x0FFFFFFF  /* End-of-chain maximum */
#define FAT32_CLUSTER_MASK         0x0FFFFFFF  /* Lower 28 bits only */

/* BPB signatures */
#define FAT32_BPB_SIGNATURE        0xAA55
#define FAT32_FAT_TYPE_SIGNATURE   0x29       /* Extended BPB signature */

/* LFN directory entry attributes */
#define FAT32_ATTR_READ_ONLY       0x01
#define FAT32_ATTR_HIDDEN          0x02
#define FAT32_ATTR_SYSTEM          0x04
#define FAT32_ATTR_VOLUME_ID       0x08
#define FAT32_ATTR_DIRECTORY       0x10
#define FAT32_ATTR_ARCHIVE         0x20
#define FAT32_ATTR_LFN             0x0F       /* Long file name entry */
#define FAT32_ATTR_DEVICE          0x40

/* LFN constants */
#define FAT32_LFN_MAX_CHARS        13         /* Max chars per LFN entry */
#define FAT32_LFN_MAX_NAME_LEN     255        /* Max LFN length */
#define FAT32_LFN_SEQ_MASK         0x3F       /* Sequence number mask */
#define FAT32_LFN_SEQ_START        0x01       /* First sequence number */
#define FAT32_LFN_SEQ_END          0x40       /* Last entry flag */

/* Driver limits */
#define FAT32_MAX_OPEN_FILES       8
#define FAT32_FAT_CACHE_SECTORS    16         /* Number of cached FAT sectors */
#define FAT32_MAX_PATH_LEN         256
#define FAT32_MAX_DIR_ENTRIES      128        /* Max entries for readdir */
#define FAT32_MAX_CLUSTER_CHAIN    65536      /* Max clusters in a chain */

/* ========================================================================
 * ERROR CODES
 * ======================================================================== */

#define FAT32_OK                  0
#define FAT32_ERR_IO             -1            /* Disk I/O error */
#define FAT32_ERR_NO_FS          -2            /* Not a FAT32 filesystem */
#define FAT32_ERR_NOT_FOUND      -3            /* File/directory not found */
#define FAT32_ERR_EXISTS         -4            /* File/directory already exists */
#define FAT32_ERR_NO_SPACE       -5            /* Disk full */
#define FAT32_ERR_PARAM          -6            /* Invalid parameter */
#define FAT32_ERR_NO_MEM         -7            /* Out of memory */
#define FAT32_ERR_ACCESS         -8            /* Permission / access denied */
#define FAT32_ERR_NOT_EMPTY      -9            /* Directory not empty */
#define FAT32_ERR_NOT_DIR        -10           /* Not a directory */
#define FAT32_ERR_IS_DIR         -11           /* Is a directory */
#define FAT32_ERR_BAD_CLUSTER    -12           /* Bad cluster encountered */
#define FAT32_ERR_HANDLE         -13           /* Invalid file handle */
#define FAT32_ERR_READ_ONLY      -14           /* Write to read-only file */
#define FAT32_ERR_NAME           -15           /* Invalid filename */
#define FAT32_ERR_PATH           -16           /* Invalid path */
#define FAT32_ERR_BUSY           -17           /* Resource busy */

/* ========================================================================
 * OPEN FLAGS
 * ======================================================================== */

#define FAT32_O_RDONLY           0x0000       /* Read only */
#define FAT32_O_WRONLY           0x0001       /* Write only */
#define FAT32_O_RDWR             0x0002       /* Read/Write */
#define FAT32_O_CREAT            0x0100       /* Create if not exists */
#define FAT32_O_EXCL             0x0200       /* Fail if exists */
#define FAT32_O_TRUNC            0x0400       /* Truncate to zero */
#define FAT32_O_APPEND           0x0800       /* Append mode */
#define FAT32_O_DIRECTORY        0x1000       /* Open as directory */

/* SEEK WHENCE */
#define FAT32_SEEK_SET           0
#define FAT32_SEEK_CUR           1
#define FAT32_SEEK_END           2

/* ========================================================================
 * BIOS PARAMETER BLOCK (BPB) - Sector 0 of partition
 * ======================================================================== */

typedef struct {
    uint8_t  jmp_boot[3];           /* Jump instruction + NOP */
    char     oem_name[8];           /* OEM name "MSWIN4.1" typically */
    uint16_t bytes_per_sector;      /* Must be 512 */
    uint8_t  sectors_per_cluster;   /* 1,2,4,8,16,32,64,128 */
    uint16_t reserved_sectors;      /* Usually 32 for FAT32 */
    uint8_t  num_fats;              /* Almost always 2 */
    uint16_t root_entry_count;      /* Must be 0 for FAT32 */
    uint16_t total_sectors_16;      /* Must be 0 for FAT32 */
    uint8_t  media_type;            /* 0xF8 = hard disk */
    uint16_t sectors_per_fat_16;    /* Must be 0 for FAT32 */
    uint16_t sectors_per_track;     /* Geometry */
    uint16_t num_heads;             /* Geometry */
    uint32_t hidden_sectors;        /* Sectors before partition */
    uint32_t total_sectors_32;      /* Total sectors (if 32-bit) */
} __attribute__((packed)) fat32_bpb_t;

/* ========================================================================
 * FAT32 EXTENDED BPB (follows BPB in sector 0)
 * ======================================================================== */

typedef struct {
    uint32_t sectors_per_fat;       /* FAT size in sectors */
    uint16_t ext_flags;             /* Mirror flag, active FAT */
    uint16_t fs_version;            /* Usually 0 */
    uint32_t root_cluster;          /* Root directory first cluster */
    uint16_t fs_info_sector;        /* FSInfo sector (usually 1) */
    uint16_t backup_boot_sector;    /* Backup boot sector (usually 6) */
    uint8_t  reserved[12];          /* Reserved */
    uint8_t  drive_number;          /* 0x80 = hard disk */
    uint8_t  reserved1;             /* Reserved */
    uint8_t  boot_sig;              /* Extended boot signature 0x29 */
    uint32_t volume_serial;         /* Volume serial number */
    char     volume_label[11];      /* Volume label */
    char     fs_type[8];            /* "FAT32   " */
    uint8_t  boot_code[420];        /* Boot code */
    uint16_t signature;             /* 0xAA55 */
} __attribute__((packed)) fat32_ebpb_t;

/* ========================================================================
 * COMBINED BOOT SECTOR (BPB + Extended BPB)
 * ======================================================================== */

typedef struct {
    fat32_bpb_t  bpb;
    fat32_ebpb_t ebpb;
} __attribute__((packed)) fat32_boot_sector_t;

/* ========================================================================
 * FSINFO SECTOR (usually sector 1 of partition)
 * ======================================================================== */

typedef struct {
    uint32_t lead_signature;        /* 0x41615252 */
    uint8_t  reserved1[480];        /* Reserved */
    uint32_t struct_signature;      /* 0x61417272 */
    uint32_t free_cluster_count;    /* Free clusters (0xFFFFFFFF if unknown) */
    uint32_t next_free_cluster;     /* Hint for next free cluster */
    uint8_t  reserved2[12];         /* Reserved */
    uint32_t trail_signature;       /* 0xAA550000 */
} __attribute__((packed)) fat32_fsinfo_t;

/* ========================================================================
 * FAT32 SHORT (8.3) DIRECTORY ENTRY
 * ======================================================================== */

typedef struct {
    uint8_t  name[8];               /* Short name (space-padded) */
    uint8_t  ext[3];                /* Extension (space-padded) */
    uint8_t  attr;                  /* Attributes */
    uint8_t  nt_reserved;           /* NT case flags */
    uint8_t  create_time_tenth;     /* Create time, tenths of second */
    uint16_t create_time;           /* Create time */
    uint16_t create_date;           /* Create date */
    uint16_t access_date;           /* Last access date */
    uint16_t cluster_hi;            /* High 16 bits of first cluster */
    uint16_t write_time;            /* Last write time */
    uint16_t write_date;            /* Last write date */
    uint16_t cluster_lo;            /* Low 16 bits of first cluster */
    uint32_t file_size;             /* File size in bytes */
} __attribute__((packed)) fat32_dirent_t;

/* ========================================================================
 * FAT32 LONG FILE NAME (LFN) DIRECTORY ENTRY
 * ======================================================================== */

typedef struct {
    uint8_t  seq;                   /* Sequence number (0x01-0x14 | 0x40) */
    uint16_t name1[5];              /* Characters 1-5 */
    uint8_t  attr;                  /* Must be ATTR_LFN (0x0F) */
    uint8_t  type;                  /* 0 for LFN entry */
    uint8_t  checksum;              /* Short name checksum */
    uint16_t name2[6];              /* Characters 6-11 */
    uint16_t first_cluster;         /* Must be 0 */
    uint16_t name3[2];              /* Characters 12-13 */
} __attribute__((packed)) fat32_lfn_entry_t;

/* ========================================================================
 * FILE STAT INFORMATION
 * ======================================================================== */

typedef struct {
    char     name[FAT32_LFN_MAX_NAME_LEN + 1];  /* Full file name */
    uint32_t size;                                /* File size in bytes */
    uint8_t  attr;                                /* File attributes */
    uint16_t create_date;                          /* Creation date */
    uint16_t create_time;                          /* Creation time */
    uint16_t write_date;                           /* Last write date */
    uint16_t write_time;                           /* Last write time */
    uint16_t access_date;                          /* Last access date */
    uint32_t first_cluster;                        /* First data cluster */
} fat32_stat_t;

/* ========================================================================
 * DIRECTORY ENTRY FOR READDIR OUTPUT
 * ======================================================================== */

typedef struct {
    char     name[FAT32_LFN_MAX_NAME_LEN + 1];  /* File/directory name */
    uint32_t size;                                /* File size (0 for dirs) */
    uint8_t  attr;                                /* Attributes */
    uint32_t first_cluster;                        /* First cluster */
} fat32_dirent_out_t;

/* ========================================================================
 * FILE HANDLE (internal)
 * ======================================================================== */

typedef struct {
    int       in_use;               /* 1 = handle is active */
    uint32_t  first_cluster;        /* First cluster of file */
    uint32_t  current_cluster;      /* Current position cluster */
    uint32_t  current_offset;       /* Byte offset within current cluster */
    uint32_t  file_size;            /* File size in bytes */
    uint32_t  position;             /* Current seek position */
    int       flags;                /* Open flags */
    uint8_t   attr;                 /* File attributes */
    uint32_t  dir_cluster;          /* Cluster of directory containing this entry */
    uint32_t  dir_entry_offset;     /* Offset within dir cluster for this entry */
} fat32_file_handle_t;

/* ========================================================================
 * FAT CACHE ENTRY
 * ======================================================================== */

typedef struct {
    uint32_t sector;                /* Sector number, 0xFFFFFFFF = invalid */
    uint8_t  data[FAT32_SECTOR_SIZE]; /* Cached sector data */
    int      dirty;                 /* 1 = needs write-back */
    int      lru;                   /* LRU counter */
} fat32_fat_cache_entry_t;

/* ========================================================================
 * DRIVER STATE (internal)
 * ======================================================================== */

typedef struct {
    int           initialized;      /* 1 = driver initialized */
    uint32_t      partition_start;  /* LBA of partition start */
    uint32_t      sectors_per_fat;  /* Sectors per FAT table */
    uint32_t      root_cluster;     /* Root directory cluster */
    uint32_t      data_start;       /* First sector of data area */
    uint32_t      sectors_per_cluster;  /* Sectors per cluster */
    uint32_t      bytes_per_cluster;    /* Bytes per cluster */
    uint32_t      total_clusters;       /* Total data clusters */
    uint32_t      total_sectors;        /* Total sectors in partition */
    uint32_t      fat_start;            /* First sector of FAT */
    uint32_t      fs_info_sector;       /* FSInfo sector (relative) */
    uint32_t      free_cluster_count;   /* Free clusters (from FSInfo) */
    uint32_t      next_free_cluster;    /* Next free cluster hint */
    uint8_t       num_fats;             /* Number of FAT copies */
    uint16_t      ext_flags;            /* FAT flags (mirroring) */
    char          volume_label[12];     /* Volume label */
    uint32_t      volume_serial;        /* Volume serial number */

    /* FAT sector cache */
    fat32_fat_cache_entry_t fat_cache[FAT32_FAT_CACHE_SECTORS];
    int                      cache_counter;  /* LRU counter */

    /* File handles */
    fat32_file_handle_t handles[FAT32_MAX_OPEN_FILES];
} fat32_state_t;

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

/**
 * Initialize the FAT32 driver for a partition.
 * Reads and validates the BPB, sets up internal state.
 *
 * @param partition_start_lba  LBA of the first sector of the partition
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_init(uint32_t partition_start_lba);

/**
 * Open a file by path.
 *
 * @param path   Absolute path (e.g. "/dir/file.txt")
 * @param flags  Open flags (FAT32_O_RDONLY, FAT32_O_WRONLY, etc.)
 * @return Non-negative file handle on success, negative error code on failure
 */
int fat32_open(const char* path, int flags);

/**
 * Close an open file handle.
 *
 * @param handle  File handle returned by fat32_open
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_close(int handle);

/**
 * Read from an open file.
 *
 * @param handle  File handle
 * @param buffer  Destination buffer
 * @param count   Number of bytes to read
 * @return Number of bytes read, or negative error code
 */
int32_t fat32_read(int handle, void* buffer, uint32_t count);

/**
 * Write to an open file.
 *
 * @param handle  File handle
 * @param buffer  Source buffer
 * @param count   Number of bytes to write
 * @return Number of bytes written, or negative error code
 */
int32_t fat32_write(int handle, const void* buffer, uint32_t count);

/**
 * Seek to a position in an open file.
 *
 * @param handle  File handle
 * @param offset  Byte offset
 * @param whence  FAT32_SEEK_SET, FAT32_SEEK_CUR, or FAT32_SEEK_END
 * @return New position on success, negative error code on failure
 */
int32_t fat32_seek(int handle, int32_t offset, int whence);

/**
 * Get file information by path.
 *
 * @param path  Absolute path
 * @param stat  Output stat structure
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_stat(const char* path, fat32_stat_t* stat);

/**
 * Create a directory.
 *
 * @param path  Absolute path of new directory
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_mkdir(const char* path);

/**
 * Delete a file.
 *
 * @param path  Absolute path of file to delete
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_unlink(const char* path);

/**
 * Rename a file or directory.
 *
 * @param oldpath  Current path
 * @param newpath  New path
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_rename(const char* oldpath, const char* newpath);

/**
 * List directory contents.
 *
 * @param path      Absolute path of directory
 * @param entries   Output array of directory entries
 * @param max       Maximum number of entries to return
 * @return Number of entries read, or negative error code
 */
int fat32_readdir(const char* path, fat32_dirent_out_t* entries, uint32_t max);

/**
 * Sync all pending writes to disk.
 *
 * @return FAT32_OK on success, negative error code on failure
 */
int fat32_sync(void);

/**
 * Get the number of free clusters on the volume.
 *
 * @return Number of free clusters, or negative error code
 */
int32_t fat32_free_clusters(void);

/**
 * Check if the FAT32 driver is initialized.
 *
 * @return 1 if initialized, 0 otherwise
 */
int fat32_is_initialized(void);

#endif /* FAT32_H */
