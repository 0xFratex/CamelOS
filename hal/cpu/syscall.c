// hal/cpu/syscall.c - Syscall handler implementation for CamelOS
// Dispatches int 0x80 system calls to the appropriate kernel functions

#include "syscall.h"
#include "../../sys/cdl_defs.h"
#include "../../sys/api.h"
#include "../core/memory.h"
#include "../core/string.h"
#include "../drivers/serial.h"
#include "idt.h"
#include "../../core/vmm.h"
#include "../../core/signal.h"
#include "../../core/pipe.h"
#include "../../core/notification.h"
#include "../../core/task.h"
#include "../../core/scheduler.h"
#include "../../core/process.h"
#include "../../core/bsd_syscall.h"
#include "../../core/user_copy.h"

// Extern the kernel API table and wrappers from cdl_loader.c
extern kernel_api_t g_kernel_api;

// ============================================================================
// SYSCALL DISPATCH TABLE
// ============================================================================

void syscall_handler(syscall_regs_t* regs) {
    int syscall_num = (int)regs->eax;
    
    // Arguments from registers
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;
    uint32_t arg4 = regs->esi;
    uint32_t arg5 = regs->edi;
    
    int32_t result = 0;
    
    switch (syscall_num) {
        // --- Process Control ---
        case SYS_EXIT:
            g_kernel_api.exit();
            result = 0;
            break;
            
        case SYS_EXEC:
            result = g_kernel_api.exec((const char*)arg1);
            break;
            
        case SYS_EXEC_ARGS:
            result = g_kernel_api.exec_with_args((const char*)arg1, (const char*)arg2);
            break;
            
        case SYS_GET_ARGS:
            g_kernel_api.get_launch_args((char*)arg1, (int)arg2);
            result = 0;
            break;
            
        // --- I/O ---
        case SYS_PRINT:
            g_kernel_api.print((const char*)arg1);
            result = 0;
            break;
            
        // --- Memory ---
        case SYS_MALLOC:
            result = (int32_t)g_kernel_api.malloc((unsigned long)arg1);
            break;
            
        case SYS_REALLOC:
            result = (int32_t)g_kernel_api.realloc((void*)arg1, (unsigned long)arg2);
            break;
            
        case SYS_FREE:
            g_kernel_api.free((void*)arg1);
            result = 0;
            break;
            
        // --- Filesystem ---
        case SYS_FS_READ:
            result = g_kernel_api.fs_read((const char*)arg1, (char*)arg2, (int)arg3);
            break;
            
        case SYS_FS_WRITE:
            result = g_kernel_api.fs_write((const char*)arg1, (char*)arg2, (int)arg3);
            break;
            
        case SYS_FS_LIST:
            result = g_kernel_api.fs_list((const char*)arg1, (void*)arg2, (int)arg3);
            break;
            
        case SYS_FS_CREATE:
            result = g_kernel_api.fs_create((const char*)arg1, (int)arg2);
            break;
            
        case SYS_FS_DELETE:
            result = g_kernel_api.fs_delete((const char*)arg1);
            break;
            
        case SYS_FS_RENAME:
            result = g_kernel_api.fs_rename((const char*)arg1, (const char*)arg2);
            break;
            
        case SYS_FS_EXISTS:
            result = g_kernel_api.fs_exists((const char*)arg1);
            break;
            
        // --- Graphics ---
        case SYS_CREATE_WIN:
            result = (int32_t)g_kernel_api.create_window(
                (const char*)arg1, (int)arg2, (int)arg3,
                (void*)arg4 /* paint_cb */, 
                (void*)arg5 /* input_cb */,
                (void*)0 /* mouse_cb - would need more args */);
            break;
            
        case SYS_DRAW_RECT:
            g_kernel_api.draw_rect((int)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5);
            result = 0;
            break;
            
        case SYS_DRAW_TEXT:
            g_kernel_api.draw_text((int)arg1, (int)arg2, (const char*)arg3, (int)arg4);
            result = 0;
            break;
            
        case SYS_DRAW_RRECT:
            g_kernel_api.draw_rect_rounded((int)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5, 0 /* radius */);
            result = 0;
            break;
            
        // --- System Info ---
        case SYS_GET_TICKS:
            result = (int32_t)g_kernel_api.get_ticks();
            break;
            
        case SYS_MEM_USED:
            result = (int32_t)g_kernel_api.mem_used();
            break;
            
        case SYS_MEM_TOTAL:
            result = (int32_t)g_kernel_api.mem_total();
            break;
            
        case SYS_KBD_STATE:
            g_kernel_api.get_kbd_state((int*)arg1, (int*)arg2, (int*)arg3);
            result = 0;
            break;
            
        // --- Network ---
        case SYS_SOCKET:
            result = g_kernel_api.socket((int)arg1, (int)arg2, (int)arg3);
            break;
            
        case SYS_BIND:
            result = g_kernel_api.bind((int)arg1, (const void*)arg2, (int)arg3);
            break;
            
        case SYS_CONNECT:
            result = g_kernel_api.connect((int)arg1, (const void*)arg2, (int)arg3);
            break;
            
        case SYS_SEND:
            result = g_kernel_api.send((int)arg1, (const void*)arg2, (unsigned long)arg3, (int)arg4);
            break;
            
        case SYS_RECV:
            result = g_kernel_api.recv((int)arg1, (void*)arg2, (unsigned long)arg3, (int)arg4);
            break;
            
        case SYS_CLOSE:
            result = g_kernel_api.close((int)arg1);
            break;
            
        case SYS_DNS:
            result = g_kernel_api.dns_resolve((const char*)arg1, (char*)arg2, (int)arg3);
            break;
            
        case SYS_HTTP_GET:
            result = g_kernel_api.http_get((const char*)arg1, (char*)arg2, (int)arg3, (const char**)arg4, (int)arg5);
            break;
            
        case SYS_PING:
            result = g_kernel_api.ping((const char*)arg1, (char*)arg2, (int)arg3);
            break;
            
        case SYS_LISTEN:
            result = g_kernel_api.listen((int)arg1, (int)arg2);
            break;
            
        case SYS_ACCEPT:
            result = g_kernel_api.accept((int)arg1, (void*)arg2, (int*)arg3);
            break;
            
        case SYS_PROCESS_EVENTS:
            g_kernel_api.process_events();
            result = 0;
            break;
            
        // --- Process Management ---
        case SYS_GETPID:
            {
                task_t* cur = scheduler_get_current();
                result = cur ? cur->id : -1;
            }
            break;
            
        case SYS_FORK:
            result = process_fork();
            break;
            
        case SYS_WAITPID: {
            // waitpid(pid, status_ptr, options)
            int pid = (int)arg1;
            int* status_ptr = (int*)arg2;
            // int options = (int)arg3;  // WNOHANG etc — not yet used

            // Look up the process entry for the specified child
            process_entry_t* pe = process_find_by_pid(pid);
            if (!pe) {
                // No such process or not our child — ECHILD
                result = -1;
            } else if (pe->is_zombie) {
                // Child is a zombie — reap it
                if (status_ptr) {
                    *status_ptr = pe->exit_code;
                }
                process_reap(pid);
                result = pid;
            } else {
                // Child exists but hasn't exited — no blocking wait yet
                result = -1;
            }
            break;
        }
            
        case SYS_KILL:
            result = signal_send((int)arg1, (int)arg2, SI_USER, 0);
            break;
            
        case SYS_SIGNAL:
            result = (int32_t)signal_set_handler((int)arg1, (signal_handler_t)arg2);
            break;
            
        case SYS_SIGACTION:
            result = signal_sigaction((int)arg1, (const sigaction_t*)arg2, (sigaction_t*)arg3);
            break;
            
        case SYS_SIGPROCMASK:
            result = signal_sigprocmask((int)arg1, (uint32_t*)arg2, (uint32_t*)arg3);
            break;
            
        // --- Virtual Memory ---
        case SYS_MMAP:
            {
                address_space_t* space = current_task ? (address_space_t*)current_task->address_space : 0;
                if (space) {
                    void* addr = vmm_mmap(space, (uint32_t)arg1, (uint32_t)arg2,
                                          (int)arg3, (int)arg4, (uint32_t)arg5);
                    result = (int32_t)addr;
                } else {
                    result = -1;
                }
            }
            break;
            
        case SYS_MUNMAP:
            {
                address_space_t* space = current_task ? (address_space_t*)current_task->address_space : 0;
                if (space) {
                    result = vmm_munmap(space, (uint32_t)arg1, (uint32_t)arg2);
                } else {
                    result = -1;
                }
            }
            break;
            
        case SYS_BRK:
            {
                address_space_t* space = current_task ? (address_space_t*)current_task->address_space : 0;
                if (space) {
                    result = vmm_brk(space, (uint32_t)arg1);
                } else {
                    result = -1;
                }
            }
            break;
            
        case SYS_MPROTECT:
            result = bsd_mprotect((void*)arg1, (uint32_t)arg2, (int)arg3);
            break;
            
        // --- Pipe IPC ---
        case SYS_PIPE:
            result = pipe_create((int*)arg1);
            break;
            
        case SYS_MKFIFO:
            result = pipe_mkfifo((const char*)arg1, (int)arg2);
            break;
            
        case SYS_READ_PIPE:
            result = pipe_read((int)arg1, (void*)arg2, (size_t)arg3);
            break;
            
        case SYS_WRITE_PIPE:
            result = pipe_write((int)arg1, (const void*)arg2, (size_t)arg3);
            break;
            
        case SYS_IOCTL_PIPE:
            result = pipe_ioctl((int)arg1, (int)arg2, (void*)arg3);
            break;
            
        // --- Notification ---
        case SYS_NOTIFY_POST:
            result = notify_post((const char*)arg1, (const char*)arg2, (const char*)arg3,
                                 (notify_priority_t)arg4, (notify_category_t)arg5);
            break;
            
        case SYS_NOTIFY_DISMISS:
            result = notify_dismiss((int)arg1);
            break;
            
        case SYS_NOTIFY_CLICK:
            result = notify_click((int)arg1, (int)arg2);
            break;
            
        case SYS_NOTIFY_DND:
            notify_set_dnd((int)arg1);
            result = 0;
            break;

        // --- User-Mode Syscalls (Task 7 - Ring 3 compatible) ---
        // IMPORTANT: All user-space pointers must be validated before
        // the kernel dereferences them.  A Ring 3 process can set any
        // register value; without validation it could trick the kernel
        // into reading/writing arbitrary kernel memory.
        case SYS_USER_EXIT:
            process_exit((int)arg1);
            result = 0;  /* Does not return */
            break;
            
        case SYS_USER_READ: {
            /* read(fd, buf, count) — kernel writes into user buf */
            if (arg3 > 0 && validate_user_ptr((const void*)arg2, (size_t)arg3, 1) != 0) {
                result = -1;
                break;
            }
            result = bsd_read((int)arg1, (void*)arg2, (uint32_t)arg3);
            break;
        }
            
        case SYS_USER_WRITE: {
            /* write(fd, buf, count) — kernel reads from user buf */
            if (arg3 > 0 && validate_user_ptr((const void*)arg2, (size_t)arg3, 0) != 0) {
                result = -1;
                break;
            }
            result = bsd_write((int)arg1, (const void*)arg2, (uint32_t)arg3);
            break;
        }
            
        case SYS_USER_OPEN: {
            /* open(path, flags, mode) — kernel reads path string from user */
            char k_path[256];
            if (copy_from_user(k_path, (const void*)arg1, sizeof(k_path)) != 0) {
                result = -1;
                break;
            }
            k_path[255] = '\0';  /* Ensure NUL-termination */
            result = bsd_open(k_path, (int)arg2, (int)arg3);
            break;
        }
            
        case SYS_USER_CLOSE:
            result = bsd_close((int)arg1);
            break;
            
        case SYS_USER_FORK:
            result = process_fork();
            break;
            
        case SYS_USER_EXEC: {
            /* exec(path, argv) — kernel reads path & argv from user */
            char k_path[256];
            if (copy_from_user(k_path, (const void*)arg1, sizeof(k_path)) != 0) {
                result = -1;
                break;
            }
            k_path[255] = '\0';
            /* Validate the argv pointer if non-NULL */
            if (arg2 && validate_user_ptr((const void*)arg2, sizeof(char*), 0) != 0) {
                result = -1;
                break;
            }
            result = process_exec(k_path, (char* const*)arg2);
            break;
        }
            
        case SYS_USER_YIELD:
            scheduler_yield();
            result = 0;
            break;
            
        default:
            // Unknown syscall
            {
                char buf[64];
                serial_write_string("SYSCALL: Unknown syscall ");
                extern void int_to_str(int, char*);
                int_to_str(syscall_num, buf);
                serial_write_string(buf);
                serial_write_string("\n");
            }
            result = -1; // ENOSYS
            break;
    }
    
    // Store the result in EAX so it's returned to the caller
    regs->eax = (uint32_t)result;
}

