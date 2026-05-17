/**
 * process.c - Process Management Syscalls for CamelOS
 *
 * Full implementation of the UNIX process lifecycle:
 *   fork / exec / wait / exit / kill / getpid / getppid / ps
 *
 * Layered on:
 *   - Preemptive scheduler  (core/scheduler.c)
 *   - VMM with COW fork     (core/vmm.c)
 *   - Signal subsystem      (core/signal.c)
 *   - Pipe / FD subsystem   (core/pipe.c)
 *   - Mach-O / CDL loader   (core/macho_loader.c, core/cdl_loader.c)
 */

#include "process.h"
#include "memory.h"
#include "string.h"
#include "scheduler.h"
#include "vmm.h"
#include "signal.h"
#include "pipe.h"
#include "macho_loader.h"
#include "../hal/cpu/isr.h"
#include "../hal/drivers/serial.h"

/* ========================================================================
 * Internal State
 * ======================================================================== */

/** The global process table -- indexed by PID for O(1) lookup */
static process_entry_t process_table[MAX_PROCESSES];

/** Next candidate PID (scans upward, wraps around) */
static int proc_next_pid = 2;  /* 0 = kernel, 1 = init are reserved */

/** Flag: subsystem initialized */
static int process_subsys_ready = 0;

/* Access the task list maintained in task.c */
extern task_t* task_list_head;

/* ========================================================================
 * Debug Helpers
 * ======================================================================== */

/**
 * Write a debug line to the serial console.
 * Uses s_printf which only accepts a plain const char* (no format args).
 * For dynamic values, build into a temp buffer with sprintf first.
 */
#define PROC_LOG(msg) s_printf("[PROC] " msg "\n")

/* Small helper to log a PID value */
static void proc_log_pid(const char* prefix, int pid_val)
{
    char buf[80];
    sprintf(buf, "[PROC] %s %d\n", prefix, pid_val);
    s_printf(buf);
}

/* Small helper to log two PIDs */
static void proc_log_ppid(const char* prefix, int val1, int val2)
{
    char buf[80];
    sprintf(buf, "[PROC] %s %d %d\n", prefix, val1, val2);
    s_printf(buf);
}

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/**
 * Allocate a free PID and process table slot.
 * Returns the PID (>0) on success, or -1 if the table is full.
 */
static int alloc_pid(void)
{
    /* Simple linear scan starting from proc_next_pid.
     * Wrap around if we hit MAX_PROCESSES. */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        int candidate = proc_next_pid;
        proc_next_pid++;
        if (proc_next_pid >= MAX_PROCESSES) {
            proc_next_pid = 2;  /* Skip reserved 0 and 1 */
        }
        if (!process_table[candidate].in_use) {
            /* Reserve the slot immediately so a concurrent alloc
             * (from an interrupt handler) cannot claim the same PID. */
            process_table[candidate].in_use = 1;
            return candidate;
        }
    }
    return -1;  /* Table full */
}

/**
 * Find a task_t* by PID by walking the circular task list.
 */
static task_t* find_task_by_pid(int pid)
{
    if (!task_list_head) return NULL;

    task_t* cur = task_list_head;
    do {
        if (cur->id == pid) return cur;
        cur = cur->next;
    } while (cur && cur != task_list_head);

    return NULL;
}

/**
 * Close all pipe file descriptors owned by a process.
 * Walks the per-process FD table maintained by pipe.c.
 */
static void close_all_fds(task_t* task)
{
    if (!task) return;

    /* Iterate the pipe FD table and close any that belong to us */
    for (int fd = 0; fd < PIPE_MAX_PIPES; fd++) {
        pipe_fd_t* pfd = pipe_fd_get(fd);
        if (pfd && pfd->owner == task) {
            pipe_close(fd);
        }
    }
}

/**
 * Copy the signal state from parent to child.
 * The child gets a fresh signal_state with the same handlers but
 * no pending signals and an empty blocked mask (POSIX semantics).
 */
static signal_state_t* fork_signal_state(task_t* parent)
{
    signal_state_t* child_ss = signal_state_create();
    if (!child_ss) return NULL;

    /* Copy handler table from parent -- the only inheritable part.
     * Pending signals and blocked mask are NOT inherited (POSIX). */
    if (parent && parent->signal_state) {
        signal_state_t* parent_ss = (signal_state_t*)parent->signal_state;
        for (int i = 0; i < MAX_SIGNALS; i++) {
            child_ss->handlers[i]     = parent_ss->handlers[i];
            child_ss->handler_flags[i] = parent_ss->handler_flags[i];
        }
    }

    return child_ss;
}

