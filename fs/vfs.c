/**
 * vfs.c - Virtual Filesystem Layer for CamelOS
 *
 * Implements the VFS abstraction that allows multiple filesystem types
 * to coexist under a unified namespace with mount points.
 *
 * The root filesystem "/" is mounted as PFS32 at boot.
 * Additional filesystems can be mounted at any directory.
 */

#include "vfs.h"
#include "pfs32.h"
#include "../core/memory.h"
#include "../core/string.h"
#include "../hal/drivers/serial.h"

/* ========================================================================
 * Global State
 * ======================================================================== */

/* Mount table */
static vfs_mount_t mounts[VFS_MAX_MOUNTS];

/* Open file table */
static vfs_file_t open_files[VFS_MAX_OPEN_FILES];

/* Registered filesystem drivers (one per type) */
static vfs_ops_t* fs_ops[VFS_FS_MAX];

/* Next file descriptor to allocate */
static int next_fd = 3;  /* 0=stdin, 1=stdout, 2=stderr reserved */

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/**
 * Find which mount point a given path belongs to.
 * Returns the mount table index, or -1 if not found.
 *
 * For example, with mounts at "/" and "/mnt/usb":
 *   "/Users/test.txt"  -> mount at "/"
 *   "/mnt/usb/file.txt" -> mount at "/mnt/usb"
 *
 * We find the LONGEST matching mount path prefix.
 */
static int find_mount_for_path(const char* path)
{
    int best = -1;
    uint32_t best_len = 0;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;

        uint32_t mlen = strlen(mounts[i].path);

        /* Check if path starts with mount path */
        if (strncmp(path, mounts[i].path, mlen) != 0) continue;

        /* Must match at a path separator boundary:
         * "/" matches everything
         * "/mnt/usb" matches "/mnt/usb/file" but not "/mnt/usbdrive"
         */
        if (mlen > 1 && path[mlen] != '/' && path[mlen] != '\0') continue;

        /* Pick the longest (most specific) mount */
        if (mlen > best_len) {
            best = i;
            best_len = mlen;
        }
    }

    return best;
}

/**
 * Translate an absolute path to a filesystem-relative path.
 * Strips the mount point prefix.
 *
 * e.g. mount at "/mnt/usb", path "/mnt/usb/file.txt" -> "/file.txt"
 * e.g. mount at "/", path "/Users/test.txt" -> "/Users/test.txt"
 */
static const char* path_to_fs_relative(int mount_idx, const char* path)
{
    if (mount_idx < 0) return path;

    uint32_t mlen = strlen(mounts[mount_idx].path);

    /* For root mount "/", return the full path */
    if (mlen == 1 && mounts[mount_idx].path[0] == '/') {
        return path;
    }

    /* Skip the mount prefix */
    const char* relative = path + mlen;

    /* Ensure it starts with '/' */
    if (*relative == '\0') return "/";
    if (*relative == '/') return relative;

    return path;  /* Fallback: return full path */
}

/**
 * Allocate a file descriptor.
 * Returns fd number, or -1 if table is full.
 */
static int alloc_fd(void)
{
    /* Search from next_fd, wrapping around */
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        int fd = (next_fd + i) % VFS_MAX_OPEN_FILES;
        /* Skip reserved fds */
        if (fd < 3) continue;
        if (!open_files[fd].in_use) {
            next_fd = fd + 1;
            if (next_fd >= VFS_MAX_OPEN_FILES) next_fd = 3;
            return fd;
        }
    }
    return -1;
}

/**
 * Look up a file descriptor in the open file table.
 * Returns pointer to the vfs_file_t, or NULL if invalid.
 */
static vfs_file_t* get_file(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES) return 0;
    if (!open_files[fd].in_use) return 0;
    return &open_files[fd];
}

/* ========================================================================
 * PFS32 Filesystem Wrapper
 * ======================================================================== */

static int pfs32_vfs_open(const char* path, int flags)
{
    int pfs_flags = 0;
    if (flags & VFS_O_WRONLY) pfs_flags = 1;  /* write */
    if (flags & VFS_O_RDWR)   pfs_flags = 2;  /* read/write */
    if (flags & VFS_O_CREAT)  pfs_flags |= 4;

    return pfs32_open(path, pfs_flags);
}

static int pfs32_vfs_close(int fs_handle)
{
    pfs32_close(fs_handle);
    return 0;
}

static int pfs32_vfs_read(int fs_handle, void* buf, uint32_t count)
{
    return pfs32_read_handle(fs_handle, buf, count);
}

static int pfs32_vfs_write(int fs_handle, const void* buf, uint32_t count)
{
    return pfs32_write_handle(fs_handle, buf, count);
}

static int pfs32_vfs_seek(int fs_handle, uint32_t offset, int whence)
{
    /* PFS32 only supports SEEK_SET style */
    (void)whence;
    return pfs32_seek(fs_handle, offset);
}