// ============================================================================
// SYSCALL INITIALIZATION
// ============================================================================

extern void syscall_entry(void); // Assembly entry point

void init_syscall(void) {
    // Install syscall_entry as the handler for int 0x80
    // 0x8E = Present, Ring 0, 32-bit interrupt gate
    // For user-mode syscalls later, we'd use 0xEE (Ring 3 accessible)
    idt_set_gate(0x80, (uint32_t)syscall_entry, 0x08, 0xEE); // Ring 3 accessible!
    
    // Move RTL8169 NIC handler from 0x80 to 0x81
    extern void isr129(void); // Assembly ISR for vector 0x81
    idt_set_gate(0x81, (uint32_t)isr129, 0x08, 0x8E);
    
    serial_write_string("SYSCALL: Initialized (int 0x80), NIC moved to int 0x81\n");
}

// ============================================================================
// FAST SYSCALL (SYSENTER/SYSEXIT) - Task 7
// ============================================================================

// MSR addresses for sysenter/sysexit
#define IA32_SYSENTER_CS   0x174
#define IA32_SYSENTER_ESP  0x175
#define IA32_SYSENTER_EIP  0x176

// Helper: write to an MSR
static inline void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    asm volatile(
        "wrmsr"
        :
        : "c"(msr), "a"(lo), "d"(hi)
    );
}

