/**
 * klog.c - Kernel Logging System implementation for CamelOS
 *
 * Ring-buffer based structured logger with level filtering, dual output
 * (serial + VGA), spinlock-protected buffer access, and a custom
 * vsnprintf that supports common format specifiers.
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#include "klog.h"
#include "string.h"       /* strlen, strcpy, strncpy */
#include "memory.h"       /* memset */
#include "../hal/drivers/serial.h"  /* s_printf, serial_write_string, write_serial */
#include "../hal/drivers/vga.h"     /* vga_print, vga_mute_log */

/* va_list / va_start / va_arg / va_end are provided by include/string.h */

/* ================================================================== */
/*  Timer ticks                                                       */
/* ================================================================== */

extern volatile uint32_t ticks;

/* ================================================================== */
/*  Spinlock (simple test-and-set, safe for preemptive multitasking)  */
/* ================================================================== */

static volatile int klog_lock = 0;

/**
 * Spin until we acquire the lock.
 * Uses GCC atomic builtin for correct test-and-set semantics.
 */
static void klog_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&klog_lock, 1)) {
        /* Spin - could insert asm volatile("pause") here for x86 */
        asm volatile("pause");
    }
}

/** Release the spinlock. */
static void klog_lock_release(void)
{
    __sync_lock_release(&klog_lock);
}

/* ================================================================== */
/*  Level name strings (fixed-width for aligned serial output)        */
/* ================================================================== */

static const char* level_tags[] = {
    "DEBUG",    /* KLOG_DEBUG  = 0 */
    "INFO ",    /* KLOG_INFO   = 1 */
    "WARN ",    /* KLOG_WARN   = 2 */
    "ERROR",    /* KLOG_ERROR  = 3 */
    "CRIT ",    /* KLOG_CRIT   = 4 */
    "NONE ",    /* KLOG_NONE   = 5 */
};

/* ================================================================== */
/*  Ring buffer state                                                 */
/* ================================================================== */

static klog_entry_t  ring[KLOG_RING_SIZE];
static uint32_t      ring_head  = 0;   /* Next write position         */
static uint32_t      ring_tail  = 0;   /* Oldest valid entry          */
static uint32_t      ring_count = 0;   /* Number of valid entries     */

/* ================================================================== */
/*  Configuration state                                               */
/* ================================================================== */

static klog_level_t  min_level       = KLOG_INFO;
static int           serial_enabled  = 1;
static int           vga_enabled     = 0;
static int           initialized     = 0;

/* ================================================================== */
/*  Statistics                                                        */
/* ================================================================== */

static klog_stats_t  stats;

/* ================================================================== */
/*  Internal vsnprintf                                                */
/* ================================================================== */

/**
 * klog_vsnprintf - Minimal vsnprintf for kernel logging.
 *
 * Supported conversions:
 *   %s   - string          (NULL -> "(null)")
 *   %d   - signed decimal
 *   %u   - unsigned decimal
 *   %x   - hexadecimal lowercase
 *   %X   - hexadecimal uppercase
 *   %p   - pointer (0x-prefixed lowercase hex)
 *   %c   - character
 *   %%   - literal percent
 *
 * Supported flags / width (between % and specifier):
 *   '-'  - left-justify within field
 *   '0'  - pad with zeros
 *   N    - minimum field width (decimal digits)
 *
 * Always null-terminates the output (if size > 0).
 * Returns the number of characters that would have been written
 * (excluding the null terminator), like standard vsnprintf.
 */
