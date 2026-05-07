/**
 * process.h - Process Management Syscalls for CamelOS
 *
 * Provides the UNIX process lifecycle API: fork, exec, wait, exit,
 * kill, and introspection (ps).  Layered on top of the existing
 * scheduler, VMM (COW fork), signal, and pipe subsystems.
 *
 * Design:
 *   - A static process_table[] maps PIDs to process metadata.
 *   - fork() uses vmm_fork_address_space() for COW memory sharing.
 *   - exec() replaces the current address space via the Mach-O / CDL loader.
 *   - wait() blocks on BLOCK_REASON_WAITPID and is woken by SIGCHLD.
 *   - exit() marks the task zombie, notifies the parent, and yields.
 */

#ifndef PROCESS_H
#define PROCESS_H

#include "../include/types.h"
#include "task.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

/** Maximum number of concurrent processes (fits the scheduler PID space) */
#define MAX_PROCESSES     256

/** Reserved PIDs */
#define PID_KERNEL        0
#define PID_INIT          1

/** Return codes for process_wait / process_fork */
#define PROCESS_OK        0
#define PROCESS_ERR      -1

/** exec() failed to load the binary */
#define PROCESS_EXEC_FAIL -2

/** wait() would block and WNOHANG was requested */
#define PROCESS_WNOHANG   -3

/** No child matching the requested PID */
#define PROCESS_ECHILD   -4

/* ========================================================================
 * Process State Constants  (mirrors task.h TASK_STATE_*)
 * ======================================================================== */

#define PROC_STATE_READY     TASK_STATE_READY
#define PROC_STATE_RUNNING   TASK_STATE_RUNNING
#define PROC_STATE_BLOCKED   TASK_STATE_BLOCKED
#define PROC_STATE_ZOMBIE    TASK_STATE_ZOMBIE
#define PROC_STATE_SLEEPING  TASK_STATE_SLEEPING

/* ========================================================================
 * Process Info — snapshot for `ps` / process_list()
 * ======================================================================== */

typedef struct {
    int   pid;                /* Process ID               */
    int   ppid;               /* Parent PID               */
    int   uid;                /* User ID                  */
    int   state;              /* PROC_STATE_*             */
    int   exit_code;          /* Only valid if ZOMBIE     */
    char  name[32];           /* Process name             */
    uint32_t cpu_ticks;       /* Cumulative CPU time      */
} process_info_t;

/* ========================================================================
 * Process Table Entry — internal kernel bookkeeping
 * ======================================================================== */

/* Forward declaration — full type is in vmm.h; we use void* in the
 * header to avoid pulling vmm.h into every consumer of process.h.
 * process.c casts to address_space_t* internally. */

typedef struct {
    int       in_use;         /* Slot is allocated        */
    int       pid;            /* Same as task->id         */
    int       ppid;           /* Parent PID               */
    task_t*   task;           /* Pointer to TCB (NULL if zombie & reaped) */
    void*     addr_space;     /* Cached address space (address_space_t*, kept while zombie) */
    int       exit_code;      /* Exit status (valid when zombie) */
    int       is_zombie;      /* 1 after exit, 0 after wait reaps */
} process_entry_t;

/* ========================================================================
 * Public API
 * ======================================================================== */

/**
 * Initialize the process subsystem.
 * Sets up the process table, registers PID 0 (kernel) and PID 1 (init).
 * Must be called after scheduler_init() and vmm_init().
 */
void process_init(void);

/**
 * Fork the current process.
 * Creates a child that is an exact copy of the caller:
 *   - COW-forked address space (vmm_fork_address_space)
 *   - Copied signal state
 *   - Same register context so the child resumes from the same point
 *
 * @return  Child PID to the parent, 0 to the child, -1 on error
 */
int process_fork(void);

/**
 * Replace the current process image with a new executable.
 *
 *   1. Load the binary via Mach-O or CDL loader
 *   2. Create a fresh address space
 *   3. Reset signal handlers to defaults
 *   4. Jump to the new entry point
 *
 * Does not return on success; returns -1 on failure (old image intact).
 *
 * @param path  Path to the executable
 * @param argv  Argument vector (NULL-terminated)
 * @return      Does not return on success; -1 on failure
 */
int process_exec(const char* path, char* const argv[]);

/**
 * Execute a binary in user mode (Ring 3).
 *
 * Like process_exec() but sets up the task's context frame for
 * Ring 3 return: CS=0x1B, DS=ES=FS=GS=SS=0x23, EFLAGS with IOPL=0.
 * The binary runs at CPL 3 with no direct hardware access.
 *
 * @param path  Path to the executable
 * @param argv  Argument vector (NULL-terminated)
 * @return      Does not return on success; -1 on failure
 */
int process_exec_user(const char* path, const char** argv);

/**
 * Terminate the current process.
 *
 *   1. Mark task as ZOMBIE
 *   2. Store exit code
 *   3. Send SIGCHLD to parent
 *   4. Close all open pipe file descriptors
 *   5. Yield (never returns)
 *
 * @param status  Exit status code (0-255)
 */
void process_exit(int status) __attribute__((noreturn));

/**
 * Wait for a child process to change state.
 *
 * If the child is already a zombie, it is reaped immediately.
 * Otherwise the caller blocks until SIGCHLD arrives.
 *
 * @param pid     -1 = wait for any child; >0 = wait for specific PID
 * @param status  [out] Exit status of the child (may be NULL)
 * @return        PID of the reaped child, or -1 on error
 */
int process_wait(int pid, int* status);

/**
 * Return the PID of the current process.
 */
int process_getpid(void);

/**
 * Return the PID of the parent of the current process.
 */
int process_getppid(void);

/**
 * Send a signal to a process.
 *
 * @param pid  Target PID
 * @param sig  Signal number (1-31)
 * @return     0 on success, -1 on error
 */
int process_kill(int pid, int sig);

/**
 * List all processes for `ps`.
 *
 * @param info  Caller-supplied array to fill
 * @param max   Maximum number of entries the array can hold
 * @return      Number of entries written
 */
int process_list(process_info_t* info, int max);

/* ========================================================================
 * Internal helpers (exposed for scheduler / syscall integration)
 * ======================================================================== */

/** Look up a process_entry_t by PID. Returns NULL if not found. */
process_entry_t* process_find_by_pid(int pid);

/** Reap a zombie entry — free its resources and mark the slot unused. */
void process_reap(int pid);

#endif /* PROCESS_H */