// Helper: read from an MSR
static inline void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    asm volatile(
        "rdmsr"
        : "=a"(*lo), "=d"(*hi)
        : "c"(msr)
    );
}

// Assembly entry point for sysenter (defined in system_entry.asm)
extern void sysenter_entry(void);

void syscall_init_fast(void) {
    // Set up MSRs for sysenter/sysexit fast system call mechanism
    
    // IA32_SYSENTER_CS: kernel code segment selector (0x08)
    // When sysenter executes, the CPU loads:
    //   CS  = value from this MSR
    //   SS  = value from this MSR + 8 (so 0x10 = kernel data)
    wrmsr(IA32_SYSENTER_CS, 0x08, 0);
    
    // IA32_SYSENTER_ESP: kernel stack pointer
    // We use the current kernel stack top. The scheduler will update
    // the TSS ESP0 on task switch; for sysenter we also need a
    // dedicated kernel stack. For now, use a static kernel stack.
    extern uint32_t tss_esp0_for_sysenter;  // Defined below
    static uint8_t sysenter_stack[8192] __attribute__((aligned(16)));
    uint32_t stack_top = (uint32_t)sysenter_stack + sizeof(sysenter_stack);
    tss_esp0_for_sysenter = stack_top;
    wrmsr(IA32_SYSENTER_ESP, stack_top, 0);
    
    // IA32_SYSENTER_EIP: syscall handler entry point
    wrmsr(IA32_SYSENTER_EIP, (uint32_t)sysenter_entry, 0);
    
    s_printf("[SYSCALL] Fast syscall (sysenter/sysexit) initialized\n");
}

// tss_esp0_for_sysenter is defined in hal/cpu/gdt.c
// It holds the current kernel stack for sysenter, updated by the
// scheduler when switching to a Ring 3 task.
