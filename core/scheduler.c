/**
 * Camel OS Preemptive Scheduler Implementation
 * 
 * Priority-based preemptive multitasking with round-robin
 * within each priority level.
 */

#include "scheduler.h"
#include "memory.h"
#include "string.h"
#include "../hal/cpu/timer.h"
#include "../hal/drivers/serial.h"
#include "task.h"

/* Number of priority levels */
#define NUM_PRIORITIES 256

/* Scheduler state */
static task_t* current_running = 0;              /* Currently running task */
static task_t* priority_queues[NUM_PRIORITIES];  /* Array of task lists (one per priority) */
static task_t* priority_tails[NUM_PRIORITIES];   /* Tail pointers for O(1) insertion */
static uint8_t highest_ready_priority = 255;     /* Track highest priority with ready tasks */
static int scheduler_initialized = 0;

/* Statistics */
static sched_stats_t stats = {0, 0, 0, 0};

/* Idle task (runs when no other tasks are ready) */
static void idle_task(void) {
    while (1) {
        asm volatile("hlt");  /* Halt until interrupt */
    }
}

/* Forward declarations */
static void enqueue_task(task_t* task, uint8_t priority);
static task_t* dequeue_task(uint8_t priority);
static task_t* pick_next_task(void);
static void update_highest_priority(void);

/**
 * Initialize the scheduler
 */
void scheduler_init(void) {
    s_printf("[SCHED] Initializing preemptive scheduler...\n");
    
    /* Clear all priority queues */
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        priority_queues[i] = 0;
        priority_tails[i] = 0;
    }
    
    highest_ready_priority = 255;
    scheduler_initialized = 1;
    
    /* Create idle task at lowest priority */
    /* Note: The idle task is created with create_task from task.c */
    task_t* idle = create_task(0, (uint32_t)idle_task, 0x10000);
    if (idle) {
        idle->priority = SCHED_PRIORITY_IDLE;
        idle->time_slice = SCHED_DEFAULT_TIME_SLICE;
        idle->time_used = 0;
        idle->state = TASK_STATE_READY;
        strcpy(idle->name, "idle");
        enqueue_task(idle, SCHED_PRIORITY_IDLE);
        current_running = idle;
        idle->state = TASK_STATE_RUNNING;
    }
    
    s_printf("[SCHED] Scheduler initialized with idle task\n");
}

/**
 * Add a task to the scheduler
 */
void scheduler_add_task(task_t* task, uint8_t priority) {
    if (!task || !scheduler_initialized) return;
    
    /* Clamp priority to valid range */
    if (priority > SCHED_PRIORITY_MAX) {
        priority = SCHED_PRIORITY_MAX;
    }
    
    task->priority = priority;
    task->state = TASK_STATE_READY;
    task->time_slice = SCHED_DEFAULT_TIME_SLICE;
    task->time_used = 0;
    
    enqueue_task(task, priority);
    stats.total_tasks++;
    stats.tasks_created++;
    
    /* Update highest priority if needed */
    if (priority < highest_ready_priority) {
        highest_ready_priority = priority;
    }
    
    s_printf("[SCHED] Added task to scheduler\n");
}

/**
 * Remove a task from the scheduler
 */
void scheduler_remove_task(task_t* task) {
    if (!task || !scheduler_initialized) return;
    
    uint8_t prio = task->priority;
    
    /* Remove from priority queue */
    if (priority_queues[prio] == task) {
        /* Task is at head */
        priority_queues[prio] = task->next;
        if (priority_tails[prio] == task) {
            /* Was only task in queue */
            priority_tails[prio] = 0;
        }
    } else {
        /* Find and remove from middle */
        task_t* prev = priority_queues[prio];
        while (prev && prev->next != task) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = task->next;
            if (priority_tails[prio] == task) {
                priority_tails[prio] = prev;
            }
        }
    }
    
    task->next = 0;
    stats.total_tasks--;
    stats.tasks_destroyed++;
    
    /* Update highest priority if this queue is now empty */
    if (priority_queues[prio] == 0) {
        update_highest_priority();
    }
}