static int pfs32_vfs_stat(const char* path, vfs_stat_t* stat)
{
    pfs32_direntry_t entry;
    int ret = pfs32_stat(path, &entry);
    if (ret != 0) return -1;

    memset(stat, 0, sizeof(vfs_stat_t));
    strncpy(stat->name, entry.filename, VFS_MAX_NAME - 1);
    stat->size         = entry.file_size;
    stat->permissions  = entry.permissions;
    stat->uid          = entry.uid;
    stat->gid          = entry.gid;
    stat->create_time  = entry.create_time;
    stat->modify_time  = entry.modify_time;
    stat->access_time  = entry.access_time;
    stat->blocks       = (entry.file_size + 511) / 512;

    if (entry.attributes & PFS32_ATTR_DIRECTORY)
        stat->type = VFS_TYPE_DIRECTORY;
    else if (entry.attributes & PFS32_ATTR_SYMLINK)
        stat->type = VFS_TYPE_SYMLINK;
    else
        stat->type = VFS_TYPE_REGULAR;

    return 0;
}

static int pfs32_vfs_readdir(const char* path, vfs_dirent_t* entries, uint32_t max)
{
    /* Resolve path to directory block */
    uint32_t dir_block;
    if (get_dir_block(path, &dir_block) != 0) return -1;

    pfs32_direntry_t* pfs_entries = (pfs32_direntry_t*)kmalloc(max * sizeof(pfs32_direntry_t));
    if (!pfs_entries) return -1;

    int count = pfs32_listdir(dir_block, pfs_entries, max);
    if (count < 0) {
        kfree(pfs_entries);
        return -1;
    }

    /* Convert PFS32 entries to VFS entries */
    for (int i = 0; i < count && i < (int)max; i++) {
        strncpy(entries[i].name, pfs_entries[i].filename, VFS_MAX_NAME - 1);
        entries[i].name[VFS_MAX_NAME - 1] = '\0';
        entries[i].size         = pfs_entries[i].file_size;
        entries[i].permissions  = pfs_entries[i].permissions;
        entries[i].uid          = pfs_entries[i].uid;
        entries[i].gid          = pfs_entries[i].gid;
        entries[i].modify_time  = pfs_entries[i].modify_time;

        if (pfs_entries[i].attributes & PFS32_ATTR_DIRECTORY)
            entries[i].type = VFS_TYPE_DIRECTORY;
        else if (pfs_entries[i].attributes & PFS32_ATTR_SYMLINK)
            entries[i].type = VFS_TYPE_SYMLINK;
        else
            entries[i].type = VFS_TYPE_REGULAR;
    }

    kfree(pfs_entries);
    return count;
}

static int pfs32_vfs_mkdir(const char* path)
{
    return pfs32_create_directory(path);
}

static int pfs32_vfs_unlink(const char* path)
{
    return pfs32_delete(path);
}

static int pfs32_vfs_rename(const char* oldpath, const char* newpath)
{
    return pfs32_rename(oldpath, newpath);
}

/* PFS32 operations table */
static vfs_ops_t pfs32_ops = {
    .open    = pfs32_vfs_open,
    .close   = pfs32_vfs_close,
    .read    = pfs32_vfs_read,
    .write   = pfs32_vfs_write,
    .seek    = pfs32_vfs_seek,
    .stat    = pfs32_vfs_stat,
    .readdir = pfs32_vfs_readdir,
    .mkdir   = pfs32_vfs_mkdir,
    .unlink  = pfs32_vfs_unlink,
    .rename  = pfs32_vfs_rename,
};

/* ========================================================================
 * Public API Implementation
 * ======================================================================== */

void vfs_init(void)
{
    s_printf("[VFS] Initializing Virtual Filesystem Layer...\n");

    /* Clear mount table */
    memset(mounts, 0, sizeof(mounts));

    /* Clear open file table */
    memset(open_files, 0, sizeof(open_files));

    /* Clear filesystem ops table */
    memset(fs_ops, 0, sizeof(fs_ops));

    /* Reserve stdin/stdout/stderr (not yet backed by real files) */
    open_files[0].in_use = 1; open_files[0].fd = 0;
    open_files[1].in_use = 1; open_files[1].fd = 1;
    open_files[2].in_use = 1; open_files[2].fd = 2;

    /* Register PFS32 as the first filesystem driver */
    vfs_register_filesystem(VFS_FS_PFS32, &pfs32_ops);

    /* Mount root filesystem as PFS32 */
    vfs_mount("/", VFS_FS_PFS32, 0);

    s_printf("[VFS] Root filesystem mounted as PFS32\n");
}