static int klog_vsnprintf(char* buf, size_t size, const char* fmt, va_list args)
{
    char*       out   = buf;
    char*       end   = buf + size - 1;   /* Reserve space for '\0' */
    const char* p     = fmt;

    /* Early out for zero-size buffer */
    if (size == 0) {
        /* Count characters that would be produced */
        int count = 0;
        while (*p) {
            if (*p == '%') {
                p++;
                /* Skip flags */
                while (*p == '-' || *p == '0') p++;
                /* Skip width */
                while (*p >= '0' && *p <= '9') p++;
                /* Handle specifier */
                switch (*p) {
                    case 's': va_arg(args, const char*); count += 6; break; /* approx */
                    case 'd': va_arg(args, int); count += 11; break;
                    case 'u': va_arg(args, unsigned int); count += 10; break;
                    case 'x':
                    case 'X': va_arg(args, unsigned int); count += 8; break;
                    case 'p': va_arg(args, void*); count += 10; break;
                    case 'c': va_arg(args, int); count++; break;
                    case '%': count++; break;
                    default:  count++; break;
                }
            } else {
                count++;
            }
            p++;
        }
        return count;
    }

    while (*p && out < end) {
        if (*p != '%') {
            *out++ = *p++;
            continue;
        }

        p++;    /* Skip '%' */

        /* ----- Parse flags ----- */
        int left_justify = 0;
        int zero_pad     = 0;

        while (*p == '-' || *p == '0') {
            if (*p == '-') left_justify = 1;
            if (*p == '0') zero_pad = 1;
            p++;
        }

        /* Left-justify overrides zero-padding */
        if (left_justify) zero_pad = 0;

        /* ----- Parse width ----- */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* ----- Temp buffer for conversion result ----- */
        char tmp[32];
        int  tmp_len = 0;

        /* ----- Handle specifier ----- */
        switch (*p) {

        /* ---- %s : string ---- */
        case 's': {
            const char* s = va_arg(args, const char*);
            if (!s) s = "(null)";
            tmp_len = 0;
            while (s[tmp_len] && tmp_len < (int)sizeof(tmp) - 1) {
                tmp[tmp_len] = s[tmp_len];
                tmp_len++;
            }
            break;
        }

        /* ---- %d : signed decimal ---- */
        case 'd': {
            int val = va_arg(args, int);
            unsigned int uval;
            int neg = 0;
            if (val < 0) {
                neg = 1;
                uval = (unsigned int)(-(val + 1)) + 1u; /* Handle INT_MIN safely */
            } else {
                uval = (unsigned int)val;
            }
            tmp_len = 0;
            if (uval == 0) {
                tmp[tmp_len++] = '0';
            } else {
                while (uval > 0) {
                    tmp[tmp_len++] = '0' + (uval % 10);
                    uval /= 10;
                }
            }
            if (neg) tmp[tmp_len++] = '-';
            /* Reverse */
            for (int i = 0; i < tmp_len / 2; i++) {
                char t = tmp[i];
                tmp[i] = tmp[tmp_len - 1 - i];
                tmp[tmp_len - 1 - i] = t;
            }
            break;
        }

        /* ---- %u : unsigned decimal ---- */
        case 'u': {
            unsigned int uval = va_arg(args, unsigned int);
            tmp_len = 0;
            if (uval == 0) {
                tmp[tmp_len++] = '0';
            } else {
                while (uval > 0) {
                    tmp[tmp_len++] = '0' + (uval % 10);
                    uval /= 10;
                }
            }
            /* Reverse */
            for (int i = 0; i < tmp_len / 2; i++) {
                char t = tmp[i];
                tmp[i] = tmp[tmp_len - 1 - i];
                tmp[tmp_len - 1 - i] = t;
            }
            break;
        }

        /* ---- %x : hexadecimal lowercase ---- */
        case 'x': {
            unsigned int uval = va_arg(args, unsigned int);
            const char* hx = "0123456789abcdef";
            tmp_len = 0;
            if (uval == 0) {
                tmp[tmp_len++] = '0';
            } else {
                while (uval > 0) {
                    tmp[tmp_len++] = hx[uval & 0xF];
                    uval >>= 4;
                }
            }
            /* Reverse */
            for (int i = 0; i < tmp_len / 2; i++) {
                char t = tmp[i];
                tmp[i] = tmp[tmp_len - 1 - i];
                tmp[tmp_len - 1 - i] = t;
            }
            break;
        }

        /* ---- %X : hexadecimal uppercase ---- */
        case 'X': {
            unsigned int uval = va_arg(args, unsigned int);
            const char* hx = "0123456789ABCDEF";
            tmp_len = 0;
            if (uval == 0) {
                tmp[tmp_len++] = '0';
            } else {
                while (uval > 0) {
                    tmp[tmp_len++] = hx[uval & 0xF];
                    uval >>= 4;
                }
            }
            /* Reverse */
            for (int i = 0; i < tmp_len / 2; i++) {
                char t = tmp[i];
                tmp[i] = tmp[tmp_len - 1 - i];
                tmp[tmp_len - 1 - i] = t;
            }
            break;
        }

        /* ---- %p : pointer (0x-prefixed hex) ---- */
        case 'p': {
            unsigned int uval = (unsigned int)va_arg(args, void*);
            const char* hx = "0123456789abcdef";
            /* Write "0x" then 8 hex digits */
            tmp[0] = '0'; tmp[1] = 'x';
            tmp_len = 10;
            for (int i = 0; i < 8; i++) {
                tmp[9 - i] = hx[uval & 0xF];
                uval >>= 4;
            }
            break;
        }

        /* ---- %c : character ---- */
        case 'c': {
            char c = (char)va_arg(args, int);
            tmp[0] = c;
            tmp_len = 1;
            break;
        }

        /* ---- %% : literal percent ---- */
        case '%': {
            tmp[0] = '%';
            tmp_len = 1;
            break;
        }

        /* ---- Unknown specifier: emit as-is ---- */
        default: {
            tmp[0] = '%';
            tmp_len = 1;
            /* Put back the unknown char so we output it in the next iteration */
            if (*p) {
                if (out < end) *out++ = tmp[0];
                p++;
                continue;
            }
            break;
        }
        } /* switch */

        p++;    /* Advance past the specifier character */

        /* ----- Apply width / padding ----- */
        int pad_len = width - tmp_len;
        if (pad_len < 0) pad_len = 0;

        if (left_justify) {
            /* Content first, then spaces */
            for (int i = 0; i < tmp_len && out < end; i++)
                *out++ = tmp[i];
            for (int i = 0; i < pad_len && out < end; i++)
                *out++ = ' ';
        } else if (zero_pad) {
            /* Pad with '0' then content */
            for (int i = 0; i < pad_len && out < end; i++)
                *out++ = '0';
            for (int i = 0; i < tmp_len && out < end; i++)
                *out++ = tmp[i];
        } else {
            /* Pad with spaces then content */
            for (int i = 0; i < pad_len && out < end; i++)
                *out++ = ' ';
            for (int i = 0; i < tmp_len && out < end; i++)
                *out++ = tmp[i];
        }
    }

    *out = '\0';
    return (int)(out - buf);
}