/**
 * Reset all signal handlers to SIG_DFL for exec().
 * Pending signals are cleared; the mask is reset.
 */
static void reset_signal_handlers(task_t* task)
{
    if (!task) return;

    /* Destroy old state and create a fresh one (all SIG_DFL) */
    signal_state_t* ss = (signal_state_t*)task->signal_state;
    if (ss) {
        signal_state_destroy(ss);
    }
    ss = signal_state_create();
    task->signal_state = ss;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void process_init(void)
{
    PROC_LOG("Initializing process subsystem...");

    /* Zero the entire process table */
    memset(process_table, 0, sizeof(process_table));

    /* ---- Register PID 0 (kernel) ---- */
    process_table[PID_KERNEL].in_use     = 1;
    process_table[PID_KERNEL].pid        = PID_KERNEL;
    process_table[PID_KERNEL].ppid       = 0;   /* Kernel is its own parent */
    process_table[PID_KERNEL].task       = find_task_by_pid(PID_KERNEL);
    process_table[PID_KERNEL].addr_space = NULL; /* Uses kernel address space */
    process_table[PID_KERNEL].exit_code  = 0;
    process_table[PID_KERNEL].is_zombie  = 0;

    /* ---- Register PID 1 (init / launchd placeholder) ---- */
    process_table[PID_INIT].in_use     = 1;
    process_table[PID_INIT].pid        = PID_INIT;
    process_table[PID_INIT].ppid       = PID_KERNEL;
    process_table[PID_INIT].task       = find_task_by_pid(PID_INIT);
    process_table[PID_INIT].addr_space = NULL;
    process_table[PID_INIT].exit_code  = 0;
    process_table[PID_INIT].is_zombie  = 0;

    process_subsys_ready = 1;

    PROC_LOG("Process table initialized (0=kernel, 1=init)");
}

/* -------------------------------------------------------------------- */

int process_fork(void)
{
    task_t* parent = scheduler_get_current();
    if (!parent) {
        PROC_LOG("fork: no current task");
        return PROCESS_ERR;
    }

    /* Allocate a new PID and process table slot */
    int child_pid = alloc_pid();
    if (child_pid < 0) {
        PROC_LOG("fork: process table full");
        return PROCESS_ERR;
    }

    proc_log_ppid("fork: parent, child_pid", parent->id, child_pid);

    /* ---- 1. Allocate a new task_t ---- */
    task_t* child = (task_t*)kzalloc(sizeof(task_t));
    if (!child) {
        PROC_LOG("fork: kzalloc failed for child task");
        process_table[child_pid].in_use = 0;  /* Release reserved PID slot */
        return PROCESS_ERR;
    }

    /* Copy parent's task fields wholesale, then fix up what differs */
    memcpy(child, parent, sizeof(task_t));
    child->id         = child_pid;
    child->parent_pid = parent->id;
    child->next       = NULL;
    child->state      = TASK_STATE_READY;

    /* ---- 2. Fork the address space (COW) ---- */
    address_space_t* parent_as = (address_space_t*)parent->address_space;
    if (parent_as) {
        address_space_t* child_as = vmm_fork_address_space(parent_as);
        if (!child_as) {
            PROC_LOG("fork: vmm_fork_address_space failed");
            kfree(child);
            process_table[child_pid].in_use = 0;
            return PROCESS_ERR;
        }
        child->address_space = child_as;
    } else {
        child->address_space = NULL;
    }

    /* ---- 3. Fork signal state ---- */
    child->signal_state = fork_signal_state(parent);
    /* If signal_state_create fails we still proceed -- the child
     * will simply have no signal handling until the signal
     * subsystem lazily creates one on first use. */

    /* ---- 4. Set up the child's kernel stack ----
     *
     * We need the child to resume execution at the same point as the
     * parent, with identical register state EXCEPT:
     *   - eax = 0  (fork returns 0 to the child)
     *
     * The parent's ESP points to a registers_t frame on the kernel
     * stack (pushed by the ISR / scheduler).  We allocate a new
     * kernel stack for the child and copy that frame, then patch
     * the return value.
     */
    {
        /* Allocate a fresh kernel stack for the child (16 KB) */
        uint32_t stack_size = 16384;
        uint8_t* stack_buf  = (uint8_t*)kmalloc(stack_size);
        if (!stack_buf) {
            PROC_LOG("fork: kmalloc failed for child stack");
            if (child->address_space) {
                vmm_destroy_address_space((address_space_t*)child->address_space);
            }
            if (child->signal_state) {
                signal_state_destroy((signal_state_t*)child->signal_state);
            }
            kfree(child);
            process_table[child_pid].in_use = 0;
            return PROCESS_ERR;
        }
        memset(stack_buf, 0, stack_size);

        /* Store the kernel stack base for cleanup in process_reap */
        child->kernel_stack = stack_buf;

        /* The parent's ESP points to a registers_t on its kernel stack.
         * Copy the entire register frame to the child's new stack. */
        registers_t* parent_regs = (registers_t*)parent->esp;

        /* Place the child's register frame at the top of the new stack,
         * aligned down by sizeof(registers_t). */
        uint32_t child_esp_top = (uint32_t)stack_buf + stack_size;
        child_esp_top -= sizeof(registers_t);
        registers_t* child_regs = (registers_t*)child_esp_top;

        /* Copy the parent's register state verbatim */
        memcpy(child_regs, parent_regs, sizeof(registers_t));

        /* Fork returns 0 to the child (eax holds the return value
         * after the context-switch assembly pops the register frame) */
        child_regs->eax = 0;

        /* Set the child's ESP to point to this register frame */
        child->esp = child_esp_top;
    }

    /* ---- 5. Add to scheduler with the same priority as the parent ---- */
    scheduler_add_task(child, parent->priority);

    /* ---- 6. Fill in the process table entry ---- */
    process_table[child_pid].pid        = child_pid;
    process_table[child_pid].ppid       = parent->id;
    process_table[child_pid].task       = child;
    process_table[child_pid].addr_space = child->address_space;  /* void* alias */
    process_table[child_pid].exit_code  = 0;
    process_table[child_pid].is_zombie  = 0;
    /* in_use was already set to 1 by alloc_pid() */

    /* ---- 7. Insert into the circular task linked list ---- */
    if (task_list_head) {
        task_t* tmp = task_list_head;
        while (tmp->next && tmp->next != task_list_head) {
            tmp = tmp->next;
        }
        tmp->next    = child;
        child->next  = task_list_head;  /* Keep circular */
    }

    /* Return child PID to the parent.
     * The child will eventually be scheduled and will pop its register
     * frame, seeing eax = 0. */
    return child_pid;
}

/* -------------------------------------------------------------------- */

int process_exec(const char* path, char* const argv[])
{
    if (!path) {
        PROC_LOG("exec: NULL path");
        return PROCESS_ERR;
    }

    task_t* current = scheduler_get_current();
    if (!current) {
        PROC_LOG("exec: no current task");
        return PROCESS_ERR;
    }

    proc_log_pid("exec: loading", current->id);

    /* ---- 1. Attempt to load the binary ----
     *
     * We try Mach-O first (for macOS app compatibility), then fall
     * back to CDL/ELF.  Both loaders return a handle we can use.
     *
     * IMPORTANT: We load *before* destroying the old address space so
     * that a load failure leaves the current process intact.
     */
    loaded_macho_t* macho_img = NULL;
    int             load_ok   = 0;

    /* Try Mach-O first */
    macho_img = macho_load(path);
    if (macho_img && macho_img->entry_point) {
        load_ok = 1;
        PROC_LOG("exec: Mach-O loaded OK");
    }

    /* Try CDL/ELF if Mach-O failed */
    if (!load_ok) {
        extern int internal_load_library(const char* p);
        int load_result = internal_load_library(path);
        if (load_result >= 0) {
            load_ok = 1;
            PROC_LOG("exec: CDL/ELF loaded OK");

            /* For CDL, the entry point was already invoked by the
             * loader.  The new code is active in the current address
             * space.  We still need to clean up signal state and
             * update the process name. */

            /* Reset signal handlers to defaults */
            reset_signal_handlers(current);

            /* Update process name to the new binary's basename */
            {
                const char* base = path;
                const char* p = path;
                while (*p) { if (*p == '/') base = p + 1; p++; }
                strncpy(current->name, base, 31);
                current->name[31] = '\0';
            }

            return PROCESS_OK;
        }
    }

    if (!load_ok) {
        PROC_LOG("exec: failed to load binary");
        return PROCESS_EXEC_FAIL;
    }

    /* ---- 2. Destroy the old address space ----
     * We only reach here for Mach-O loads.  The old image is
     * no longer needed. */
    address_space_t* old_as = (address_space_t*)current->address_space;
    if (old_as) {
        vmm_destroy_address_space(old_as);
        current->address_space = NULL;
    }

    /* ---- 3. Create a new address space for the loaded image ---- */
    address_space_t* new_as = vmm_create_address_space();
    if (!new_as) {
        PROC_LOG("exec: failed to create new address space (FATAL)");
        /* We already destroyed the old one -- this is fatal. */
        process_exit(127);
        /* Does not return */
    }
    current->address_space = new_as;

    /* Switch to the new address space immediately (loads CR3) */
    vmm_switch_address_space(new_as);

    /* ---- 4. Reset signal handlers to defaults ---- */
    reset_signal_handlers(current);

    /* ---- 5. Update the process name ---- */
    {
        const char* base = path;
        const char* p = path;
        while (*p) { if (*p == '/') base = p + 1; p++; }
        strncpy(current->name, base, 31);
        current->name[31] = '\0';
    }

    /* ---- 6. Jump to the new entry point ----
     *
     * For a Mach-O binary, macho_execute() calls the entry point.
     * It does not return on success.  If it does, we exit.
     */
    if (macho_img) {
        /* Build argc/argv for the new program */
        int argc = 0;
        if (argv) {
            while (argv[argc]) argc++;
        }
        macho_execute(macho_img, argc, (char**)argv);
        /* macho_execute should not return, but if it does: */
        process_exit(0);
    }

/* Should not reach here */
    return PROCESS_OK;
}

/* -------------------------------------------------------------------- */
/* Task 7: Execute a binary in user mode (Ring 3)                        */
/* -------------------------------------------------------------------- */

int process_exec_user(const char* path, const char** argv)
{
    if (!path) {
        PROC_LOG("exec_user: NULL path");
        return PROCESS_ERR;
    }

    task_t* current = scheduler_get_current();
    if (!current) {
        PROC_LOG("exec_user: no current task");
        return PROCESS_ERR;
    }

    proc_log_pid("exec_user: loading into Ring 3", current->id);

    /* ---- 1. Load the binary ---- */
    loaded_macho_t* macho_img = NULL;
    int load_ok = 0;

    /* Try Mach-O first */
    macho_img = macho_load(path);
    if (macho_img && macho_img->entry_point) {
        load_ok = 1;
        PROC_LOG("exec_user: Mach-O loaded OK");
    }

    /* Try CDL/ELF if Mach-O failed */
    if (!load_ok) {
        extern int internal_load_library(const char* p);
        int load_result = internal_load_library(path);
        if (load_result >= 0) {
            load_ok = 1;
            PROC_LOG("exec_user: CDL/ELF loaded OK");
        }
    }

    if (!load_ok) {
        PROC_LOG("exec_user: failed to load binary");
        return PROCESS_EXEC_FAIL;
    }

    /* ---- 2. Destroy old address space and create a new one ---- */
    address_space_t* old_as = (address_space_t*)current->address_space;
    if (old_as) {
        vmm_destroy_address_space(old_as);
        current->address_space = NULL;
    }

    address_space_t* new_as = vmm_create_address_space();
    if (!new_as) {
        PROC_LOG("exec_user: failed to create new address space (FATAL)");
        process_exit(127);
    }
    current->address_space = new_as;

    /* Switch to the new address space */
    vmm_switch_address_space(new_as);

    /* ---- 3. Map user code pages as user-accessible ---- */
    if (macho_img) {
        /* Map the Mach-O segments as user pages */
        extern void paging_set_user_page(uint32_t virtual_addr, int user_accessible);
        uint32_t entry = (uint32_t)macho_img->entry_point;
        /* Mark pages from entry point region as user-accessible.
         * The loader has already mapped them; we just need to set
         * the User/Supervisor bit in the page table entries. */
        for (uint32_t addr = (entry & 0xFFFFF000); addr < (entry & 0xFFFFF000) + 0x10000; addr += 0x1000) {
            paging_set_user_page(addr, 1);
        }
    }

    /* ---- 4. Allocate a user stack ---- */
    /* Map user stack at USER_STACK_TOP (0x7FFFF000) going down */
    uint32_t user_stack_top = 0x7FFFF000;
    uint32_t user_stack_size = USER_STACK_INIT;  /* 4MB */
    for (uint32_t addr = user_stack_top - user_stack_size; addr < user_stack_top; addr += 0x1000) {
        uint32_t frame = pmm_alloc_frame();
        if (frame) {
            vmm_map_page(new_as, addr, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
        }
    }

    /* ---- 5. Reset signal handlers ---- */
    reset_signal_handlers(current);

    /* ---- 6. Update the process name ---- */
    {
        const char* base = path;
        const char* p = path;
        while (*p) { if (*p == '/') base = p + 1; p++; }
        strncpy(current->name, base, 31);
        current->name[31] = '\0';
    }

    /* ---- 7. Set up the task's kernel stack for Ring 3 return ---- */
    {
        /* Allocate a fresh kernel stack for Ring 0 privilege transitions */
        uint32_t kstack_size = 16384;
        uint8_t* kstack_buf = (uint8_t*)kmalloc(kstack_size);
        if (!kstack_buf) {
            PROC_LOG("exec_user: kmalloc failed for kernel stack");
            process_exit(127);
        }

        uint32_t* top = (uint32_t*)(kstack_buf + kstack_size);

        /* Build the iret frame for Ring 3 return */
        *(--top) = 0x202;              /* EFLAGS: IF=1, IOPL=0 */
        *(--top) = 0x18 | 3;           /* CS = user code + RPL 3 */
        *(--top) = (uint32_t)macho_img->entry_point; /* EIP */

        *(--top) = 0;                  /* err_code */
        *(--top) = 32;                 /* int_no */

        /* pusha */
        *(--top) = 0;                  /* eax */
        *(--top) = 0;                  /* ecx */
        *(--top) = 0;                  /* edx */
        *(--top) = 0;                  /* ebx */
        *(--top) = user_stack_top;     /* esp (user stack) */
        *(--top) = 0;                  /* ebp */
        *(--top) = 0;                  /* esi */
        *(--top) = 0;                  /* edi */

        /* Segment registers */
        *(--top) = 0x20 | 3;           /* DS = user data + RPL 3 */
        *(--top) = 0x20 | 3;           /* ES */
        *(--top) = 0x20 | 3;           /* FS */
        *(--top) = 0x20 | 3;           /* GS */

        current->esp = (uint32_t)top;
    }

    /* ---- 8. Execute the entry point in user mode ---- */
    if (macho_img) {
        int argc = 0;
        if (argv) {
            while (argv[argc]) argc++;
        }
        /* We don't call macho_execute() here because that runs in Ring 0.
         * Instead, the scheduler will pick up this task and iret to Ring 3
         * via the context frame we just built. */
        PROC_LOG("exec_user: task context set up for Ring 3 return");
        /* Force a reschedule so the task returns to Ring 3 */
        scheduler_yield();
    }

    /* Should not reach here */
    return PROCESS_OK;
}

/* -------------------------------------------------------------------- */

void process_exit(int status)
{
    task_t* current = scheduler_get_current();
    if (!current) {
        /* No current task -- catastrophic.  Halt. */
        PROC_LOG("exit: FATAL -- no current task");
        for (;;) asm volatile("hlt");
    }

    int pid = current->id;
    proc_log_ppid("exit: pid, status", pid, status);

    /* ---- 1. Mark task as ZOMBIE ---- */
    current->state = TASK_STATE_ZOMBIE;

    /* ---- 2. Store exit code ---- */
    current->exit_code = status;

    /* ---- 3. Update process table entry ---- */
    if (pid >= 0 && pid < MAX_PROCESSES && process_table[pid].in_use) {
        process_table[pid].is_zombie  = 1;
        process_table[pid].exit_code  = status;
        /* Keep the process_entry alive until the parent wait()s.
         * The address_space pointer is cached here so we can
         * destroy it later during reap. */
    }

    /* ---- 4. Send SIGCHLD to parent ---- */
    int ppid = current->parent_pid;
    if (ppid > 0 && ppid < MAX_PROCESSES) {
        task_t* parent_task = find_task_by_pid(ppid);
        if (parent_task) {
            signal_send_to_task(parent_task, SIGCHLD, SI_KERNEL, (uint32_t)pid);

            /* If the parent is blocked in waitpid, unblock it so it
             * can reap this child immediately. */
            if (parent_task->state == TASK_STATE_BLOCKED &&
                parent_task->block_reason == BLOCK_REASON_WAITPID) {
                scheduler_unblock(parent_task);
            }
        }
    }

    /* ---- 5. Close all open file descriptors (pipes, etc.) ---- */
    close_all_fds(current);

    /* ---- 6. Remove from scheduler ----
     * We do NOT destroy the address space yet -- the parent may need
     * to inspect it (e.g. via /proc or wait()).  The address space
     * is destroyed during reap(). */
    scheduler_remove_task(current);

    /* ---- 7. Yield forever (this function never returns) ---- */
    scheduler_yield();

    /* If we somehow get back here (should not happen), halt */
    for (;;) {
        asm volatile("cli");
        asm volatile("hlt");
    }
}

/* -------------------------------------------------------------------- */

int process_wait(int pid, int* status)
{
    task_t* current = scheduler_get_current();
    if (!current) {
        PROC_LOG("wait: no current task");
        return PROCESS_ERR;
    }

    int my_pid = current->id;

    /* ---- Scan for zombie children ---- */
    int has_children = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_entry_t* pe = &process_table[i];
        if (!pe->in_use) continue;
        if (pe->ppid != my_pid) continue;

        has_children = 1;

        /* If looking for a specific child, skip others */
        if (pid > 0 && pe->pid != pid) continue;

        /* Found a zombie child -- reap it immediately */
        if (pe->is_zombie) {
            int reaped_pid = pe->pid;

            if (status) {
                *status = pe->exit_code;
            }

            process_reap(reaped_pid);

            proc_log_pid("wait: reaped zombie", reaped_pid);
            return reaped_pid;
        }
    }

    /* No zombie children found */

    /* If pid > 0, verify the target child actually exists */
    if (pid > 0) {
        if (pid >= MAX_PROCESSES || !process_table[pid].in_use ||
            process_table[pid].ppid != my_pid) {
            PROC_LOG("wait: no such child");
            return PROCESS_ECHILD;
        }
    }

    /* No children at all? */
    if (!has_children) {
        PROC_LOG("wait: no children");
        return PROCESS_ECHILD;
    }

    /* ---- Block until SIGCHLD arrives ---- */
    PROC_LOG("wait: blocking for SIGCHLD");

    current->state       = TASK_STATE_BLOCKED;
    current->block_reason = BLOCK_REASON_WAITPID;

    /* Yield to the scheduler.  We will be unblocked when a child
     * calls process_exit(), which sends SIGCHLD to us and calls
     * scheduler_unblock() for BLOCK_REASON_WAITPID. */
    scheduler_yield();

    /* ---- We've been unblocked -- re-scan for zombie children ---- */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_entry_t* pe = &process_table[i];
        if (!pe->in_use) continue;
        if (pe->ppid != my_pid) continue;
        if (pid > 0 && pe->pid != pid) continue;

        if (pe->is_zombie) {
            int reaped_pid = pe->pid;

            if (status) {
                *status = pe->exit_code;
            }

            process_reap(reaped_pid);

            proc_log_pid("wait: reaped after unblock", reaped_pid);
            return reaped_pid;
        }
    }

    /* Spurious wakeup -- no zombie found.  This can happen if
     * SIGCHLD was delivered for a stopped (not exited) child.
     * Return -1 so the caller can retry. */
    PROC_LOG("wait: spurious wakeup, retry");
    return PROCESS_ERR;
}

