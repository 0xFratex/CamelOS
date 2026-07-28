/**
 * crash.c - Crash Reporter Implementation for CamelOS
 *
 * Captures stack traces on kernel panics and process crashes, then
 * persists macOS-style diagnostic reports to the PFS32 filesystem.
 *
 * Architecture notes (32-bit x86):
 *   - Stack frames are linked via the EBP chain:
 *       [EBP+0] = saved previous EBP
 *       [EBP+4] = return address
 *   - We walk the chain until we hit NULL / invalid EBP or
 *     CRASH_LOG_MAX_STACK frames are collected.
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#include "crash.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"
#include "../fs/pfs32.h"
#include "../core/klog.h"

/* ------------------------------------------------------------------ */
/*  Internal state                                                    */
/* ------------------------------------------------------------------ */

/** Running count of crashes since boot — used for log filenames */
static uint32_t crash_count = 0;

/* ------------------------------------------------------------------ */
/*  Crash type strings                                                */
/* ------------------------------------------------------------------ */

static const char* crash_type_name(crash_type_t type) {
    switch (type) {
        case CRASH_KERNEL_PANIC:       return "KERNEL_PANIC";
        case CRASH_USER_FAULT:         return "USER_FAULT";
        case CRASH_STACK_OVERFLOW:     return "STACK_OVERFLOW";
        case CRASH_DIV_ZERO:           return "DIVIDE_BY_ZERO";
        case CRASH_GENERAL_PROTECTION: return "GENERAL_PROTECTION";
        default:                       return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/*  crash_reporter_init                                               */
/* ------------------------------------------------------------------ */

void crash_reporter_init(void) {
    /*
     * Create the directory tree for diagnostic reports.
     * pfs32_create_directory() will succeed silently if the directory
     * already exists, so creating each level is safe.
     */
    pfs32_create_directory("/Library");
    pfs32_create_directory("/Library/Logs");
    pfs32_create_directory("/Library/Logs/DiagnosticReports");

    klog_log(KLOG_INFO, "crash", "Crash reporter initialized; log dir /Library/Logs/DiagnosticReports/");
    s_printf("[CRASH] Reporter initialized\n");
}

/* ------------------------------------------------------------------ */
/*  crash_unwind_stack                                                */
/* ------------------------------------------------------------------ */

/**
 * Read a 32-bit value from a potentially unsafe address.
 * Returns 1 if the read succeeded (address passed basic sanity checks),
 * 0 otherwise.  The read value is stored in *out.
 *
 * In a hobby OS without a software MMU, we rely on heuristic checks:
 *   - Address must not be NULL
 *   - Address must be 4-byte aligned
 *   - Address must not be in the very low page (NULL-page neighborhood)
 *   - Address must not be 0xFFFFFFFF
 */
static int safe_read32(uint32_t addr, uint32_t* out) {
    if (addr == 0)              return 0;
    if (addr == 0xFFFFFFFF)     return 0;
    if (addr & 0x3)             return 0;   /* not 4-byte aligned */
    if (addr < 0x1000)          return 0;   /* NULL-page guard     */

    /*
     * Volatile read — the compiler must not optimise this away.
     * If the address happens to be unmapped we'll still fault, but
     * the heuristics above catch the common bad cases.
     */
    *out = *(volatile uint32_t*)addr;
    return 1;
}

int crash_unwind_stack(uint32_t ebp, uint32_t* frames, int max_frames) {
    int count = 0;
    uint32_t current_ebp = ebp;

    /*
     * Walk the linked list of stack frames.
     *
     * Stack layout at each frame:
     *   [current_ebp + 0]  ->  previous EBP  (link to caller's frame)
     *   [current_ebp + 4]  ->  return address (saved EIP of caller)
     *
     * Stop conditions:
     *   1. Collected max_frames frames
     *   2. EBP is NULL / invalid (end of chain)
     *   3. EBP stops advancing (corrupted frame loop)
     */
    while (count < max_frames) {
        /* Validate the current frame pointer */
        if (current_ebp == 0)          break;
        if (current_ebp == 0xFFFFFFFF) break;
        if (current_ebp & 0x3)         break;   /* misaligned */
        if (current_ebp < 0x1000)      break;   /* NULL page  */

        /* Read return address at [EBP + 4] */
        uint32_t ret_addr;
        if (!safe_read32(current_ebp + 4, &ret_addr))
            break;

        /* A return address of 0 usually means end of call chain */
        if (ret_addr == 0)
            break;

        frames[count++] = ret_addr;

        /* Read previous EBP at [EBP + 0] to continue the walk */
        uint32_t prev_ebp;
        if (!safe_read32(current_ebp, &prev_ebp))
            break;

        /* Guard against infinite loops (corrupted frame pointer) */
        if (prev_ebp <= current_ebp)
            break;

        current_ebp = prev_ebp;
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  crash_report                                                      */
/* ------------------------------------------------------------------ */

void crash_report(crash_type_t type, const char* msg, registers_t* regs,
                  const char* proc_name, int pid) {
    /*
     * Emit an early serial notification so the developer sees the
     * crash immediately, even if the log-file write fails.
     */
    s_printf("[CRASH] === Crash reported ===\n");
    s_printf("[CRASH] Type: %s\n", crash_type_name(type));

    /*
     * Allocate and zero-fill the crash log structure.
     * Using kzalloc ensures all fields (especially the stack_trace
     * and message arrays) start at zero / empty.
     */
    crash_log_t* log = (crash_log_t*)kzalloc(sizeof(crash_log_t));
    if (!log) {
        s_printf("[CRASH] FATAL: cannot allocate crash_log_t\n");
        klog_log(KLOG_CRIT, "crash", "Crash reporter OOM — log dropped");
        return;
    }

    /* ---- Fill in the crash type ---- */
    log->crash_type = type;

    /* ---- Copy the message ---- */
    if (msg) {
        strncpy(log->message, msg, CRASH_LOG_MAX_MSG - 1);
        log->message[CRASH_LOG_MAX_MSG - 1] = '\0';
    } else {
        strncpy(log->message, "(no message)", CRASH_LOG_MAX_MSG - 1);
    }

    /* ---- Copy register state ---- */
    if (regs) {
        log->eip      = regs->eip;
        log->esp      = regs->esp;
        log->ebp      = regs->ebp;
        log->eax      = regs->eax;
        log->ebx      = regs->ebx;
        log->ecx      = regs->ecx;
        log->edx      = regs->edx;
        log->esi      = regs->esi;
        log->edi      = regs->edi;
        log->cs       = regs->cs;
        log->ds       = regs->ds;
        log->err_code = regs->err_code;
    }

    /* ---- Timestamp ---- */
    log->timestamp = pfs32_time_now();

    /* ---- Process info ---- */
    if (proc_name) {
        strncpy(log->process_name, proc_name, CRASH_LOG_MAX_PROC - 1);
        log->process_name[CRASH_LOG_MAX_PROC - 1] = '\0';
    } else {
        strncpy(log->process_name, "unknown", CRASH_LOG_MAX_PROC - 1);
    }
    log->process_pid = pid;

    /* ---- Unwind the stack ---- */
    uint32_t start_ebp = regs ? regs->ebp : 0;
    int nframes = crash_unwind_stack(start_ebp, log->stack_trace,
                                     CRASH_LOG_MAX_STACK);

    /* ---- Log via klog ---- */
    klog_log(KLOG_CRIT, "crash", "Crash: %s in %s[%d] @ EIP=0x%x (%d frames)",
             crash_type_name(type), log->process_name, pid,
             regs ? regs->eip : 0, nframes);

    /* ---- Persist to disk ---- */
    crash_save_log(log);

    /* ---- Increment crash counter ---- */
    crash_count++;

    /* ---- Free the log structure ---- */
    kfree(log);
}

/* ------------------------------------------------------------------ */
/*  crash_save_log                                                    */
/* ------------------------------------------------------------------ */

/**
 * Helper: append a string to the buffer at the current offset.
 * Returns the new offset.  Will not write past buf_size.
 */
static int buf_append(char* buf, int offset, int buf_size, const char* str) {
    if (!str || offset >= buf_size - 1)
        return offset;

    int slen = (int)strlen(str);
    int remaining = buf_size - offset - 1;
    if (remaining <= 0)
        return offset;

    int to_copy = slen < remaining ? slen : remaining;
    memcpy(buf + offset, str, to_copy);
    offset += to_copy;
    buf[offset] = '\0';

    return offset;
}

/**
 * Helper: append a formatted 32-bit hex value  "0x0000ABCD"
 */
static int buf_append_hex(char* buf, int offset, int buf_size, uint32_t value) {
    char tmp[16];
    tmp[0] = '0'; tmp[1] = 'x';
    int_to_hex(value, tmp + 2);
    return buf_append(buf, offset, buf_size, tmp);
}

/**
 * Helper: append a formatted decimal integer
 */
static int buf_append_int(char* buf, int offset, int buf_size, int value) {
    char tmp[16];
    int_to_str(value, tmp);
    return buf_append(buf, offset, buf_size, tmp);
}

void crash_save_log(crash_log_t* log) {
    /*
     * We build the entire log in memory first, then write it to disk
     * in a single pfs32_write_file() call.  This minimises the number
     * of filesystem operations during a crash (when the system may be
     * in an unstable state).
     *
     * The buffer is sized generously at 4096 bytes — more than enough
     * for a 16-frame stack trace plus register dump.
     */
    const int BUF_SIZE = 4096;
    char* buf = (char*)kmalloc(BUF_SIZE);
    if (!buf) {
        s_printf("[CRASH] Cannot allocate log buffer\n");
        return;
    }

    int pos = 0;
    buf[0] = '\0';

    /* ---- Header (macOS crash report style) ---- */
    pos = buf_append(buf, pos, BUF_SIZE,
        "============================================================\n");
    pos = buf_append(buf, pos, BUF_SIZE,
        "CamelOS Crash Report\n");
    pos = buf_append(buf, pos, BUF_SIZE,
        "============================================================\n\n");

    /* ---- Incident metadata ---- */
    pos = buf_append(buf, pos, BUF_SIZE, "Incident Identifier: crash_");
    /* Zero-padded 4-digit crash number */
    char num_buf[8];
    uint32_t id = crash_count + 1;   /* 1-based in the file */
    num_buf[0] = '0' + (char)((id / 1000) % 10);
    num_buf[1] = '0' + (char)((id / 100)  % 10);
    num_buf[2] = '0' + (char)((id / 10)   % 10);
    num_buf[3] = '0' + (char)(id % 10);
    num_buf[4] = '\0';
    pos = buf_append(buf, pos, BUF_SIZE, num_buf);
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "Process:         ");
    pos = buf_append(buf, pos, BUF_SIZE, log->process_name);
    pos = buf_append(buf, pos, BUF_SIZE, " [");
    pos = buf_append_int(buf, pos, BUF_SIZE, log->process_pid);
    pos = buf_append(buf, pos, BUF_SIZE, "]\n");

    pos = buf_append(buf, pos, BUF_SIZE, "Crash Type:      ");
    pos = buf_append(buf, pos, BUF_SIZE, crash_type_name(log->crash_type));
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "Date/Time:       tick ");
    pos = buf_append_int(buf, pos, BUF_SIZE, (int)log->timestamp);
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "OS Version:      CamelOS 1.0\n\n");

    /* ---- Exception information ---- */
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");
    pos = buf_append(buf, pos, BUF_SIZE, "Exception Information\n");
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");

    pos = buf_append(buf, pos, BUF_SIZE, "Exception Type:  ");
    pos = buf_append(buf, pos, BUF_SIZE, crash_type_name(log->crash_type));
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "Error Code:      ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->err_code);
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "Message:         ");
    pos = buf_append(buf, pos, BUF_SIZE, log->message);
    pos = buf_append(buf, pos, BUF_SIZE, "\n\n");

    /* ---- Register dump ---- */
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");
    pos = buf_append(buf, pos, BUF_SIZE, "Register Dump\n");
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");

    pos = buf_append(buf, pos, BUF_SIZE, "EAX: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->eax);
    pos = buf_append(buf, pos, BUF_SIZE, "  EBX: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->ebx);
    pos = buf_append(buf, pos, BUF_SIZE, "  ECX: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->ecx);
    pos = buf_append(buf, pos, BUF_SIZE, "  EDX: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->edx);
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "ESI: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->esi);
    pos = buf_append(buf, pos, BUF_SIZE, "  EDI: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->edi);
    pos = buf_append(buf, pos, BUF_SIZE, "  EBP: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->ebp);
    pos = buf_append(buf, pos, BUF_SIZE, "  ESP: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->esp);
    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    pos = buf_append(buf, pos, BUF_SIZE, "EIP: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->eip);
    pos = buf_append(buf, pos, BUF_SIZE, "  CS:  ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->cs);
    pos = buf_append(buf, pos, BUF_SIZE, "  DS:  ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->ds);
    pos = buf_append(buf, pos, BUF_SIZE, "  Err: ");
    pos = buf_append_hex(buf, pos, BUF_SIZE, log->err_code);
    pos = buf_append(buf, pos, BUF_SIZE, "\n\n");

    /* ---- Stack trace ---- */
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");
    pos = buf_append(buf, pos, BUF_SIZE, "Stack Trace (Thread 0 Crashed)\n");
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");

    int frame_count = 0;
    for (int i = 0; i < CRASH_LOG_MAX_STACK; i++) {
        if (log->stack_trace[i] == 0)
            break;
        frame_count++;
    }

    if (frame_count == 0) {
        pos = buf_append(buf, pos, BUF_SIZE, "  (no stack frames captured)\n");
    } else {
        for (int i = 0; i < frame_count; i++) {
            /* Frame index: right-aligned 2 digits */
            pos = buf_append(buf, pos, BUF_SIZE, "  ");
            pos = buf_append_int(buf, pos, BUF_SIZE, i);
            pos = buf_append(buf, pos, BUF_SIZE, "  ");

            /* Symbol name placeholder — CamelOS does not yet have a
             * symbol table, so we print the raw address. */
            pos = buf_append_hex(buf, pos, BUF_SIZE, log->stack_trace[i]);

            pos = buf_append(buf, pos, BUF_SIZE, "  (no symbol)\n");
        }
    }

    pos = buf_append(buf, pos, BUF_SIZE, "\n");

    /* ---- Footer ---- */
    pos = buf_append(buf, pos, BUF_SIZE,
        "------------------------------------------------------------\n");
    pos = buf_append(buf, pos, BUF_SIZE, "End of crash report\n");
    pos = buf_append(buf, pos, BUF_SIZE,
        "============================================================\n");

    /* ---- Construct file path ---- */
    char filepath[96];
    strcpy(filepath, "/Library/Logs/DiagnosticReports/crash_");
    /* Zero-padded 4-digit crash number */
    uint32_t fid = crash_count + 1;
    char fid_buf[8];
    fid_buf[0] = '0' + (char)((fid / 1000) % 10);
    fid_buf[1] = '0' + (char)((fid / 100)  % 10);
    fid_buf[2] = '0' + (char)((fid / 10)   % 10);
    fid_buf[3] = '0' + (char)(fid % 10);
    fid_buf[4] = '\0';
    strcat(filepath, fid_buf);
    strcat(filepath, ".log");

    /* ---- Write to filesystem ---- */
    int result = pfs32_write_file(filepath, (uint8_t*)buf, (uint32_t)pos);
    if (result == PFS_OK) {
        s_printf("[CRASH] Log written to %s\n", filepath);
        klog_log(KLOG_INFO, "crash", "Crash log saved to %s", filepath);
    } else {
        s_printf("[CRASH] FAILED to write log (err=%d)\n", result);
        klog_log(KLOG_ERROR, "crash", "Failed to write crash log: err=%d", result);
    }

    kfree(buf);
}
