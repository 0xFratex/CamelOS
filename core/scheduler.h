/**
 * Camel OS Preemptive Scheduler
 * 
 * Implements priority-based preemptive multitasking with:
 * - 256 priority levels (0 = highest priority)
 * - Round-robin within each priority level
 * - Timer-based preemption
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "../hal/cpu/isr.h"
#include "../core/task.h"

/* Priority levels: 0-255 (0 = highest priority) */
#define SCHED_PRIORITY_MIN      0       /* Highest priority - critical kernel tasks */
#define SCHED_PRIORITY_MAX      255     /* Lowest priority - background tasks */
#define SCHED_PRIORITY_DEFAULT  128     /* Default priority for new tasks */
#define SCHED_PRIORITY_KERNEL   32      /* Kernel threads */
#define SCHED_PRIORITY_USER     128     /* User applications */
#define SCHED_PRIORITY_IDLE     255     /* Idle task */

/* Task states - use defines from task.h if available */
#ifndef TASK_STATE_DEFINED
typedef enum {
    TASK_STATE_READY     = 0,    /* Task is ready to run */
    TASK_STATE_RUNNING   = 1,    /* Task is currently executing */
    TASK_STATE_BLOCKED   = 2,    /* Task is blocked (waiting for resource) */
    TASK_STATE_ZOMBIE    = 3,    /* Task has exited but not cleaned up */
    TASK_STATE_SLEEPING  = 4     /* Task is sleeping for a duration */
} task_state_t;
#endif

/* Block reasons */
typedef enum {
    BLOCK_REASON_NONE        = 0,
    BLOCK_REASON_IO          = 1,    /* Waiting for I/O */
    BLOCK_REASON_SEMAPHORE   = 2,    /* Waiting for semaphore */
    BLOCK_REASON_MUTEX       = 3,    /* Waiting for mutex */
    BLOCK_REASON_SLEEP       = 4,    /* Sleeping */
    BLOCK_REASON_WAITPID     = 5     /* Waiting for child process */
} block_reason_t;

/* Time slice configuration */
#define SCHED_DEFAULT_TIME_SLICE   10    /* Default time quantum in ticks */
#define SCHED_MAX_TIME_SLICE       100   /* Maximum time quantum */

/* Scheduler statistics */
typedef struct {
    uint32_t total_tasks;
    uint32_t context_switches;
    uint32_t tasks_created;
    uint32_t tasks_destroyed;
} sched_stats_t;

/* 
 * Scheduler API Functions 
 */

/**
 * Initialize the scheduler subsystem
 * Must be called after tasking_init()
 */
void scheduler_init(void);

/**
 * Add a task to the scheduler with specified priority
 * @param task     Pointer to task control block
 * @param priority Priority level (0-255, 0 = highest)
 */
void scheduler_add_task(task_t* task, uint8_t priority);

/**
 * Remove a task from the scheduler
 * @param task Pointer to task control block
 */
void scheduler_remove_task(task_t* task);

/**
 * Block the current task with a reason
 * @param reason Why the task is being blocked
 */
void scheduler_block(int reason);

/**
 * Unblock a task and make it ready to run
 * @param task Pointer to task control block to unblock
 */
void scheduler_unblock(task_t* task);

/**
 * Yield the CPU voluntarily
 * Current task goes to end of its priority queue
 */
void scheduler_yield(void);

/**
 * Get the currently running task
 * @return Pointer to current task's TCB
 */
task_t* scheduler_get_current(void);

/**
 * Get the priority of a task
 * @param task Pointer to task control block
 * @return Priority level (0-255)
 */
uint8_t scheduler_get_priority(task_t* task);

/**
 * Set the priority of a task
 * @param task     Pointer to task control block
 * @param priority New priority level (0-255)
 */
void scheduler_set_priority(task_t* task, uint8_t priority);

/**
 * Main scheduling function - called from timer ISR
 * Performs context switch if needed
 * @param regs Pointer to current register state on stack
 * @return New stack pointer to switch to (or same if no switch)
 */
uint32_t scheduler_schedule(registers_t* regs);

/**
 * Timer tick handler - called from timer ISR
 * Decrements time slice and triggers scheduling
 */
void scheduler_tick(void);

/**
 * Put current task to sleep for specified duration
 * @param ms Milliseconds to sleep
 */
void scheduler_sleep(uint32_t ms);

/**
 * Wake up a sleeping task
 * @param task Pointer to task control block
 */
void scheduler_wakeup(task_t* task);

/**
 * Get scheduler statistics
 * @return Pointer to statistics structure
 */
sched_stats_t* scheduler_get_stats(void);

/**
 * Dump scheduler state for debugging
 */
void scheduler_dump_state(void);

/* Assembly context switch function */
extern void context_switch_asm(uint32_t* old_esp_ptr, uint32_t new_esp);

#endif /* SCHEDULER_H */