int vfs_register_filesystem(vfs_filesystem_type_t type, vfs_ops_t* ops)
{
    if (type >= VFS_FS_MAX || !ops) return -1;

    fs_ops[type] = ops;
    s_printf("[VFS] Registered filesystem driver type %d\n", type);
    return 0;
}

int vfs_mount(const char* path, vfs_filesystem_type_t type, void* fs_data)
{
    if (!path || type >= VFS_FS_MAX) return -1;

    /* Find a free mount slot */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        s_printf("[VFS] Mount table full\n");
        return -1;
    }

    /* Check if a filesystem driver is registered for this type */
    if (!fs_ops[type]) {
        s_printf("[VFS] No driver registered for filesystem type %d\n", type);
        return -1;
    }

    /* Check if already mounted at this path */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, path) == 0) {
            s_printf("[VFS] Path already mounted\n");
            return -1;
        }
    }

    /* Set up the mount entry */
    strncpy(mounts[slot].path, path, VFS_MAX_PATH - 1);
    mounts[slot].path[VFS_MAX_PATH - 1] = '\0';
    mounts[slot].fs_type   = type;
    mounts[slot].fs_data   = fs_data;
    mounts[slot].mount_flags = 0;
    mounts[slot].in_use    = 1;

    s_printf("[VFS] Mounted filesystem type %d at %s\n", type, path);
    return 0;
}

int vfs_umount(const char* path)
{
    if (!path) return -1;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].path, path) == 0) {
            /* Check for open files on this mount */
            for (int j = 0; j < VFS_MAX_OPEN_FILES; j++) {
                if (open_files[j].in_use && open_files[j].mount_index == i) {
                    s_printf("[VFS] Cannot unmount: files still open\n");
                    return -1;
                }
            }

            mounts[i].in_use = 0;
            s_printf("[VFS] Unmounted %s\n", path);
            return 0;
        }
    }

    s_printf("[VFS] Mount point not found\n");
    return -1;
}

int vfs_open(const char* path, int flags)
{
    if (!path) return -1;

    /* Find the mount point for this path */
    int mi = find_mount_for_path(path);
    if (mi < 0) {
        s_printf("[VFS] No mount point for path\n");
        return -1;
    }

    vfs_ops_t* ops = fs_ops[mounts[mi].fs_type];
    if (!ops || !ops->open) {
        s_printf("[VFS] No open operation for filesystem\n");
        return -1;
    }

    /* Translate path to filesystem-relative path */
    const char* rel_path = path_to_fs_relative(mi, path);

    /* Call the filesystem-specific open */
    int fs_handle = ops->open(rel_path, flags);
    if (fs_handle < 0) return -1;

    /* Allocate a VFS file descriptor */
    int fd = alloc_fd();
    if (fd < 0) {
        /* No VFS fd available, close the fs handle */
        if (ops->close) ops->close(fs_handle);
        s_printf("[VFS] File descriptor table full\n");
        return -1;
    }

    /* Get file size if possible */
    uint32_t file_size = 0;
    if (ops->stat) {
        vfs_stat_t st;
        if (ops->stat(rel_path, &st) == 0) {
            file_size = st.size;
        }
    }

    /* Fill in the file handle */
    open_files[fd].in_use      = 1;
    open_files[fd].fd          = fd;
    open_files[fd].fs_type     = mounts[mi].fs_type;
    open_files[fd].pos         = (flags & VFS_O_APPEND) ? file_size : 0;
    open_files[fd].size        = file_size;
    open_files[fd].flags       = flags;
    open_files[fd].fs_handle   = fs_handle;
    open_files[fd].mount_index = mi;

    return fd;
}

int vfs_close(int fd)
{
    vfs_file_t* f = get_file(fd);
    if (!f) return -1;

    /* Call filesystem-specific close */
    vfs_ops_t* ops = fs_ops[f->fs_type];
    if (ops && ops->close) {
        ops->close(f->fs_handle);
    }

    /* Free the VFS file descriptor */
    f->in_use = 0;
    f->fd     = 0;
    f->fs_handle = 0;

    return 0;
}

int vfs_read(int fd, void* buf, uint32_t count)
{
    vfs_file_t* f = get_file(fd);
    if (!f || !buf) return -1;

    if (!(f->flags & VFS_O_RDONLY) && !(f->flags & VFS_O_RDWR)) return -1;

    vfs_ops_t* ops = fs_ops[f->fs_type];
    if (!ops || !ops->read) return -1;

    int bytes = ops->read(f->fs_handle, buf, count);
    if (bytes > 0) {
        f->pos += bytes;
    }
    return bytes;
}

