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
            // Fork is complex - for now return -1 (ENOSYS)
            // Full implementation requires COW + address space duplication
            result = -1;
            break;
            
        case SYS_WAITPID:
            // Simplified waitpid - check if child is zombie
            result = -1; // Not yet fully implemented
            break;
            
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
            result = 0; // Stub - mprotect not yet fully implemented
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