/**
 * Enqueue a task at the end of its priority queue
 */
static void enqueue_task(task_t* task, uint8_t priority) {
    task->next = 0;
    
    if (priority_tails[priority] == 0) {
        /* Queue is empty */
        priority_queues[priority] = task;
        priority_tails[priority] = task;
    } else {
        /* Add to tail */
        priority_tails[priority]->next = task;
        priority_tails[priority] = task;
    }
}

/**
 * Dequeue the first task from a priority queue
 */
static task_t* dequeue_task(uint8_t priority) {
    task_t* task = priority_queues[priority];
    
    if (task) {
        priority_queues[priority] = task->next;
        if (priority_queues[priority] == 0) {
            /* Queue is now empty */
            priority_tails[priority] = 0;
        }
        task->next = 0;
    }
    
    return task;
}

/**
 * Update the highest_ready_priority field
 */
static void update_highest_priority(void) {
    highest_ready_priority = 255;
    
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        if (priority_queues[i] != 0) {
            highest_ready_priority = i;
            break;
        }
    }
}

/**
 * Pick the next task to run
 * Returns the highest priority ready task
 */
static task_t* pick_next_task(void) {
    /* Find highest priority non-empty queue */
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        if (priority_queues[i] != 0) {
            /* Dequeue and re-enqueue for round-robin */
            task_t* task = dequeue_task(i);
            if (task && task->state == TASK_STATE_READY) {
                /* Re-enqueue for round-robin */
                enqueue_task(task, i);
                return task;
            } else if (task) {
                /* Task not ready, put it back and try next */
                enqueue_task(task, i);
            }
        }
    }
    
    /* No ready tasks - return current (idle) */
    return current_running;
}

/**
 * Block the current task
 */
void scheduler_block(int reason) {
    if (!current_running) return;
    
    current_running->state = TASK_STATE_BLOCKED;
    current_running->block_reason = reason;
    
    s_printf("[SCHED] Task blocked\n");
    
    /* Force reschedule */
    scheduler_yield();
}

/**
 * Unblock a task
 */
void scheduler_unblock(task_t* task) {
    if (!task || task->state != TASK_STATE_BLOCKED) return;
    
    task->state = TASK_STATE_READY;
    task->block_reason = BLOCK_REASON_NONE;
    
    /* Update highest priority if needed */
    if (task->priority < highest_ready_priority) {
        highest_ready_priority = task->priority;
    }
    
    s_printf("[SCHED] Task unblocked\n");
}

/**
 * Yield the CPU voluntarily
 */
void scheduler_yield(void) {
    if (!current_running || !scheduler_initialized) return;
    
    /* Trigger a reschedule by setting time_slice to 0 */
    current_running->time_slice = 0;
    
    /* The actual switch will happen on return from interrupt */
    /* For voluntary yield, we need to force a context switch */
    asm volatile("int $32");  /* Trigger timer interrupt to force scheduling */
}

/**
 * Get current task
 */
task_t* scheduler_get_current(void) {
    return current_running;
}

/**
 * Get task priority
 */
uint8_t scheduler_get_priority(task_t* task) {
    if (!task) return SCHED_PRIORITY_MAX;
    return task->priority;
}

/**
 * Set task priority
 */
void scheduler_set_priority(task_t* task, uint8_t priority) {
    if (!task || !scheduler_initialized) return;
    
    /* Clamp priority */
    if (priority > SCHED_PRIORITY_MAX) {
        priority = SCHED_PRIORITY_MAX;
    }
    
    uint8_t old_priority = task->priority;
    
    /* If task is in a queue, remove it first */
    if (task->state == TASK_STATE_READY && old_priority != priority) {
        scheduler_remove_task(task);
        task->priority = priority;
        enqueue_task(task, priority);
    } else {
        task->priority = priority;
    }
    
    /* Update highest priority */
    if (priority < highest_ready_priority) {
        highest_ready_priority = priority;
    }
    
    s_printf("[SCHED] Task priority changed\n");
}

