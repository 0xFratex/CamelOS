/**
 * fat32_vfs.c - FAT32 VFS Integration Layer for CamelOS
 *
 * Provides a vfs_ops_t wrapper that maps the VFS abstraction layer
 * operations to the FAT32 driver functions. This allows FAT32 partitions
 * to be mounted and accessed through the unified VFS namespace.
 *
 * Pattern follows the PFS32 VFS wrapper in vfs.c.
 */

#include "vfs.h"
#include "fat32.h"
#include "../core/memory.h"
#include "../include/string.h"
#include "../hal/drivers/serial.h"

/* ========================================================================
 * VFS <-> FAT32 Flag Conversion Helpers
 * ======================================================================== */

/**
 * Convert VFS open flags to FAT32 open flags.
 */
static int fat32_vfs_convert_flags(int vfs_flags)
{
    int fat32_flags = 0;

    if (vfs_flags & VFS_O_WRONLY) fat32_flags |= FAT32_O_WRONLY;
    if (vfs_flags & VFS_O_RDWR)   fat32_flags |= FAT32_O_RDWR;
    if (vfs_flags & VFS_O_CREAT)  fat32_flags |= FAT32_O_CREAT;
    if (vfs_flags & VFS_O_APPEND) fat32_flags |= FAT32_O_APPEND;
    if (vfs_flags & VFS_O_TRUNC)  fat32_flags |= FAT32_O_TRUNC;
    if (vfs_flags & VFS_O_EXCL)   fat32_flags |= FAT32_O_EXCL;

    /* Default to read-only if no write flags set */
    if (!(fat32_flags & (FAT32_O_WRONLY | FAT32_O_RDWR))) {
        fat32_flags |= FAT32_O_RDONLY;
    }

    return fat32_flags;
}

/**
 * Convert FAT32 file attributes to a VFS type flag.
 */
static uint8_t fat32_attr_to_vfs_type(uint8_t attr)
{
    if (attr & FAT32_ATTR_DIRECTORY)
        return VFS_TYPE_DIRECTORY;
    if (attr & FAT32_ATTR_DEVICE)
        return VFS_TYPE_DEVICE;
    return VFS_TYPE_REGULAR;
}

/**
 * Convert FAT32 date/time fields to a flat 32-bit timestamp.
 *
 * FAT date: bits 15-9 = year-1980, bits 8-5 = month, bits 4-0 = day
 * FAT time: bits 15-11 = hours, bits 10-5 = minutes, bits 4-0 = seconds/2
 *
 * We pack these into a single uint32_t for VFS consumption:
 *   bits 31-16 = date, bits 15-0 = time
 */
static uint32_t fat32_datetime_to_timestamp(uint16_t date, uint16_t time)
{
    return ((uint32_t)date << 16) | (uint32_t)time;
}

/**
 * Convert FAT32 permissions (attributes) to VFS permissions byte.
 *
 * FAT32 doesn't have Unix-style permissions. We map:
 *   - Read-only attribute -> no write bits
 *   - Otherwise -> full rwxrwxrwx (0777)
 */
static uint8_t fat32_attr_to_permissions(uint8_t attr)
{
    if (attr & FAT32_ATTR_READ_ONLY)
        return 0x55;   /* r-xr-xr-x: read+execute for all, no write */
    return 0x77;       /* rwxrwxrwx: full access for all */
}

/* ========================================================================
 * VFS Operation Wrappers
 * ======================================================================== */

static int fat32_vfs_open(const char* path, int flags)
{
    int fat32_flags = fat32_vfs_convert_flags(flags);
    return fat32_open(path, fat32_flags);
}

static int fat32_vfs_close(int fs_handle)
{
    int ret = fat32_close(fs_handle);
    return (ret == FAT32_OK) ? 0 : -1;
}

static int fat32_vfs_read(int fs_handle, void* buf, uint32_t count)
{
    int32_t bytes = fat32_read(fs_handle, buf, count);
    return (int)bytes;
}

static int fat32_vfs_write(int fs_handle, const void* buf, uint32_t count)
{
    int32_t bytes = fat32_write(fs_handle, buf, count);
    return (int)bytes;
}

static int fat32_vfs_seek(int fs_handle, uint32_t offset, int whence)
{
    /* FAT32 seek uses int32_t offset; VFS uses uint32_t.
     * We cast carefully - for SEEK_SET/SEEK_CUR large offsets
     * may lose the sign bit, but FAT32 volumes are typically
     * under 2GB per file anyway. */
    int32_t result = fat32_seek(fs_handle, (int32_t)offset, whence);
    return (int)result;
}

