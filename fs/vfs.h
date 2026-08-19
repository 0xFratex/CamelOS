/**
 * vfs.h - Virtual Filesystem Layer for CamelOS
 *
 * Provides a filesystem abstraction layer that allows multiple filesystem
 * types (PFS32, FAT32, devfs, procfs, tmpfs) to coexist under a unified
 * namespace with mount points.
 *
 * Design inspired by Linux VFS and macOS VFS, simplified for a hobby OS.
 */

#ifndef VFS_H
#define VFS_H

#include "../include/types.h"

/* ========================================================================
 * Filesystem Type Identifiers
 * ======================================================================== */

typedef enum {
    VFS_FS_PFS32 = 0,   /* CamelOS native filesystem */
    VFS_FS_FAT32,       /* FAT32 (interoperability) */
    VFS_FS_DEVFS,       /* Device filesystem (/dev) */
    VFS_FS_PROCFS,      /* Process filesystem (/proc) */
    VFS_FS_TMPFS,       /* Temporary filesystem */
    VFS_FS_MAX
} vfs_filesystem_type_t;

/* ========================================================================
 * File Types
 * ======================================================================== */

#define VFS_TYPE_REGULAR    0x01
#define VFS_TYPE_DIRECTORY  0x02
#define VFS_TYPE_SYMLINK    0x04
#define VFS_TYPE_DEVICE     0x08
#define VFS_TYPE_PIPE       0x10
#define VFS_TYPE_UNKNOWN    0x20

/* ========================================================================
 * Open Flags
 * ======================================================================== */

#define VFS_O_RDONLY    0x0001
#define VFS_O_WRONLY    0x0002
#define VFS_O_RDWR      0x0004
#define VFS_O_CREAT     0x0100
#define VFS_O_APPEND    0x0200
#define VFS_O_TRUNC     0x0400
#define VFS_O_EXCL      0x0800

/* ========================================================================
 * Seek Whence
 * ======================================================================== */

#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

/* ========================================================================
 * VFS Limits
 * ======================================================================== */

#define VFS_MAX_MOUNTS      16
#define VFS_MAX_OPEN_FILES  64
#define VFS_MAX_NAME        256
#define VFS_MAX_PATH        1024

/* ========================================================================
 * VFS Directory Entry (for readdir)
 * ======================================================================== */

typedef struct {
    char     name[VFS_MAX_NAME];   /* File/directory name */
    uint32_t size;                  /* File size in bytes */
    uint8_t  type;                  /* VFS_TYPE_* flags */
    uint8_t  permissions;           /* rwxrwxrwx bits */
    uint8_t  uid;                   /* Owner UID */
    uint8_t  gid;                   /* Group GID */
    uint32_t modify_time;           /* Modification time */
} vfs_dirent_t;

/* ========================================================================
 * VFS Stat Structure
 * ======================================================================== */

typedef struct {
    char     name[VFS_MAX_NAME];
    uint32_t size;
    uint8_t  type;                  /* VFS_TYPE_* */
    uint8_t  permissions;
    uint8_t  uid;
    uint8_t  gid;
    uint32_t create_time;
    uint32_t modify_time;
    uint32_t access_time;
    uint32_t blocks;                /* Number of 512-byte blocks */
} vfs_stat_t;

/* ========================================================================
 * VFS File Handle
 * ======================================================================== */

typedef struct {
    int          in_use;            /* 1 if this handle is active */
    int          fd;                /* File descriptor number */
    vfs_filesystem_type_t fs_type;  /* Which filesystem owns this file */
    uint32_t     pos;               /* Current read/write position */
    uint32_t     size;              /* File size */
    int          flags;             /* VFS_O_* flags */
    int          fs_handle;         /* Underlying filesystem handle */
    int          mount_index;       /* Which mount point this belongs to */
} vfs_file_t;

/* ========================================================================
 * VFS Mount Point
 * ======================================================================== */

typedef struct {
    int          in_use;            /* 1 if this mount slot is active */
    char         path[VFS_MAX_PATH]; /* Mount point path (e.g. "/", "/mnt/usb") */
    vfs_filesystem_type_t fs_type;  /* Filesystem type */
    void*        fs_data;           /* Filesystem-specific private data */
    int          mount_flags;       /* Mount flags (read-only, etc.) */
} vfs_mount_t;

/* ========================================================================
 * Filesystem Operations Table
 * ======================================================================== */

typedef struct vfs_ops {
    /* File operations */
    int  (*open)(const char* path, int flags);                  /* Returns fs-specific handle */
    int  (*close)(int fs_handle);
    int  (*read)(int fs_handle, void* buf, uint32_t count);
    int  (*write)(int fs_handle, const void* buf, uint32_t count);
    int  (*seek)(int fs_handle, uint32_t offset, int whence);

    /* Metadata */
    int  (*stat)(const char* path, vfs_stat_t* stat);
    int  (*readdir)(const char* path, vfs_dirent_t* entries, uint32_t max);

    /* Directory operations */
    int  (*mkdir)(const char* path);
    int  (*unlink)(const char* path);
    int  (*rename)(const char* oldpath, const char* newpath);

    /* Sync */
    int  (*sync)(int fs_handle);
} vfs_ops_t;

/* ========================================================================
 * Public API
 * ======================================================================== */

/* Initialize the VFS subsystem */
void vfs_init(void);

/* Register a filesystem driver */
int vfs_register_filesystem(vfs_filesystem_type_t type, vfs_ops_t* ops);

/* Mount a filesystem at a path */
int vfs_mount(const char* path, vfs_filesystem_type_t type, void* fs_data);

/* Unmount a filesystem */
int vfs_umount(const char* path);

/* File operations */
int   vfs_open(const char* path, int flags);
int   vfs_close(int fd);
int   vfs_read(int fd, void* buf, uint32_t count);
int   vfs_write(int fd, const void* buf, uint32_t count);
int   vfs_seek(int fd, uint32_t offset, int whence);
int   vfs_stat(const char* path, vfs_stat_t* stat);
int   vfs_readdir(const char* path, vfs_dirent_t* entries, uint32_t max);

/* Directory operations */
int   vfs_mkdir(const char* path);
int   vfs_unlink(const char* path);
int   vfs_rename(const char* oldpath, const char* newpath);

/* Utility: check if a path exists */
int   vfs_exists(const char* path);

/* Utility: get filesystem type for a path */
vfs_filesystem_type_t vfs_get_fs_type(const char* path);

/* Utility: get mount info */
vfs_mount_t* vfs_get_mount_info(int index);

/* Sync a file's in-memory state with disk */
int   vfs_fsync(int fd);

/* Debug: dump mount table */
void vfs_dump_mounts(void);

#endif /* VFS_H */
