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

/* Current scheduling policy (default: priority-based) */
static int sched_policy = SCHED_POLICY_PRIORITY;

/* Global minimum vruntime for EEVDF */
static uint32_t eevdf_min_vruntime = 0;

/* Context switch flag - read by assembly IRQ stub after isr_handler returns.
 * If non-zero, the assembly stub switches ESP to sched_new_esp before popa+iret.
 * This must be a global symbol visible to assembly. */
uint32_t sched_context_switch_needed = 0;
uint32_t sched_new_esp = 0;

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
static void eevdf_task_init(task_t* task);
static uint32_t eevdf_nice_to_weight(int nice);
static uint32_t eevdf_calc_slice(task_t* task);
static void eevdf_update_min_vruntime(void);
static task_t* eevdf_pick_next(void);
static void eevdf_update_current(void);

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
        eevdf_task_init(idle);
        idle->eevdf_nice = 19;  /* Lowest nice for idle */
        idle->eevdf_weight = eevdf_nice_to_weight(19);
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
    
    /* Initialize EEVDF fields for the new task */
    eevdf_task_init(task);
    
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

/* Forward declaration - scheduler_check_sleepers is defined after scheduler_tick */
void scheduler_check_sleepers(void);

/**
 * Timer tick handler
 */
void scheduler_tick(void) {
    if (!scheduler_initialized || !current_running) return;
    
    if (sched_policy == SCHED_POLICY_EEVDF) {
        /* EEVDF: advance vruntime and check if slice expired */
        eevdf_update_current();
        current_running->time_used++;
        if (current_running->time_slice > 0) {
            current_running->time_slice--;
        }
    } else {
        /* Priority-based: decrement time slice */
        if (current_running->time_slice > 0) {
            current_running->time_slice--;
            current_running->time_used++;
        }
    }
    
    /* Check for sleeping tasks that should wake up */
    scheduler_check_sleepers();
}

/**
 * Main scheduling function
 * Called from timer ISR
 *
 * Sets sched_context_switch_needed and sched_new_esp globals
 * which are read by the assembly IRQ stub to perform the
 * actual stack switch after this function returns.
 */
uint32_t scheduler_schedule(registers_t* regs) {
    /* Clear context switch flag at the start of each tick */
    sched_context_switch_needed = 0;

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
    
    /* Save current task's ESP.
     * regs points at the register frame base (the gs slot pushed by the stub);
     * regs->esp is the pusha ESP (frame base + 48), which the assembly restore
     * does NOT expect. The IRQ stub does `mov esp, sched_new_esp` then pops
     * gs/fs/es/ds + pusha + add esp,8 + iret, so we must store the frame base. */
    if (current_running) {
        current_running->esp = (uint32_t)regs;
        
        /* If current was running and not blocked, mark ready */
        if (current_running->state == TASK_STATE_RUNNING) {
            current_running->state = TASK_STATE_READY;
        }
    }
    
    /* Pick next task (use EEVDF or priority-based depending on policy) */
    task_t* next;
    if (sched_policy == SCHED_POLICY_EEVDF) {
        next = eevdf_pick_next();
    } else {
        next = pick_next_task();
    }
    
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
    
    /* Switch address space if needed.
     *
     * If the next task has an address space, switch to it.
     * If the next task has NO address space (kernel/idle tasks), we must
     * switch back to kernel_directory so that kernel code can access
     * VRAM, APIC MMIO, and other kernel-only mappings.  Without this,
     * the scheduler leaves CR3 pointing at the previous user process's
     * page directory, causing page faults when kernel code writes to
     * high kernel addresses like VRAM at 0xFD000000.
     */
    if (next->address_space) {
        if (next->address_space != old->address_space) {
            extern void vmm_switch_address_space(void*);
            vmm_switch_address_space(next->address_space);
        }
    } else if (old->address_space) {
        /* Switching from a user task back to the kernel — restore
         * kernel_directory so kernel code can access all mappings. */
        extern void vmm_switch_address_space(void*);
        vmm_switch_address_space(NULL);
    }
    
    /* Update TSS kernel stack pointer for Ring 3 transitions.
     * ESP0 must point to the TOP of the task's own kernel stack (a free
     * location for the CPU to push onto), NOT its saved register frame.
     * Kernel (Ring 0) tasks have kernel_stack_top == 0, in which case ESP0
     * is never used, so fall back to the saved ESP. */
    extern void tss_set_kernel_stack(uint32_t);
    tss_set_kernel_stack(next->kernel_stack_top ? next->kernel_stack_top
                                                : (uint32_t)next->esp);

    /* Task 7: Also update the sysenter MSR kernel stack.
     * When a Ring 3 task does sysenter, the CPU loads ESP from
     * IA32_SYSENTER_ESP MSR. We update it here so the next
     * sysenter from the new task uses the correct kernel stack. */
    extern uint32_t tss_esp0_for_sysenter;
    tss_esp0_for_sysenter = next->esp;
    
    /* Signal to the assembly IRQ stub that a context switch is needed.
     * The stub will set ESP = sched_new_esp before doing popa+iret,
     * which restores the new task's register state and returns to it. */
    sched_context_switch_needed = 1;
    sched_new_esp = current_running->esp;
    
    /* Return new ESP (for compatibility; the assembly stub uses the globals) */
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
    
    /* Scan all priority queues for sleeping tasks that should wake up */
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        task_t* task = priority_queues[i];
        while (task) {
            if (task->state == TASK_STATE_SLEEPING && task->sleep_until != 0) {
                if (ticks >= task->sleep_until) {
                    task->state = TASK_STATE_READY;
                    task->sleep_until = 0;
                    task->block_reason = 0;
                    /* Update highest priority */
                    if (task->priority < highest_ready_priority) {
                        highest_ready_priority = task->priority;
                    }
                }
            }
            task = task->next;
        }
    }
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

