// installer/syscall_stub.c - Minimal syscall stubs for the installer
// The installer doesn't use syscalls but some objects reference these symbols

#include "../include/types.h"

// Stub for tss_esp0_for_sysenter (referenced by scheduler.c)
uint32_t tss_esp0_for_sysenter = 0;

// ── FS stubs so core/theme.c can link against the installer binary ──
// The installer never persists theme preferences (the FS isn't mounted
// in the traditional sense during install), so all of these return
// "not found" / "failure" and theme.c falls back to its built-in defaults.

int sys_fs_create(const char* path, int is_dir) {
    (void)path; (void)is_dir;
    return -1;
}

int sys_fs_write(const char* path, const char* data, int len) {
    (void)path; (void)data; (void)len;
    return -1;
}

int sys_fs_read(const char* path, char* buf, int len) {
    (void)path; (void)buf; (void)len;
    return -1;
}