/* ================================================================== */
/*  Internal: write a single char to serial                           */
/* ================================================================== */

static void klog_serial_putc(char c)
{
    write_serial(c);
}

/* ================================================================== */
/*  Internal: write formatted level line to serial                    */
/* ================================================================== */

/**
 * Format: [LEVEL][source] message\n
 * Example: [INFO ][kernel/main.c] System initialized (ticks=1234)
 */
static void klog_write_serial(klog_level_t level, const char* source,
                               const char* message)
{
    klog_serial_putc('[');

    /* Level tag - always 5 chars */
    const char* tag = level_tags[level];
    for (int i = 0; i < 5; i++)
        klog_serial_putc(tag[i]);

    klog_serial_putc(']');
    klog_serial_putc('[');

    /* Source name */
    if (source) {
        while (*source)
            klog_serial_putc(*source++);
    } else {
        klog_serial_putc('?');
    }

    klog_serial_putc(']');
    klog_serial_putc(' ');

    /* Message body */
    const char* m = message;
    while (*m)
        klog_serial_putc(*m++);

    klog_serial_putc('\n');
}

/* ================================================================== */
/*  Internal: write formatted level line to VGA                       */
/* ================================================================== */

static void klog_write_vga(klog_level_t level, const char* source,
                            const char* message)
{
    char line[256];
    int pos = 0;

    /* [LEVEL] */
    line[pos++] = '[';
    const char* tag = level_tags[level];
    for (int i = 0; i < 5 && pos < (int)sizeof(line) - 2; i++)
        line[pos++] = tag[i];
    line[pos++] = ']';
    line[pos++] = ' ';

    /* Source (abbreviated to avoid overflow) */
    if (source) {
        /* Trim to last path component for brevity */
        const char* base = source;
        for (int i = 0; source[i]; i++) {
            if (source[i] == '/' || source[i] == '\\')
                base = &source[i + 1];
        }
        while (*base && pos < (int)sizeof(line) - 4)
            line[pos++] = *base++;
    }

    line[pos++] = ':';
    line[pos++] = ' ';

    /* Message */
    const char* m = message;
    while (*m && pos < (int)sizeof(line) - 2)
        line[pos++] = *m++;

    line[pos++] = '\n';
    line[pos]   = '\0';

    /* Temporarily un-mute VGA log so the message appears */
    vga_mute_log(0);
    vga_print(line);
    vga_mute_log(1);
}