int vfs_write(int fd, const void* buf, uint32_t count)
{
    vfs_file_t* f = get_file(fd);
    if (!f || !buf) return -1;

    if (!(f->flags & VFS_O_WRONLY) && !(f->flags & VFS_O_RDWR)) return -1;

    vfs_ops_t* ops = fs_ops[f->fs_type];
    if (!ops || !ops->write) return -1;

    /* Handle append mode */
    if (f->flags & VFS_O_APPEND) {
        if (ops->seek) {
            ops->seek(f->fs_handle, f->size, VFS_SEEK_END);
        }
        f->pos = f->size;
    }

    int bytes = ops->write(f->fs_handle, buf, count);
    if (bytes > 0) {
        f->pos += bytes;
        if (f->pos > f->size) f->size = f->pos;
    }
    return bytes;
}

int vfs_seek(int fd, uint32_t offset, int whence)
{
    vfs_file_t* f = get_file(fd);
    if (!f) return -1;

    uint32_t new_pos;
    switch (whence) {
        case VFS_SEEK_SET:
            new_pos = offset;
            break;
        case VFS_SEEK_CUR:
            new_pos = f->pos + offset;
            break;
        case VFS_SEEK_END:
            new_pos = f->size + offset;
            break;
        default:
            return -1;
    }

    /* Call filesystem-specific seek */
    vfs_ops_t* ops = fs_ops[f->fs_type];
    if (ops && ops->seek) {
        int ret = ops->seek(f->fs_handle, new_pos, whence);
        if (ret < 0) return -1;
    }

    f->pos = new_pos;
    return (int)new_pos;
}

int vfs_stat(const char* path, vfs_stat_t* stat)
{
    if (!path || !stat) return -1;

    int mi = find_mount_for_path(path);
    if (mi < 0) return -1;

    vfs_ops_t* ops = fs_ops[mounts[mi].fs_type];
    if (!ops || !ops->stat) return -1;

    const char* rel_path = path_to_fs_relative(mi, path);
    return ops->stat(rel_path, stat);
}

int vfs_readdir(const char* path, vfs_dirent_t* entries, uint32_t max)
{
    if (!path || !entries) return -1;

    int mi = find_mount_for_path(path);
    if (mi < 0) return -1;

    vfs_ops_t* ops = fs_ops[mounts[mi].fs_type];
    if (!ops || !ops->readdir) return -1;

    const char* rel_path = path_to_fs_relative(mi, path);
    return ops->readdir(rel_path, entries, max);
}

int vfs_mkdir(const char* path)
{
    if (!path) return -1;

    int mi = find_mount_for_path(path);
    if (mi < 0) return -1;

    vfs_ops_t* ops = fs_ops[mounts[mi].fs_type];
    if (!ops || !ops->mkdir) return -1;

    const char* rel_path = path_to_fs_relative(mi, path);
    return ops->mkdir(rel_path);
}

int vfs_unlink(const char* path)
{
    if (!path) return -1;

    int mi = find_mount_for_path(path);
    if (mi < 0) return -1;

    vfs_ops_t* ops = fs_ops[mounts[mi].fs_type];
    if (!ops || !ops->unlink) return -1;

    const char* rel_path = path_to_fs_relative(mi, path);
    return ops->unlink(rel_path);
}

int vfs_rename(const char* oldpath, const char* newpath)
{
    if (!oldpath || !newpath) return -1;

    /* For now, both paths must be on the same mount */
    int mi_old = find_mount_for_path(oldpath);
    int mi_new = find_mount_for_path(newpath);

    if (mi_old < 0 || mi_new < 0) return -1;
    if (mi_old != mi_new) {
        s_printf("[VFS] Cross-filesystem rename not supported\n");
        return -1;
    }

    vfs_ops_t* ops = fs_ops[mounts[mi_old].fs_type];
    if (!ops || !ops->rename) return -1;

    const char* rel_old = path_to_fs_relative(mi_old, oldpath);
    const char* rel_new = path_to_fs_relative(mi_new, newpath);
    return ops->rename(rel_old, rel_new);
}

int vfs_exists(const char* path)
{
    vfs_stat_t st;
    return (vfs_stat(path, &st) == 0) ? 1 : 0;
}

vfs_filesystem_type_t vfs_get_fs_type(const char* path)
{
    int mi = find_mount_for_path(path);
    if (mi < 0) return VFS_FS_MAX;  /* Unknown */
    return mounts[mi].fs_type;
}

vfs_mount_t* vfs_get_mount_info(int index)
{
    if (index < 0 || index >= VFS_MAX_MOUNTS) return 0;
    if (!mounts[index].in_use) return 0;
    return &mounts[index];
}

void vfs_dump_mounts(void)
{
    s_printf("[VFS] Mount Table:\n");
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use) {
            s_printf("  [");
            char buf[8];
            int_to_str(i, buf);
            s_printf(buf);
            s_printf("] ");
            s_printf(mounts[i].path);
            s_printf(" -> type=");
            int_to_str(mounts[i].fs_type, buf);
            s_printf(buf);
            s_printf("\n");
        }
    }
}
