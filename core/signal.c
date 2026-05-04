/**
 * CamelOS Signal Handling Implementation
 *
 * Provides POSIX-like signal delivery, disposition, and masking for tasks.
 * Signal state is maintained per-process via an internal PID-indexed table
 * since task_t does not yet carry a signal_state pointer.
 */

#include "signal.h"
#include "memory.h"
#include "string.h"
#include "scheduler.h"
#include "../hal/drivers/serial.h"

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

#define SIGNAL_MAX_TASKS  256

static int signal_initialized = 0;

/* Per-PID signal state lookup (indexed by task->id) */
static signal_state_t* task_signals[SIGNAL_MAX_TASKS];

/* Access the task list maintained in task.c */
extern task_t* task_list_head;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * Find a task by its PID by walking the circular task list.
 */
static task_t* find_task_by_pid(int pid) {
    if (!task_list_head) return NULL;

    task_t* cur = task_list_head;
    do {
        if (cur->id == pid) return cur;
        cur = cur->next;
    } while (cur && cur != task_list_head);

    return NULL;
}

/**
 * Retrieve (or lazily create) the signal state for a task.
 * Returns NULL if the PID is out of range or allocation fails.
 */
static signal_state_t* get_signal_state(task_t* task) {
    if (!task) return NULL;
    if (task->id < 0 || task->id >= SIGNAL_MAX_TASKS) return NULL;

    if (!task_signals[task->id]) {
        task_signals[task->id] = signal_state_create();
    }
    return task_signals[task->id];
}

/**
 * Return the signal_state for a task without creating one.
 */
