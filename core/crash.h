/**
 * crash.h - Crash Reporter for CamelOS
 *
 * Captures stack traces on kernel panics and process crashes, writing
 * structured crash logs to /Library/Logs/DiagnosticReports/ in a format
 * inspired by macOS crash reports.
 *
 * Usage:
 *   crash_reporter_init();   // Call once during boot
 *   crash_report(CRASH_KERNEL_PANIC, "unrecoverable error", &regs, "kernel", 0);
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#ifndef CRASH_H
#define CRASH_H

#include "../include/types.h"
#include "../hal/cpu/isr.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

/** Maximum number of stack frames captured in a crash log */
#define CRASH_LOG_MAX_STACK   16

/** Maximum length of the human-readable crash message */
#define CRASH_LOG_MAX_MSG     256

/** Maximum length of a process name stored in crash_log_t */
#define CRASH_LOG_MAX_PROC    32

/* ------------------------------------------------------------------ */
/*  Crash types                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    CRASH_KERNEL_PANIC        = 0,  /* Unrecoverable kernel fault        */
    CRASH_USER_FAULT          = 1,  /* User-space segfault / access violation */
    CRASH_STACK_OVERFLOW      = 2,  /* Stack guard page hit              */
    CRASH_DIV_ZERO            = 3,  /* Division by zero (int_no 0)       */
    CRASH_GENERAL_PROTECTION  = 4,  /* General protection fault (#GP)    */
} crash_type_t;

/* ------------------------------------------------------------------ */
/*  Crash log structure                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    crash_type_t  crash_type;                            /* Type of crash          */
    char          message[CRASH_LOG_MAX_MSG];            /* Human-readable message */

    /* ---- Register dump at crash time ---- */
    uint32_t      eip;
    uint32_t      esp;
    uint32_t      ebp;
    uint32_t      eax;
    uint32_t      ebx;
    uint32_t      ecx;
    uint32_t      edx;
    uint32_t      esi;
    uint32_t      edi;
    uint32_t      cs;
    uint32_t      ds;
    uint32_t      err_code;

    /* ---- Stack trace (return addresses) ---- */
    uint32_t      stack_trace[CRASH_LOG_MAX_STACK];

    /* ---- Metadata ---- */
    uint32_t      timestamp;                             /* Tick count at crash    */
    char          process_name[CRASH_LOG_MAX_PROC];      /* Crashing process name  */
    int           process_pid;                           /* Crashing process PID   */
} crash_log_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * crash_reporter_init - Initialize the crash reporter subsystem.
 *
 * Creates the /Library/Logs/DiagnosticReports/ directory tree on the
 * PFS32 filesystem.  Must be called once during boot after pfs32_init().
 */
void crash_reporter_init(void);

/**
 * crash_report - Capture and persist a crash report.
 *
 * @type:      Classification of the crash
 * @msg:       Human-readable description (may be NULL)
 * @regs:      CPU register state at the time of the crash (may be NULL)
 * @proc_name: Name of the crashing process (may be NULL for kernel)
 * @pid:       PID of the crashing process (0 for kernel)
 *
 * Builds a crash_log_t, unwinds the stack, writes the log to disk
 * via pfs32_write_file(), and emits a KLOG_CRIT message.
 */
void crash_report(crash_type_t type, const char* msg, registers_t* regs,
                  const char* proc_name, int pid);

/**
 * crash_unwind_stack - Walk the x86 stack frame chain.
 *
 * @ebp:        Starting frame pointer value
 * @frames:     Output array for captured return addresses
 * @max_frames: Capacity of @frames (typically CRASH_LOG_MAX_STACK)
 * @return:     Number of frames actually captured
 *
 * Each x86 stack frame is laid out as:
 *   [prev_ebp][return_address]
 * At address EBP:   prev_ebp  (saved frame pointer of caller)
 * At address EBP+4: return_address (where caller will resume)
 *
 * The walk stops when a NULL or obviously invalid EBP is encountered,
 * or when @max_frames frames have been collected.
 */
int crash_unwind_stack(uint32_t ebp, uint32_t* frames, int max_frames);

/**
 * crash_save_log - Format and write a crash log to disk.
 *
 * @log:  Fully populated crash_log_t to persist
 *
 * Writes a macOS-style crash report to
 * /Library/Logs/DiagnosticReports/crash_XXXX.log where XXXX is a
 * zero-padded sequential crash counter.
 */
void crash_save_log(crash_log_t* log);

#endif /* CRASH_H */