/* ================================================================== */
/*  Internal: store an entry into the ring buffer                     */
/* ================================================================== */

static void klog_store_entry(klog_level_t level, const char* source,
                              const char* message)
{
    klog_entry_t* slot = &ring[ring_head];

    slot->timestamp = ticks;
    slot->level     = level;
    slot->source    = source ? source : "?";
    strncpy(slot->message, message, KLOG_MSG_SIZE - 1);
    slot->message[KLOG_MSG_SIZE - 1] = '\0';

    ring_head = (ring_head + 1) % KLOG_RING_SIZE;

    if (ring_count < KLOG_RING_SIZE) {
        ring_count++;
    } else {
        /* Buffer full: advance tail, overwriting the oldest entry */
        ring_tail = (ring_tail + 1) % KLOG_RING_SIZE;
        stats.dropped++;
    }
}

/* ================================================================== */
/*  Public API: Initialization                                        */
/* ================================================================== */

void klog_init(void)
{
    klog_lock_acquire();

    /* Zero the ring buffer */
    memset(ring, 0, sizeof(ring));
    ring_head  = 0;
    ring_tail  = 0;
    ring_count = 0;

    /* Default configuration */
    min_level      = KLOG_INFO;
    serial_enabled = 1;
    vga_enabled    = 0;

    /* Clear statistics */
    memset(&stats, 0, sizeof(stats));

    initialized = 1;

    klog_lock_release();

    klog_log(KLOG_INFO, "klog", "Kernel logger initialized (ring=%u entries)",
             KLOG_RING_SIZE);
}

/* ================================================================== */
/*  Public API: Core logging functions                                */
/* ================================================================== */

void klog_log(klog_level_t level, const char* source, const char* fmt, ...)
{
    /* Level filter: discard messages below the minimum level */
    if (level < min_level)
        return;

    /* Sanity check level */
    if (level > KLOG_CRIT)
        level = KLOG_CRIT;

    /* Format the message */
    char buf[KLOG_MSG_SIZE];
    va_list args;
    va_start(args, fmt);
    klog_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Store in ring buffer (spinlock-protected) */
    klog_lock_acquire();
    klog_store_entry(level, source, buf);
    stats.total_logged++;
    if (level <= KLOG_CRIT)
        stats.by_level[level]++;
    klog_lock_release();

    /* Route to outputs (outside lock to reduce contention) */
    if (serial_enabled)
        klog_write_serial(level, source, buf);
    if (vga_enabled)
        klog_write_vga(level, source, buf);
}

void klog_log_raw(klog_level_t level, const char* source, const char* message)
{
    /* Level filter */
    if (level < min_level)
        return;

    /* Sanity check level */
    if (level > KLOG_CRIT)
        level = KLOG_CRIT;

    /* Store in ring buffer */
    klog_lock_acquire();
    klog_store_entry(level, source, message ? message : "");
    stats.total_logged++;
    if (level <= KLOG_CRIT)
        stats.by_level[level]++;
    klog_lock_release();

    /* Route to outputs */
    if (serial_enabled)
        klog_write_serial(level, source, message ? message : "");
    if (vga_enabled)
        klog_write_vga(level, source, message ? message : "");
}

/* ================================================================== */
/*  Public API: Configuration                                         */
/* ================================================================== */

void klog_set_level(klog_level_t level)
{
    if (level > KLOG_NONE) level = KLOG_NONE;
    klog_lock_acquire();
    min_level = level;
    klog_lock_release();
}

klog_level_t klog_get_level(void)
{
    klog_level_t lvl;
    klog_lock_acquire();
    lvl = min_level;
    klog_lock_release();
    return lvl;
}

void klog_set_serial_output(int enabled)
{
    klog_lock_acquire();
    serial_enabled = enabled ? 1 : 0;
    klog_lock_release();
}

void klog_set_vga_output(int enabled)
{
    klog_lock_acquire();
    vga_enabled = enabled ? 1 : 0;
    klog_lock_release();
}

