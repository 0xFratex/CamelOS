// installer/syscall_stub.c - Minimal syscall stubs for the installer
// The installer doesn't use syscalls directly, but some objects still
// reference a few symbols during link-time.

#include "../include/types.h"

// Stub for tss_esp0_for_sysenter (referenced by scheduler.c)
uint32_t tss_esp0_for_sysenter = 0;
