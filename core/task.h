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
} task_t;

/* Task function prototype */
typedef void (*task_func_t)(void);

/* Function declarations */
task_t* create_task(int id, uint32_t entry_point, uint32_t stack_top);
void create_user_task(void (*entry)(), const char* name, int uid, int is_app);
void task_switch(void);
void task_exit(void);

/* External reference to current task */
extern task_t* current_task;

#endif /* TASK_H */
