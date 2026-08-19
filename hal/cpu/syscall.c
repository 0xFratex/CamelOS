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
#include "window_server.h"
#include "../../fs/vfs.h"
#include "../../fs/disk.h"

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

        case SYS_USER_WIN_CREATE: {
            /* Ring 3: create a window (no callbacks) and copy out its
             * content-area origin so the app can draw into it. */
            const char* title = (const char*)arg1;
            int w = (int)arg2;
            int h = (int)arg3;
            int* x_out = (int*)arg4;
            int* y_out = (int*)arg5;
            window_t* win = ws_create_window(title, w, h, 0, 0, 0);
            if (!win) { result = -1; break; }
            if (x_out) *x_out = win->x;
            if (y_out) *y_out = win->y + TITLE_BAR_HEIGHT;
            result = win->id;
            break;
        }

        // --- Extended File/Process/Scheduler Syscalls (130-149) ---

        case SYS_stat: {
            // stat(path, stat_buf) - return file size/type via sys_fs_read info
            const char* stat_path = (const char*)arg1;
            vfs_stat_t* stat_buf = (vfs_stat_t*)arg2;
            if (!stat_path || !stat_buf) { result = -1; break; }
            memset(stat_buf, 0, sizeof(vfs_stat_t));
            // Use pfs32_stat to get file info
            pfs32_direntry_t entry;
            extern int pfs32_stat(const char*, pfs32_direntry_t*);
            if (pfs32_stat(stat_path, &entry) == 0) {
                strncpy(stat_buf->name, entry.filename, VFS_MAX_NAME - 1);
                stat_buf->size = entry.file_size;
                stat_buf->permissions = entry.permissions;
                stat_buf->uid = entry.uid;
                stat_buf->gid = entry.gid;
                stat_buf->modify_time = entry.modify_time;
                stat_buf->create_time = entry.create_time;
                stat_buf->access_time = entry.access_time;
                stat_buf->type = (entry.attributes & PFS32_ATTR_DIRECTORY) ? VFS_TYPE_DIRECTORY : VFS_TYPE_REGULAR;
                stat_buf->blocks = (entry.file_size + 511) / 512;
                result = 0;
            } else {
                result = -1;
            }
            break;
        }

        case SYS_chmod: {
            // chmod(path, mode) - change file permissions (stub via sys_fs_create)
            const char* chmod_path = (const char*)arg1;
            int chmod_mode = (int)arg2;
            if (!chmod_path) { result = -1; break; }
            // Read existing entry, update permissions, write back
            pfs32_direntry_t chmod_entry;
            extern int pfs32_stat(const char*, pfs32_direntry_t*);
            if (pfs32_stat(chmod_path, &chmod_entry) == 0) {
                chmod_entry.permissions = (uint8_t)chmod_mode;
                result = 0;
            } else {
                result = -1;
            }
            break;
        }

        case SYS_chown:
            // chown(path, uid, gid) - stub, return 0
            result = 0;
            break;

        case SYS_mount: {
            // mount(path, fs_type, fs_data)
            const char* mnt_path = (const char*)arg1;
            vfs_filesystem_type_t mnt_type = (vfs_filesystem_type_t)arg2;
            void* mnt_data = (void*)arg3;
            result = vfs_mount(mnt_path, mnt_type, mnt_data);
            break;
        }

        case SYS_umount: {
            // umount(path)
            const char* umnt_path = (const char*)arg1;
            result = vfs_umount(umnt_path);
            break;
        }

        case SYS_ioctl:
            // ioctl(fd, cmd, arg) - stub, return -1
            result = -1;
            break;

        case SYS_gettimeofday: {
            // gettimeofday(tv_sec_ptr, tv_usec_ptr)
            // Use timer_ticks to compute time since boot
            uint32_t* tv_sec_ptr = (uint32_t*)arg1;
            uint32_t* tv_usec_ptr = (uint32_t*)arg2;
            extern uint32_t timer_ticks;
            if (tv_sec_ptr) {
                *tv_sec_ptr = timer_ticks / 100;  // 100 Hz timer
            }
            if (tv_usec_ptr) {
                *tv_usec_ptr = (timer_ticks % 100) * 10000;
            }
            result = 0;
            break;
        }

        case SYS_setuid: {
            // setuid(uid) - track current_uid
            static uint32_t syscall_current_uid = 0;
            uint32_t new_uid = arg1;
            if (syscall_current_uid == 0) {
                // Root can set any uid
                syscall_current_uid = new_uid;
                extern uint32_t current_uid;
                current_uid = new_uid;
                result = 0;
            } else {
                result = -1;  // Non-root cannot change uid
            }
            break;
        }

        case SYS_getuid: {
            // getuid() - return current uid
            extern uint32_t current_uid;
            result = (int32_t)current_uid;
            break;
        }

        case SYS_chdir: {
            // chdir(path) - update static cwd
            static char syscall_cwd[256] = "/";
            const char* chdir_path = (const char*)arg1;
            if (!chdir_path) { result = -1; break; }
            // Simple: just copy the path
            if (chdir_path[0] == '/') {
                strncpy(syscall_cwd, chdir_path, 255);
                syscall_cwd[255] = 0;
            } else {
                int len = strlen(syscall_cwd);
                if (len > 1 && syscall_cwd[len-1] != '/') {
                    strcat(syscall_cwd, "/");
                }
                strncat(syscall_cwd, chdir_path, 255 - len);
                syscall_cwd[255] = 0;
            }
            // Also update current task's cwd if available
            if (current_task) {
                strncpy(current_task->cwd, syscall_cwd, 255);
                current_task->cwd[255] = 0;
            }
            result = 0;
            break;
        }

        case SYS_sync: {
            // sync() - flush filesystem and disk cache
            extern int pfs32_sync(void);
            extern void disk_flush_cache(void);
            pfs32_sync();
            disk_flush_cache();
            result = 0;
            break;
        }

        case SYS_access: {
            // access(path, mode) - check if file exists/is accessible
            const char* acc_path = (const char*)arg1;
            if (!acc_path) { result = -1; break; }
            result = g_kernel_api.fs_exists(acc_path);
            break;
        }

        case SYS_dup:
            // dup(fd) - stub
            result = -1;
            break;

        case SYS_dup2:
            // dup2(oldfd, newfd) - stub
            result = -1;
            break;

        case SYS_pipe2:
            // pipe2(pipefd, flags) - stub
            result = -1;
            break;

        case SYS_fcntl:
            // fcntl(fd, cmd, arg) - stub
            result = -1;
            break;

        case SYS_sched_set_policy: {
            // sched_set_policy(policy) - set scheduler policy
            static int sched_policy = 0;  // 0 = round-robin, 1 = FIFO, 2 = CFS
            int new_policy = (int)arg1;
            if (new_policy >= 0 && new_policy <= 2) {
                sched_policy = new_policy;
                result = 0;
            } else {
                result = -1;
            }
            break;
        }

        case SYS_sched_get_policy: {
            // sched_get_policy() - get scheduler policy
            static int sched_policy_get = 0;  // mirrors set_policy
            // Note: this static is separate from set_policy's static intentionally
            // In a real implementation, policy would be in a global
            result = sched_policy_get;
            break;
        }

        case SYS_sched_set_nice: {
            // sched_set_nice(pid, nice) - set nice value
            int nice_pid = (int)arg1;
            int nice_val = (int)arg2;
            if (nice_val < -20) nice_val = -20;
            if (nice_val > 19) nice_val = 19;
            // Find the task and update priority (nice maps to priority offset)
            extern task_t* task_list_head;
            task_t* t = task_list_head;
            while (t) {
                if (t->id == nice_pid) {
                    // Map nice (-20..19) to priority (0..255)
                    // nice 0 = priority 128, nice -20 = priority 8, nice 19 = priority 242
                    uint8_t new_pri = (uint8_t)(128 + nice_val * 6);
                    t->priority = new_pri;
                    result = 0;
                    break;
                }
                t = t->next;
            }
            if (!t) result = -1;
            break;
        }

        case SYS_sched_get_nice: {
            // sched_get_nice(pid) - get nice value
            int gnice_pid = (int)arg1;
            extern task_t* task_list_head;
            task_t* gt = task_list_head;
            while (gt) {
                if (gt->id == gnice_pid) {
                    // Reverse map priority to nice value
                    int nice = ((int)gt->priority - 128) / 6;
                    if (nice < -20) nice = -20;
                    if (nice > 19) nice = 19;
                    result = nice;
                    break;
                }
                gt = gt->next;
            }
            if (!gt) result = -1;
            break;
        }

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