/* ====================================================================
 * EEVDF (Earliest Eligible Virtual Deadline First) Scheduler
 * ====================================================================
 * Modern fair scheduling algorithm (Linux 6.6+).
 * Each task has a virtual runtime (vruntime) that advances at a rate
 * proportional to its weight. The task with the earliest eligible
 * virtual deadline is selected to run next.
 *
 * Weight table: maps nice values (-20..19) to scheduling weights.
 * Higher weight = more CPU time = lower nice value.
 * Based on the Linux kernel prio_to_weight table.
 */

static const uint32_t eevdf_prio_to_weight[40] = {
     88,    97,   107,   118,   130,   143,   158,   174,
    192,   212,   234,   258,   285,   314,   347,   382,
    421,   465,   513,   567,   626,   690,   761,   840,
    926,  1021,  1127,  1242,  1369,  1510,  1665,  1834,
   2020,  2226,  2453,  2703,  2979,  3284,  3620,  3991
};

/* Get weight from nice value */
static uint32_t eevdf_nice_to_weight(int nice) {
    if (nice < EEVDF_NICE_MIN) nice = EEVDF_NICE_MIN;
    if (nice > EEVDF_NICE_MAX) nice = EEVDF_NICE_MAX;
    return eevdf_prio_to_weight[nice + 20];
}

/* Calculate time slice for a task based on its weight relative to total weight.
 * slice = sched_period * weight / total_weight
 * We compute total_weight by scanning all runnable tasks. */
static uint32_t eevdf_calc_slice(task_t* task) {
    uint32_t total_weight = 0;
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        task_t* t = priority_queues[i];
        while (t) {
            if (t->state == TASK_STATE_READY || t->state == TASK_STATE_RUNNING) {
                total_weight += t->eevdf_weight;
            }
            t = t->next;
        }
    }
    if (total_weight == 0) total_weight = 1;

    /* slice = period * weight / total_weight, minimum 1 tick */
    uint32_t slice = (EEVDF_SCHED_PERIOD * task->eevdf_weight) / total_weight;
    if (slice < 1) slice = 1;
    if (slice > SCHED_MAX_TIME_SLICE) slice = SCHED_MAX_TIME_SLICE;
    return slice;
}

/* Initialize EEVDF fields for a task (called when adding a task) */
static void eevdf_task_init(task_t* task) {
    task->eevdf_nice = EEVDF_NICE_DEFAULT;
    task->eevdf_weight = eevdf_nice_to_weight(EEVDF_NICE_DEFAULT);
    task->eevdf_vruntime = eevdf_min_vruntime;  /* Start at min so new tasks don't monopolize */
    task->eevdf_slice = EEVDF_SCHED_PERIOD;
    task->eevdf_deadline = eevdf_min_vruntime + task->eevdf_slice;
    task->eevdf_start_tick = 0;
}

/* Update min_vruntime: find the minimum vruntime among all runnable tasks.
 * This is the eligibility threshold. */