static int fat32_vfs_stat(const char* path, vfs_stat_t* stat)
{
    fat32_stat_t fs;
    int ret = fat32_stat(path, &fs);
    if (ret != FAT32_OK) return -1;

    /* Clear the output structure */
    memset(stat, 0, sizeof(vfs_stat_t));

    /* Copy name */
    strncpy(stat->name, fs.name, VFS_MAX_NAME - 1);
    stat->name[VFS_MAX_NAME - 1] = '\0';

    /* Map fields */
    stat->size         = fs.size;
    stat->type         = fat32_attr_to_vfs_type(fs.attr);
    stat->permissions  = fat32_attr_to_permissions(fs.attr);
    stat->uid          = 0;    /* FAT32 has no UID concept */
    stat->gid          = 0;    /* FAT32 has no GID concept */
    stat->create_time  = fat32_datetime_to_timestamp(fs.create_date,
                                                      fs.create_time);
    stat->modify_time  = fat32_datetime_to_timestamp(fs.write_date,
                                                      fs.write_time);
    stat->access_time  = fat32_datetime_to_timestamp(fs.access_date, 0);
    stat->blocks       = (fs.size + 511) / 512;

    return 0;
}

static int fat32_vfs_readdir(const char* path, vfs_dirent_t* entries, uint32_t max)
{
    /* Allocate a temporary buffer for FAT32 directory entries */
    fat32_dirent_out_t* fat32_entries;
    fat32_entries = (fat32_dirent_out_t*)kmalloc(max * sizeof(fat32_dirent_out_t));
    if (!fat32_entries) return -1;

    int count = fat32_readdir(path, fat32_entries, max);
    if (count < 0) {
        kfree(fat32_entries);
        return -1;
    }

    /* Convert FAT32 entries to VFS entries */
    for (int i = 0; i < count && i < (int)max; i++) {
        strncpy(entries[i].name, fat32_entries[i].name, VFS_MAX_NAME - 1);
        entries[i].name[VFS_MAX_NAME - 1] = '\0';
        entries[i].size        = fat32_entries[i].size;
        entries[i].type        = fat32_attr_to_vfs_type(fat32_entries[i].attr);
        entries[i].permissions = fat32_attr_to_permissions(fat32_entries[i].attr);
        entries[i].uid         = 0;
        entries[i].gid         = 0;
        entries[i].modify_time = 0;   /* readdir doesn't return timestamps in FAT32 */
    }

    kfree(fat32_entries);
    return count;
}

static int fat32_vfs_mkdir(const char* path)
{
    int ret = fat32_mkdir(path);
    return (ret == FAT32_OK) ? 0 : -1;
}

static int fat32_vfs_unlink(const char* path)
{
    int ret = fat32_unlink(path);
    return (ret == FAT32_OK) ? 0 : -1;
}

static int fat32_vfs_rename(const char* oldpath, const char* newpath)
{
    int ret = fat32_rename(oldpath, newpath);
    return (ret == FAT32_OK) ? 0 : -1;
}

/* ========================================================================
 * FAT32 VFS Operations Table
 * ======================================================================== */

static vfs_ops_t fat32_vfs_ops = {
    .open    = fat32_vfs_open,
    .close   = fat32_vfs_close,
    .read    = fat32_vfs_read,
    .write   = fat32_vfs_write,
    .seek    = fat32_vfs_seek,
    .stat    = fat32_vfs_stat,
    .readdir = fat32_vfs_readdir,
    .mkdir   = fat32_vfs_mkdir,
    .unlink  = fat32_vfs_unlink,
    .rename  = fat32_vfs_rename,
};

/* ========================================================================
 * Public Registration Function
 * ======================================================================== */

/**
 * Register the FAT32 filesystem driver with the VFS layer.
 *
 * After calling this function, FAT32 can be used as a mount target:
 *   vfs_mount("/mnt/usb", VFS_FS_FAT32, NULL);
 *
 * The FAT32 driver must be initialized separately (via fat32_init())
 * before attempting to mount or access a FAT32 partition.
 *
 * @return 0 on success, -1 on failure
 */
int fat32_register_with_vfs(void)
{
    s_printf("[FAT32] Registering FAT32 with VFS...\n");

    int ret = vfs_register_filesystem(VFS_FS_FAT32, &fat32_vfs_ops);
    if (ret != 0) {
        s_printf("[FAT32] Failed to register with VFS\n");
        return -1;
    }

    s_printf("[FAT32] Successfully registered with VFS\n");
    return 0;
}