static signal_state_t* peek_signal_state(task_t* task) {
    if (!task) return NULL;
    if (task->id < 0 || task->id >= SIGNAL_MAX_TASKS) return NULL;
    return task_signals[task->id];
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void signal_init(void) {
    for (int i = 0; i < SIGNAL_MAX_TASKS; i++) {
        task_signals[i] = NULL;
    }
    signal_initialized = 1;
    s_printf("[SIGNAL] Signal subsystem initialized\n");
}

signal_state_t* signal_state_create(void) {
    signal_state_t* ss = (signal_state_t*)kzalloc(sizeof(signal_state_t));
    if (!ss) {
        s_printf("[SIGNAL] ERROR: failed to allocate signal_state\n");
        return NULL;
    }

    /* kzalloc zeroes everything, so:
     *   pending_mask   = 0
     *   blocked_mask   = 0
     *   pending_count  = 0
     *   in_handler     = 0
     *   handlers[]     = 0  (== SIG_DFL)
     *   handler_flags[] = 0
     *
     * Just confirm handlers are SIG_DFL explicitly for clarity.
     */
    for (int i = 0; i < MAX_SIGNALS; i++) {
        ss->handlers[i] = SIG_DFL;
    }

    return ss;
}

void signal_state_destroy(signal_state_t* ss) {
    if (!ss) return;
    kfree(ss);
}

/* ------------------------------------------------------------------ */
/*  Signal Delivery                                                    */
/* ------------------------------------------------------------------ */

int signal_send(int target_pid, int signo, int code, uint32_t value) {
    if (!signal_is_valid(signo)) {
        s_printf("[SIGNAL] signal_send: invalid signal\n");
        return -1;
    }

    task_t* target = find_task_by_pid(target_pid);
    if (!target) {
        s_printf("[SIGNAL] signal_send: target PID not found\n");
        return -1;
    }

    return signal_send_to_task(target, signo, code, value);
}

int signal_send_to_task(task_t* task, int signo, int code, uint32_t value) {
    if (!task) return -1;
    if (!signal_is_valid(signo)) return -1;

    /* SIGKILL and SIGSTOP cannot be caught, blocked, or ignored.
     * They are always delivered regardless of the handler/mask.
     */

    signal_state_t* ss = get_signal_state(task);
    if (!ss) {
        s_printf("[SIGNAL] signal_send_to_task: no signal state\n");
        return -1;
    }

    /* Check if the pending queue is full */
    if (ss->pending_count >= SIG_NO_QUEUE) {
        s_printf("[SIGNAL] signal_send_to_task: pending queue full\n");
        return -1;
    }

    /* Enqueue the signal info */
    int slot = ss->pending_count;
    ss->pending_queue[slot].signo     = signo;
    ss->pending_queue[slot].sender_pid = (scheduler_get_current() ? scheduler_get_current()->id : 0);
    ss->pending_queue[slot].code      = code;
    ss->pending_queue[slot].value     = value;
    ss->pending_count++;

    /* Mark the signal as pending in the bitmask */
    ss->pending_mask |= (1U << signo);

    s_printf("[SIGNAL] Signal queued for delivery\n");

    /* If the task is BLOCKED and this signal would unblock it, do so now */
    if (task->state == TASK_STATE_BLOCKED) {
        if (signo == SIGKILL || signo == SIGCONT) {
            scheduler_unblock(task);
            s_printf("[SIGNAL] Task unblocked by signal\n");
        }
    }

    /* Also unblock SLEEPING tasks on SIGKILL */
    if (task->state == TASK_STATE_SLEEPING && signo == SIGKILL) {
        task->state = TASK_STATE_READY;
        task->sleep_until = 0;
        s_printf("[SIGNAL] Sleeping task woken by SIGKILL\n");
    }

    return 0;
}

void signal_send_kernel(task_t* task, int signo) {
    if (!task) return;
    signal_send_to_task(task, signo, SI_KERNEL, 0);
}

/* ------------------------------------------------------------------ */
/*  Signal Handling (scheduler calls before returning to userspace)     */
/* ------------------------------------------------------------------ */

/**
 * Remove the first pending siginfo entry for a given signal number
 * and compact the queue.
 */
static void dequeue_signal(signal_state_t* ss, int signo) {
    for (int i = 0; i < ss->pending_count; i++) {
        if (ss->pending_queue[i].signo == signo) {
            /* Shift remaining entries down */
            for (int j = i; j < ss->pending_count - 1; j++) {
                ss->pending_queue[j] = ss->pending_queue[j + 1];
            }
            ss->pending_count--;
            return;
        }
    }
}

void signal_check_pending(task_t* task) {
    if (!task) return;

    signal_state_t* ss = peek_signal_state(task);
    if (!ss) return;

    /* Guard against recursive signal delivery */
    if (ss->in_handler) return;

    /* Compute deliverable signals: pending & ~blocked */
    uint32_t deliverable = ss->pending_mask & ~ss->blocked_mask;

    if (deliverable == 0) return;

    /* Pick highest priority signal (lowest signal number) */
    int signo = -1;
    for (int i = 1; i < 32; i++) {
        if (deliverable & (1U << i)) {
            signo = i;
            break;
        }
    }

    if (signo < 0) return;

    /* Clear the pending bit and dequeue */
    ss->pending_mask &= ~(1U << signo);
    dequeue_signal(ss, signo);

    /* Determine disposition */
    signal_handler_t handler = ss->handlers[signo];

    if (handler == SIG_IGN) {
        /* Signal is ignored — discard it */
        s_printf("[SIGNAL] Signal ignored\n");
        return;
    }

    if (handler == SIG_DFL) {
        /* Default action */
        signal_default_action(task, signo);
        return;
    }

    /* Custom handler — call it */
    s_printf("[SIGNAL] Delivering signal to custom handler\n");
    ss->in_handler = 1;

    /* Block the signals specified in sa_mask for this handler */
    uint32_t old_blocked = ss->blocked_mask;
    ss->blocked_mask |= ss->handler_flags[signo] ? ss->handler_flags[signo] : 0;
    /* Also block the current signal during handler execution */
    ss->blocked_mask |= (1U << signo);

    handler(signo);

    /* Restore blocked mask */
    ss->blocked_mask = old_blocked;
    ss->in_handler = 0;
}

int signal_has_pending(task_t* task) {
    if (!task) return 0;

    signal_state_t* ss = peek_signal_state(task);
    if (!ss) return 0;

    /* Any pending signal that is not blocked? */
    uint32_t deliverable = ss->pending_mask & ~ss->blocked_mask;
    return (deliverable != 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Signal Disposition                                                 */
/* ------------------------------------------------------------------ */

signal_handler_t signal_set_handler(int signo, signal_handler_t handler) {
    if (!signal_is_valid(signo)) return SIG_ERR;

    /* SIGKILL and SIGSTOP cannot have their handlers changed */
    if (signo == SIGKILL || signo == SIGSTOP) return SIG_ERR;

    task_t* cur = scheduler_get_current();
    if (!cur) return SIG_ERR;

    signal_state_t* ss = get_signal_state(cur);
    if (!ss) return SIG_ERR;

    signal_handler_t old = ss->handlers[signo];
    ss->handlers[signo] = handler;

    return old;
}

int signal_sigaction(int signo, const sigaction_t* act, sigaction_t* old_act) {
    if (!signal_is_valid(signo)) return -1;

    /* SIGKILL and SIGSTOP cannot be caught or ignored */
    if (signo == SIGKILL || signo == SIGSTOP) return -1;

    task_t* cur = scheduler_get_current();
    if (!cur) return -1;

    signal_state_t* ss = get_signal_state(cur);
    if (!ss) return -1;

    /* Save old action if requested */
    if (old_act) {
        old_act->sa_handler = ss->handlers[signo];
        old_act->sa_flags   = ss->handler_flags[signo];
        /* We don't have a separate per-signal saved mask; approximate with blocked_mask */
        old_act->sa_mask    = ss->blocked_mask;
    }

    /* Set new action if provided */
    if (act) {
        ss->handlers[signo]      = act->sa_handler;
        ss->handler_flags[signo] = act->sa_mask; /* Store sa_mask for use during delivery */
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Signal Mask                                                        */
/* ------------------------------------------------------------------ */

int signal_sigprocmask(int how, uint32_t* set, uint32_t* old_set) {
    task_t* cur = scheduler_get_current();
    if (!cur) return -1;

    signal_state_t* ss = get_signal_state(cur);
    if (!ss) return -1;

    /* Save old mask */
    if (old_set) {
        *old_set = ss->blocked_mask;
    }

    if (!set) return 0; /* Nothing to change */

    /* SIGKILL and SIGSTOP cannot be blocked — mask them out of any set */
    uint32_t unblockable = (1U << SIGKILL) | (1U << SIGSTOP);
    uint32_t safe_set = (*set) & ~unblockable;

    switch (how) {
        case SIG_BLOCK:
            ss->blocked_mask |= safe_set;
            break;
        case SIG_UNBLOCK:
            ss->blocked_mask &= ~safe_set;
            break;
        case SIG_SETMASK:
            ss->blocked_mask = safe_set;
            break;
        default:
            return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Default Signal Actions                                             */
/* ------------------------------------------------------------------ */

void signal_default_action(task_t* task, int signo) {
    if (!task) return;

    switch (signo) {
        /* ---- Signals that terminate the task ---- */
        case SIGHUP:
        case SIGINT:
        case SIGQUIT:
        case SIGILL:
        case SIGTRAP:
        case SIGABRT:
        case SIGBUS:
        case SIGFPE:
        case SIGKILL:
        case SIGSEGV:
        case SIGALRM:
        case SIGTERM: {
            s_printf("[SIGNAL] Default action: terminate task\n");
            task->state = TASK_STATE_ZOMBIE;
            scheduler_remove_task(task);

            /* Clean up signal state */
            if (task->id >= 0 && task->id < SIGNAL_MAX_TASKS && task_signals[task->id]) {
                signal_state_destroy(task_signals[task->id]);
                task_signals[task->id] = NULL;
            }
            break;
        }

        /* ---- Signals that stop the task ---- */
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU: {
            s_printf("[SIGNAL] Default action: stop task\n");
            /* Use a block reason that indicates stopped-by-signal */
            scheduler_block(signo);  /* Use signal number as reason */
            break;
        }

        /* ---- Signals that are ignored by default ---- */
        case SIGCHLD:
        case SIGWINCH:
        case SIGPIPE:
            /* Silently ignored */
            break;

        /* ---- SIGCONT: continue if stopped ---- */
        case SIGCONT: {
            if (task->state == TASK_STATE_BLOCKED) {
                scheduler_unblock(task);
                s_printf("[SIGNAL] SIGCONT: task continued\n");
            }
            break;
        }

        default:
            /* Unknown signal — default is to terminate */
            s_printf("[SIGNAL] Unknown signal: default terminate\n");
            task->state = TASK_STATE_ZOMBIE;
            scheduler_remove_task(task);

            if (task->id >= 0 && task->id < SIGNAL_MAX_TASKS && task_signals[task->id]) {
                signal_state_destroy(task_signals[task->id]);
                task_signals[task->id] = NULL;
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

int signal_is_valid(int signo) {
    return (signo >= 1 && signo <= 31);
}

const char* signal_name(int signo) {
    switch (signo) {
        case SIGHUP:   return "SIGHUP";
        case SIGINT:   return "SIGINT";
        case SIGQUIT:  return "SIGQUIT";
        case SIGILL:   return "SIGILL";
        case SIGTRAP:  return "SIGTRAP";
        case SIGABRT:  return "SIGABRT";
        case SIGBUS:   return "SIGBUS";
        case SIGFPE:   return "SIGFPE";
        case SIGKILL:  return "SIGKILL";
        case SIGUSR1:  return "SIGUSR1";
        case SIGSEGV:  return "SIGSEGV";
        case SIGUSR2:  return "SIGUSR2";
        case SIGPIPE:  return "SIGPIPE";
        case SIGALRM:  return "SIGALRM";
        case SIGTERM:  return "SIGTERM";
        case SIGCHLD:  return "SIGCHLD";
        case SIGCONT:  return "SIGCONT";
        case SIGSTOP:  return "SIGSTOP";
        case SIGTSTP:  return "SIGTSTP";
        case SIGTTIN:  return "SIGTTIN";
        case SIGTTOU:  return "SIGTTOU";
        case SIGWINCH: return "SIGWINCH";
        default:       return "SIGUNKNOWN";
    }
}
