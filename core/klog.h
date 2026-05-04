/**
 * klog.h - Kernel Logging System for CamelOS
 *
 * Provides a structured, level-filtered logging subsystem with a fixed-size
 * ring buffer, configurable output routing (serial / VGA), and convenience
 * macros that capture the source file automatically.
 *
 * Usage:
 *   klog_init();                              // Call once during boot
 *   KLOG_INFO("Heap ready, %u bytes", size);  // Convenience macro
 *   klog_dump_to_serial();                    // Dump buffer at panic time
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#ifndef KLOG_H
#define KLOG_H

#include "../include/types.h"

/* ------------------------------------------------------------------ */
/*  Log levels (lower value = more critical / more verbose)           */
/* ------------------------------------------------------------------ */
typedef enum {
    KLOG_DEBUG   = 0,    /* Verbose debugging information               */
    KLOG_INFO    = 1,    /* General informational messages               */
    KLOG_WARN    = 2,    /* Warning conditions                          */
    KLOG_ERROR   = 3,    /* Error conditions                            */
    KLOG_CRIT    = 4,    /* Critical - system may be unstable           */
    KLOG_NONE    = 5,    /* Suppress all logging                        */
} klog_level_t;

/* ------------------------------------------------------------------ */
/*  Ring-buffer entry                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t      timestamp;     /* Tick count when logged             */
    klog_level_t  level;         /* Severity level                     */
    const char*   source;        /* Source file/module (static string) */
    char          message[192];  /* Formatted message text             */
} klog_entry_t;

/* ------------------------------------------------------------------ */
/*  Statistics                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t total_logged;       /* Total messages logged              */
    uint32_t dropped;            /* Messages dropped (buffer full)     */
    uint32_t by_level[6];        /* Count per log level (0..5)         */
} klog_stats_t;

/* ------------------------------------------------------------------ */
/*  Configuration constants                                           */
/* ------------------------------------------------------------------ */
#define KLOG_RING_SIZE      256     /* Number of entries in ring buffer  */
#define KLOG_MSG_SIZE       192     /* Max message length per entry      */

/* ================================================================== */
/*  Initialization                                                    */
/* ================================================================== */

/**
 * klog_init - Initialize the kernel logging subsystem.
 *
 * Zeros the ring buffer, sets default level to KLOG_INFO,
 * enables serial output, disables VGA output, and clears statistics.
 * Must be called once before any klog_log() / KLOG_* usage.
 */
void klog_init(void);

/* ================================================================== */
/*  Core logging functions                                            */
/* ================================================================== */

/**
 * klog_log - Log a formatted message at the given level.
 *
 * @level:  Severity (KLOG_DEBUG .. KLOG_CRIT)
 * @source: Originating file/module name (must be a static string)
 * @fmt:    printf-style format string
 *          Supported: %s %d %u %x %X %p %c %% with width/flag specifiers
 *
 * The message is stored in the ring buffer regardless of the current
 * minimum level.  Serial/VGA output is produced only if the message
 * level >= the configured minimum and the respective output is enabled.
 */
void klog_log(klog_level_t level, const char* source, const char* fmt, ...);

/**
 * klog_log_raw - Log a pre-formatted string (no printf processing).
 *
 * Useful when the caller has already formatted the message or wants
 * to avoid the overhead of format parsing.
 */
void klog_log_raw(klog_level_t level, const char* source, const char* message);

/* ================================================================== */
/*  Convenience macros (capture __FILE__ automatically)               */
/* ================================================================== */

#define KLOG_DBG(fmt, ...)  klog_log(KLOG_DEBUG, __FILE__, fmt, ##__VA_ARGS__)
#define KLOG_INFO(fmt, ...) klog_log(KLOG_INFO,  __FILE__, fmt, ##__VA_ARGS__)
#define KLOG_WARN(fmt, ...) klog_log(KLOG_WARN,  __FILE__, fmt, ##__VA_ARGS__)
#define KLOG_ERR(fmt, ...)  klog_log(KLOG_ERROR, __FILE__, fmt, ##__VA_ARGS__)
#define KLOG_CRIT(fmt, ...) klog_log(KLOG_CRIT,  __FILE__, fmt, ##__VA_ARGS__)

/* ================================================================== */
/*  Configuration                                                     */
/* ================================================================== */

/** Set the minimum log level. Messages below this level are discarded. */
void klog_set_level(klog_level_t level);

/** Return the current minimum log level. */
klog_level_t klog_get_level(void);

/** Enable (1) or disable (0) serial port output. */
void klog_set_serial_output(int enabled);

/** Enable (1) or disable (0) VGA console output. */
void klog_set_vga_output(int enabled);

/* ================================================================== */
/*  Query / Retrieval                                                 */
/* ================================================================== */

/** Return the number of entries currently stored in the ring buffer. */
uint32_t klog_get_count(void);

/**
 * klog_get_entry - Retrieve a single entry by absolute index.
 *
 * @index: 0-based index into the logical log stream
 * @out:   Destination for the copied entry
 * Return: 0 on success, -1 if index is out of range
 */
int klog_get_entry(uint32_t index, klog_entry_t* out);

/**
 * klog_get_recent - Retrieve the N most recent entries.
 *
 * @count:        Desired number of entries
 * @out:          Destination array (caller-allocated, at least @count entries)
 * @actual_count: Set to the number of entries actually written
 * Return: 0 on success, -1 on error
 */
int klog_get_recent(uint32_t count, klog_entry_t* out, uint32_t* actual_count);

/** Clear all entries from the ring buffer and reset statistics. */
void klog_clear(void);

/* ================================================================== */
/*  Statistics                                                        */
/* ================================================================== */

/** Return a pointer to the live statistics structure (do NOT free). */
klog_stats_t* klog_get_stats(void);

/* ================================================================== */
/*  Dump / Display                                                    */
/* ================================================================== */

/** Dump the entire ring buffer to the serial port, oldest first. */
void klog_dump_to_serial(void);

/** Dump the entire ring buffer to the VGA console, oldest first. */
void klog_dump_to_vga(void);

/* ================================================================== */
/*  Early boot                                                        */
/* ================================================================== */

/**
 * klog_early_write - Write a raw string to serial before klog_init().
 *
 * Safe to call at any point, even before the ring buffer is initialized.
 * Simply writes directly to the serial port via serial_write_string().
 */
void klog_early_write(const char* message);

#endif /* KLOG_H */