/* -------------------------------------------------------------------- */

int process_getpid(void)
{
    task_t* current = scheduler_get_current();
    return current ? current->id : 0;
}

/* -------------------------------------------------------------------- */

int process_getppid(void)
{
    task_t* current = scheduler_get_current();
    return current ? current->parent_pid : 0;
}

/* -------------------------------------------------------------------- */

int process_kill(int pid, int sig)
{
    /* Validate signal number */
    if (!signal_is_valid(sig)) {
        PROC_LOG("kill: invalid signal");
        return PROCESS_ERR;
    }

    /* Cannot send signals to PID 0 (kernel) or negative PIDs */
    if (pid <= 0) {
        PROC_LOG("kill: invalid PID");
        return PROCESS_ERR;
    }

    /* Find the target task */
    task_t* target = find_task_by_pid(pid);
    if (!target) {
        PROC_LOG("kill: PID not found");
        return PROCESS_ERR;
    }

    /* Send the signal via the signal subsystem */
    task_t* sender     = scheduler_get_current();
    int     sender_pid = sender ? sender->id : 0;

    int result = signal_send_to_task(target, sig, SI_USER, (uint32_t)sender_pid);

    /* For SIGKILL, pre-set the exit code in the process table
     * so that a subsequent wait() returns the correct status.
     * (POSIX defines the exit status as 128 + signo.) */
    if (sig == SIGKILL && result == 0) {
        if (pid >= 0 && pid < MAX_PROCESSES && process_table[pid].in_use) {
            process_table[pid].exit_code = 128 + sig;
        }
    }

    return result;
}