static void eevdf_update_min_vruntime(void) {
    uint32_t min_vr = 0xFFFFFFFF;
    int found = 0;
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        task_t* t = priority_queues[i];
        while (t) {
            if (t->state == TASK_STATE_READY || t->state == TASK_STATE_RUNNING) {
                if (t->eevdf_vruntime < min_vr) {
                    min_vr = t->eevdf_vruntime;
                    found = 1;
                }
            }
            t = t->next;
        }
    }
    if (found) {
        /* Only advance min_vruntime, never go backwards */
        if (min_vr > eevdf_min_vruntime) {
            eevdf_min_vruntime = min_vr;
        }
    }
}

/* EEVDF: Pick the next task to run.
 * A task is eligible if its vruntime <= min_vruntime (approximate fairness).
 * Among eligible tasks, pick the one with the smallest deadline.
 * If no task is eligible, pick the one with the smallest vruntime (to advance). */
static task_t* eevdf_pick_next(void) {
    task_t* best = 0;
    uint32_t best_deadline = 0xFFFFFFFF;

    /* First pass: find eligible task with earliest deadline */
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        task_t* t = priority_queues[i];
        while (t) {
            if (t->state == TASK_STATE_READY) {
                /* Eligibility check: vruntime <= min_vruntime + margin
                 * The margin prevents starvation when all tasks are close */
                if (t->eevdf_vruntime <= eevdf_min_vruntime + t->eevdf_slice) {
                    if (t->eevdf_deadline < best_deadline) {
                        best_deadline = t->eevdf_deadline;
                        best = t;
                    }
                }
            }
            t = t->next;
        }
    }

    /* If no eligible task, fall back to smallest vruntime */
    if (!best) {
        uint32_t min_vr = 0xFFFFFFFF;
        for (int i = 0; i < NUM_PRIORITIES; i++) {
            task_t* t = priority_queues[i];
            while (t) {
                if (t->state == TASK_STATE_READY && t->eevdf_vruntime < min_vr) {
                    min_vr = t->eevdf_vruntime;
                    best = t;
                }
                t = t->next;
            }
        }
    }

    return best;
}

/* EEVDF: Update the running task's vruntime on each tick.
 * vruntime advances by (1 << SCALE) / weight per tick.
 * This means high-weight (low-nice) tasks have slow vruntime growth,
 * so they get more CPU time before their deadline expires. */
static void eevdf_update_current(void) {
    if (!current_running) return;

    task_t* t = current_running;
    uint32_t weight = t->eevdf_weight;
    if (weight == 0) weight = 1;

    /* Advance vruntime: proportional to 1/weight */
    uint32_t delta = (1 << EEVDF_VRUNTIME_SCALE) / weight;
    t->eevdf_vruntime += delta;

    /* Recalculate deadline */
    t->eevdf_slice = eevdf_calc_slice(t);
    t->eevdf_deadline = t->eevdf_vruntime + t->eevdf_slice;

    /* Update min_vruntime */
    eevdf_update_min_vruntime();
}

/* Set scheduling policy */
void scheduler_set_policy(int policy) {
    if (policy < SCHED_POLICY_PRIORITY || policy > SCHED_POLICY_EEVDF) return;
    int old = sched_policy;
    sched_policy = policy;

    if (policy == SCHED_POLICY_EEVDF && old != SCHED_POLICY_EEVDF) {
        /* Initialize EEVDF fields for all existing tasks */
        eevdf_min_vruntime = 0;
        for (int i = 0; i < NUM_PRIORITIES; i++) {
            task_t* t = priority_queues[i];
            while (t) {
                eevdf_task_init(t);
                t = t->next;
            }
        }
        s_printf("[SCHED] Switched to EEVDF policy\n");
    } else if (policy == SCHED_POLICY_PRIORITY && old != SCHED_POLICY_PRIORITY) {
        s_printf("[SCHED] Switched to priority policy\n");
    }
}

/* Get scheduling policy */
int scheduler_get_policy(void) {
    return sched_policy;
}

/* Set nice value for a task */
void scheduler_set_nice(task_t* task, int nice) {
    if (!task) return;
    if (nice < EEVDF_NICE_MIN) nice = EEVDF_NICE_MIN;
    if (nice > EEVDF_NICE_MAX) nice = EEVDF_NICE_MAX;
    task->eevdf_nice = nice;
    task->eevdf_weight = eevdf_nice_to_weight(nice);
    task->eevdf_slice = eevdf_calc_slice(task);
    /* Recalculate deadline with new weight */
    task->eevdf_deadline = task->eevdf_vruntime + task->eevdf_slice;
}

/* Get nice value for a task */
int scheduler_get_nice(task_t* task) {
    if (!task) return EEVDF_NICE_DEFAULT;
    return task->eevdf_nice;
}