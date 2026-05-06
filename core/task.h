#ifndef TASK_H
#define TASK_H

#include "../hal/cpu/isr.h"

/* Task states - must match scheduler.h task_state_t */
#ifndef TASK_STATE_DEFINED
#define TASK_STATE_DEFINED
#define TASK_STATE_READY     0
#define TASK_STATE_RUNNING   1
#define TASK_STATE_BLOCKED   2
#define TASK_STATE_ZOMBIE    3
#define TASK_STATE_SLEEPING  4
#endif

typedef struct task_control_block {
    int id;                /* Process ID */
    int uid;               /* User ID (0=Root, 1000=User) */
    uint32_t esp;          /* Stack Pointer */
    struct task_control_block* next;
    int state;             /* Task state (see TASK_STATE_* defines) */
    char name[32];
    int is_app_bundle;     /* 1 if running from .app */
    
    /* Scheduler fields */
    uint8_t priority;      /* Priority level (0-255, 0 = highest) */
    uint32_t time_slice;   /* Remaining time quantum in ticks */
    uint32_t time_used;    /* Total CPU time used in ticks */
    uint32_t sleep_until;  /* Tick count to wake up (for sleeping tasks) */
    int block_reason;      /* Why task is blocked (0 = not blocked) */

    /* Process resources (VMM, signals, pipes) */
    void* address_space;   /* address_space_t* - per-process virtual memory (vmm.h) */
    void* signal_state;    /* signal_state_t* - per-process signal handling (signal.h) */
    int exit_code;         /* Exit code for zombie processes */
    int parent_pid;        /* Parent process ID for waitpid */
    char cwd[256];         /* Per-process current working directory (default "/") */
} task_t;

/* Task function prototype */
typedef void (*task_func_t)(void);

/* Function declarations */
task_t* create_task(int id, uint32_t entry_point, uint32_t stack_top);
void create_user_task(void (*entry)(), const char* name, int uid, int is_app);

// Task 7: Create a Ring 3 (user-mode) task
// Returns a task whose context frame is set up for Ring 3 return:
//   CS = 0x18|3, DS=ES=FS=GS=SS = 0x20|3, EFLAGS with IOPL=0
task_t* task_create_user(const char* name, void* entry_point, void* stack_top);

void task_switch(void);
void task_exit(void);
int get_current_uid(void);
void set_current_uid(int uid);

/* External reference to current task */
extern task_t* current_task;

#endif /* TASK_H */