/* -------------------------------------------------------------------- */

int process_list(process_info_t* info, int max)
{
    if (!info || max <= 0) return 0;

    int count = 0;

    for (int i = 0; i < MAX_PROCESSES && count < max; i++) {
        process_entry_t* pe = &process_table[i];
        if (!pe->in_use) continue;

        process_info_t* pi = &info[count];
        pi->pid       = pe->pid;
        pi->ppid      = pe->ppid;
        pi->exit_code = pe->exit_code;

        if (pe->is_zombie) {
            /* Zombie -- no live task to inspect */
            pi->state     = PROC_STATE_ZOMBIE;
            pi->uid       = 0;
            pi->cpu_ticks = 0;
            pi->name[0]   = '\0';
        } else if (pe->task) {
            /* Live task */
            pi->state     = pe->task->state;
            pi->uid       = pe->task->uid;
            pi->cpu_ticks = pe->task->time_used;
            strncpy(pi->name, pe->task->name, 31);
            pi->name[31] = '\0';
        } else {
            /* Entry exists but task pointer is NULL (shouldn't
             * happen for non-zombie entries, but be safe) */
            pi->state     = PROC_STATE_ZOMBIE;
            pi->uid       = 0;
            pi->cpu_ticks = 0;
            pi->name[0]   = '\0';
        }

        count++;
    }

    return count;
}