/* ================================================================== */
/*  Public API: Query / Retrieval                                     */
/* ================================================================== */

uint32_t klog_get_count(void)
{
    uint32_t cnt;
    klog_lock_acquire();
    cnt = ring_count;
    klog_lock_release();
    return cnt;
}

int klog_get_entry(uint32_t index, klog_entry_t* out)
{
    if (!out)
        return -1;

    klog_lock_acquire();

    if (index >= ring_count) {
        klog_lock_release();
        return -1;
    }

    /* Map logical index to physical slot (oldest first) */
    uint32_t phys = (ring_tail + index) % KLOG_RING_SIZE;

    /* Copy the entry */
    *out = ring[phys];

    klog_lock_release();
    return 0;
}

int klog_get_recent(uint32_t count, klog_entry_t* out,
                     uint32_t* actual_count)
{
    if (!out || !actual_count)
        return -1;

    klog_lock_acquire();

    /* We can return at most ring_count entries */
    uint32_t n = (count > ring_count) ? ring_count : count;

    /* Start from the (ring_count - n)-th entry (oldest of the recent set) */
    uint32_t start_idx = ring_count - n;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t phys = (ring_tail + start_idx + i) % KLOG_RING_SIZE;
        out[i] = ring[phys];
    }

    *actual_count = n;
    klog_lock_release();
    return 0;
}

void klog_clear(void)
{
    klog_lock_acquire();

    memset(ring, 0, sizeof(ring));
    ring_head  = 0;
    ring_tail  = 0;
    ring_count = 0;
    memset(&stats, 0, sizeof(stats));

    klog_lock_release();
}

/* ================================================================== */
/*  Public API: Statistics                                            */
/* ================================================================== */

klog_stats_t* klog_get_stats(void)
{
    return &stats;
}

/* ================================================================== */
/*  Public API: Dump / Display                                        */
/* ================================================================== */

void klog_dump_to_serial(void)
{
    klog_lock_acquire();

    s_printf("=== klog dump (");
    char numbuf[16];
    int_to_str((int)ring_count, numbuf);
    s_printf(numbuf);
    s_printf(" entries) ===\n");

    for (uint32_t i = 0; i < ring_count; i++) {
        uint32_t phys = (ring_tail + i) % KLOG_RING_SIZE;
        klog_entry_t* e = &ring[phys];

        /* Reuse the serial formatter */
        klog_lock_release();   /* Release lock for I/O */
        klog_write_serial(e->level, e->source, e->message);
        klog_lock_acquire();   /* Re-acquire for next iteration */
    }

    s_printf("=== klog dump end ===\n");

    klog_lock_release();
}

void klog_dump_to_vga(void)
{
    klog_lock_acquire();

    uint32_t count = ring_count;

    vga_mute_log(0);
    vga_print("=== klog dump ===\n");

    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = (ring_tail + i) % KLOG_RING_SIZE;
        klog_entry_t* e = &ring[phys];

        /* Format a line for VGA */
        char line[256];
        int pos = 0;

        /* Timestamp */
        int_to_str((int)e->timestamp, &line[pos]);
        while (line[pos]) pos++;
        line[pos++] = ' ';

        /* Level tag */
        line[pos++] = '[';
        const char* tag = level_tags[e->level];
        for (int j = 0; j < 5 && pos < (int)sizeof(line) - 4; j++)
            line[pos++] = tag[j];
        line[pos++] = ']';
        line[pos++] = ' ';

        /* Source */
        if (e->source) {
            const char* s = e->source;
            while (*s && pos < (int)sizeof(line) - 4)
                line[pos++] = *s++;
        }
        line[pos++] = ':';
        line[pos++] = ' ';

        /* Message */
        const char* m = e->message;
        while (*m && pos < (int)sizeof(line) - 2)
            line[pos++] = *m++;
        line[pos++] = '\n';
        line[pos]   = '\0';

        klog_lock_release();   /* Release for I/O */
        vga_print(line);
        klog_lock_acquire();   /* Re-acquire */
    }

    vga_print("=== klog dump end ===\n");
    vga_mute_log(1);

    klog_lock_release();
}

/* ================================================================== */
/*  Public API: Early boot                                            */
/* ================================================================== */

void klog_early_write(const char* message)
{
    if (message)
        serial_write_string(message);
}
