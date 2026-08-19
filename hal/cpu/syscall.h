// hal/cpu/syscall.h - Kernel-internal syscall dispatch for CamelOS
//
// The user-facing ABI (syscall numbers + syscallN() helpers) now lives in
// sys/syscalls.h, which is the canonical, frozen ABI shared by Ring 0 and
// Ring 3. This header keeps only the kernel-internal pieces: the register
// frame layout and the init/dispatch entry points.

#ifndef SYSCALL_H
#define SYSCALL_H

#include "../../include/types.h"
#include "../../sys/syscalls.h"

// ============================================================================
// Syscall Register State (pushed by assembly stub)
// ============================================================================
typedef struct {
    // Pushed by our stub (gs ends up at the lowest address = frame base)
    uint32_t gs, fs, es, ds;

    // Pushed by pusha (edi pushed last = just below gs)
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;    // Kernel ESP at time of pusha
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;    // Syscall number on entry, return value on exit
} syscall_regs_t;

// ============================================================================
// Kernel-internal API
// ============================================================================

void init_syscall(void);

// Initialize fast syscall (sysenter/sysexit) via MSRs
void syscall_init_fast(void);

// Global kernel stack for sysenter (updated by scheduler)
extern uint32_t tss_esp0_for_sysenter;

// C handler called from assembly
void syscall_handler(syscall_regs_t* regs);

#endif // SYSCALL_H