/* ========================================================================
 * Internal Helpers (exposed for scheduler / syscall integration)
 * ======================================================================== */

process_entry_t* process_find_by_pid(int pid)
{
    if (pid < 0 || pid >= MAX_PROCESSES) return NULL;
    if (!process_table[pid].in_use) return NULL;
    return &process_table[pid];
}

void process_reap(int pid)
{
    if (pid < 0 || pid >= MAX_PROCESSES) return;

    process_entry_t* pe = &process_table[pid];
    if (!pe->in_use) return;

    proc_log_pid("reap: cleaning up PID", pid);

    /* Destroy the cached address space (kept alive until now so
     * the parent could inspect the zombie) */
    if (pe->addr_space) {
        vmm_destroy_address_space((address_space_t*)pe->addr_space);
        pe->addr_space = NULL;
    }

    /* Remove the task from the circular task list before freeing it,
     * otherwise we leave a dangling pointer in the linked list. */
    if (pe->task && task_list_head) {
        task_t* doomed = pe->task;

        /* Special case: doomed is the head of the list */
        if (doomed == task_list_head) {
            /* Find the tail (whose ->next points to head) */
            task_t* tail = task_list_head;
            while (tail->next && tail->next != task_list_head) {
                tail = tail->next;
            }
            if (doomed->next && doomed->next != doomed) {
                task_list_head = doomed->next;
                tail->next = task_list_head;
            } else {
                /* Last task in the list -- shouldn't happen for
                 * a normal child process, but be safe */
                task_list_head = NULL;
            }
        } else {
            /* Find the predecessor of doomed */
            task_t* prev = task_list_head;
            while (prev && prev->next != doomed) {
                prev = prev->next;
                if (prev == task_list_head) { prev = NULL; break; }
            }
            if (prev) {
                prev->next = doomed->next;
            }
        }
    }

    /* Destroy signal state if it wasn't already cleaned up */
    if (pe->task && pe->task->signal_state) {
        signal_state_destroy((signal_state_t*)pe->task->signal_state);
        pe->task->signal_state = NULL;
    }

    // Free kernel stack allocated in process_fork
    if (pe->task && pe->task->kernel_stack) {
        kfree(pe->task->kernel_stack);
        pe->task->kernel_stack = NULL;
    }

    /* Free the task_t itself */
    if (pe->task) {
        kfree(pe->task);
        pe->task = NULL;
    }

    /* Mark the slot as free */
    pe->in_use    = 0;
    pe->pid       = 0;
    pe->ppid      = 0;
    pe->exit_code = 0;
    pe->is_zombie = 0;
}
