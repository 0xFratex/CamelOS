/**
 * sys_dirs.h - System Directory Initializer for CamelOS
 *
 * Provides functions to initialize and manage the FHS-like directory
 * layout used by CamelOS at boot time. Ensures all required system
 * directories exist, handles nested creation, and allows lookup
 * of well-known directory paths by name.
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#ifndef SYS_DIRS_H
#define SYS_DIRS_H

/**
 * sys_dirs_init - Initialize all system directories.
 *
 * Creates any directories in the standard layout that do not already
 * exist.  Safe to call multiple times (idempotent).  Should be called
 * once during kernel boot after the filesystem is mounted.
 */
void sys_dirs_init(void);

/**
 * sys_dirs_ensure - Ensure a single directory exists.
 *
 * @path: Absolute path of the directory to guarantee.
 *
 * If the directory already exists this is a no-op.  If it does not
 * exist, all missing parent directories are created first, then the
 * target directory is created.
 *
 * Returns: 0 on success (created or already exists), -1 on failure.
 */
int sys_dirs_ensure(const char* path);

/**
 * sys_dirs_get_path - Look up a well-known directory path by name.
 *
 * @name: Logical name such as "applications", "library", "usr_bin",
 *        "etc", "tmp", etc.
 *
 * Returns: Pointer to a static string containing the absolute path,
 *          or NULL if the name is not recognized.
 */
const char* sys_dirs_get_path(const char* name);

/**
 * sys_dirs_is_initialized - Check whether the basic directory structure
 *                           is present.
 *
 * Verifies that a handful of key directories (/Applications, /System,
 * /usr, /etc, /tmp) exist.
 *
 * Returns: 1 if properly initialized, 0 if not.
 */
int sys_dirs_is_initialized(void);

#endif /* SYS_DIRS_H */
