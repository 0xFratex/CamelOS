#ifndef SIGNAL_H
#define SIGNAL_H

#include "../include/types.h"
#include "task.h"

/* POSIX signal numbers */
#define SIGHUP     1    /* Hangup */
#define SIGINT     2    /* Interrupt (Ctrl+C) */
#define SIGQUIT    3    /* Quit (Ctrl+\) */
#define SIGILL     4    /* Illegal instruction */
#define SIGTRAP    5    /* Trace/breakpoint trap */
#define SIGABRT    6    /* Abort */
#define SIGBUS     7    /* Bus error */
#define SIGFPE     8    /* Floating point exception */
#define SIGKILL    9    /* Kill (cannot be caught/ignored) */
#define SIGUSR1    10   /* User-defined signal 1 */
#define SIGSEGV    11   /* Segmentation violation */
#define SIGUSR2    12   /* User-defined signal 2 */
#define SIGPIPE    13   /* Broken pipe */
#define SIGALRM    14   /* Alarm clock */
#define SIGTERM    15   /* Termination */
#define SIGCHLD    17   /* Child status changed */
#define SIGCONT    18   /* Continue (cannot be stopped) */
#define SIGSTOP    19   /* Stop (cannot be caught/ignored) */
#define SIGTSTP    20   /* Terminal stop */
#define SIGTTIN    21   /* Background read from tty */
#define SIGTTOU    22   /* Background write to tty */
#define SIGWINCH   28   /* Window size change */

#define SIG_DFL    ((signal_handler_t)0)   /* Default handler */
#define SIG_IGN    ((signal_handler_t)1)   /* Ignore signal */
#define SIG_ERR    ((signal_handler_t)-1)  /* Error return */

#define MAX_SIGNALS     32
#define SIG_NO_QUEUE    64   /* Max pending signals per process */

/* Signal handler function pointer */
typedef void (*signal_handler_t)(int sig);

/* Signal action flags */
#define SA_NOCLDSTOP    0x01   /* Don't send SIGCHLD when children stop */
#define SA_RESTART      0x02   /* Restart interrupted syscalls */
#define SA_ONSTACK      0x04   /* Use alternate signal stack */

/* sigaction structure */
typedef struct {
    signal_handler_t sa_handler;    /* Signal handler */
    uint32_t sa_flags;              /* Flags */
    uint32_t sa_mask;               /* Signal mask to apply during handler */
} sigaction_t;

/* Pending signal info for a process */
typedef struct siginfo {
    int signo;                   /* Signal number */
    int sender_pid;              /* PID of sender (0 = kernel) */
    int code;                    /* Signal code (SI_USER, SI_KERNEL, etc.) */
    uint32_t value;              /* Signal value */
} siginfo_t;

/* Signal codes */
#define SI_USER     0   /* Sent by kill() */
#define SI_KERNEL   1   /* Sent by kernel */
#define SI_TIMER    2   /* Sent by timer */
#define SI_QUEUE    3   /* Sent by sigqueue() */

/* Per-process signal state (embedded in task_t or pointed to) */
typedef struct signal_state {
    uint32_t pending_mask;           /* Bitmask of pending signals */
    uint32_t blocked_mask;           /* Bitmask of blocked signals */
    signal_handler_t handlers[MAX_SIGNALS];  /* Handler for each signal */
    uint32_t handler_flags[MAX_SIGNALS];     /* Flags for each handler */
    siginfo_t pending_queue[SIG_NO_QUEUE];   /* Pending signal queue */
    int pending_count;               /* Number of pending signals */
    int in_handler;                  /* Recursion guard */
} signal_state_t;

/* === Initialization === */
void signal_init(void);
signal_state_t* signal_state_create(void);
void signal_state_destroy(signal_state_t* ss);

/* === Signal Delivery === */
int signal_send(int target_pid, int signo, int code, uint32_t value);
int signal_send_to_task(task_t* task, int signo, int code, uint32_t value);
void signal_send_kernel(task_t* task, int signo);

/* === Signal Handling (called from scheduler before returning to userspace) === */
void signal_check_pending(task_t* task);
int signal_has_pending(task_t* task);

/* === Signal Disposition === */
signal_handler_t signal_set_handler(int signo, signal_handler_t handler);
int signal_sigaction(int signo, const sigaction_t* act, sigaction_t* old_act);

/* === Signal Mask === */
int signal_sigprocmask(int how, uint32_t* set, uint32_t* old_set);
#define SIG_BLOCK      0
#define SIG_UNBLOCK    1
#define SIG_SETMASK    2

/* === Default Signal Actions === */
void signal_default_action(task_t* task, int signo);

/* === Utility === */
const char* signal_name(int signo);
int signal_is_valid(int signo);

#endif /* SIGNAL_H */