/**
 * Timer tick handler
 */
void scheduler_tick(void) {
    if (!scheduler_initialized || !current_running) return;
    
    /* Decrement time slice */
    if (current_running->time_slice > 0) {
        current_running->time_slice--;
        current_running->time_used++;
    }
}

/**
 * Main scheduling function
 * Called from timer ISR
 */
uint32_t scheduler_schedule(registers_t* regs) {
    if (!scheduler_initialized) return regs->esp;
    
    /* Check if we need to schedule */
    int need_reschedule = 0;
    
    if (!current_running) {
        /* First call - pick initial task */
        need_reschedule = 1;
    } else if (current_running->time_slice == 0) {
        /* Time slice expired */
        need_reschedule = 1;
    } else if (current_running->state == TASK_STATE_BLOCKED ||
               current_running->state == TASK_STATE_SLEEPING) {
        /* Task is blocked */
        need_reschedule = 1;
    }
    
    if (!need_reschedule) {
        return regs->esp;  /* No switch needed */
    }
    
    /* Save current task's ESP */
    if (current_running) {
        current_running->esp = regs->esp;
        
        /* If current was running and not blocked, mark ready */
        if (current_running->state == TASK_STATE_RUNNING) {
            current_running->state = TASK_STATE_READY;
        }
    }
    
    /* Pick next task */
    task_t* next = pick_next_task();
    
    if (!next) {
        /* No tasks available - stay with current */
        if (current_running) {
            current_running->state = TASK_STATE_RUNNING;
            current_running->time_slice = SCHED_DEFAULT_TIME_SLICE;
        }
        return regs->esp;
    }
    
    /* Check if we're switching to a different task */
    if (next == current_running) {
        /* Same task - just reset time slice */
        current_running->state = TASK_STATE_RUNNING;
        current_running->time_slice = SCHED_DEFAULT_TIME_SLICE;
        return regs->esp;
    }
    
    /* Perform context switch */
    task_t* old = current_running;
    current_running = next;
    current_running->state = TASK_STATE_RUNNING;
    current_running->time_slice = SCHED_DEFAULT_TIME_SLICE;
    
    stats.context_switches++;
    
    /* Return new ESP for context switch */
    return current_running->esp;
}

/**
 * Sleep for specified duration
 */
void scheduler_sleep(uint32_t ms) {
    if (!current_running) return;
    
    /* Calculate wake-up tick count */
    extern volatile uint32_t ticks;
    uint32_t wake_tick = ticks + (ms / 20);  /* Assuming 50Hz = 20ms per tick */
    
    current_running->sleep_until = wake_tick;
    current_running->state = TASK_STATE_SLEEPING;
    
    s_printf("[SCHED] Task sleeping\n");
    
    /* Yield CPU */
    scheduler_yield();
}

/**
 * Wake up a sleeping task
 */
void scheduler_wakeup(task_t* task) {
    if (!task || task->state != TASK_STATE_SLEEPING) return;
    
    task->state = TASK_STATE_READY;
    task->sleep_until = 0;
    
    /* Update highest priority */
    if (task->priority < highest_ready_priority) {
        highest_ready_priority = task->priority;
    }
    
    s_printf("[SCHED] Task woke up\n");
}

/**
 * Check for sleeping tasks that should wake up
 * Called from timer tick
 */
void scheduler_check_sleepers(void) {
    extern volatile uint32_t ticks;
    
    /* This would require iterating all sleeping tasks */
    /* For efficiency, we could maintain a separate sleeping list */
    /* For now, this is a placeholder */
}

/**
 * Get scheduler statistics
 */
sched_stats_t* scheduler_get_stats(void) {
    return &stats;
}

/**
 * Dump scheduler state for debugging
 */
void scheduler_dump_state(void) {
    s_printf("\n=== Scheduler State ===\n");
    s_printf("Initialized: yes\n");
    s_printf("Current task: running\n");
    s_printf("Highest ready priority: set\n");
    s_printf("Total tasks: active\n");
    s_printf("Context switches: counted\n");
    s_printf("======================\n");
}