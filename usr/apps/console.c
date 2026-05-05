/**
 * console.c - CamelOS Console / Log Viewer Application
 *
 * A macOS Console.app-inspired kernel log viewer that displays
 * structured log entries from the kernel ring buffer (klog).
 *
 * Features:
 *   - Scrollable log entry list with color-coded level badges
 *   - Toolbar with Refresh, Clear, Level filter, and Dump Serial
 *   - Status bar with entry count and current filter level
 *   - Auto-scroll to newest entries
 *   - Keyboard shortcuts: r=refresh, c=clear, q=quit
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#include "../framework.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../core/klog.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/drivers/serial.h"
#include "../../core/window_server.h"
#include "../../sys/api.h"

/* ================================================================== */
/*  Constants                                                         */
/* ================================================================== */

#define CONSOLE_WIN_W       600
#define CONSOLE_WIN_H       400
#define TOOLBAR_H           38
#define STATUSBAR_H         24
#define ENTRY_LINE_H        22
#define CHAR_W              8
#define CHAR_H              16
#define ENTRY_CACHE_SIZE    128
#define SCROLL_STEP         3

/* Color palette — macOS-inspired */
#define COL_WIN_BG          0xFFFFFFFF
#define COL_TOOLBAR_BG      0xFFF2F2F7
#define COL_TOOLBAR_BORDER  0xFFD1D1D6
#define COL_STATUSBAR_BG    0xFFF2F2F7
#define COL_STATUSBAR_BORDER 0xFFD1D1D6
#define COL_ENTRY_BG_ALT    0xFFFAFAFA
#define COL_ENTRY_BG_NORM   0xFFFFFFFF
#define COL_ENTRY_HOVER     0xFFE8F0FE
#define COL_TEXT_PRIMARY     0xFF1D1D1F
#define COL_TEXT_SECONDARY   0xFF8E8E93
#define COL_TEXT_TIMESTAMP   0xFF636366
#define COL_BTN_BG          0xFFE8E8ED
#define COL_BTN_BG_HOVER    0xFFD8D8DD
#define COL_BTN_TEXT         0xFF1D1D1F
#define COL_ACCENT          0xFF007AFF
#define COL_SCROLLBAR_TRACK 0x10C0C0C0
#define COL_SCROLLBAR_THUMB 0xFFC0C0C0

/* Level badge colors */
#define COL_BADGE_DEBUG     0xFF8E8E93
#define COL_BADGE_INFO      0xFF007AFF
#define COL_BADGE_WARN      0xFFFF9500
#define COL_BADGE_ERROR     0xFFFF3B30
#define COL_BADGE_CRIT      0xFFD32F2F

/* Level badge background (light tint) */
#define COL_BADGE_BG_DEBUG  0xFFF2F2F7
#define COL_BADGE_BG_INFO   0xFFE3F2FD
#define COL_BADGE_BG_WARN   0xFFFFF8E1
#define COL_BADGE_BG_ERROR  0xFFFFEBEE
#define COL_BADGE_BG_CRIT   0xFFFCE4EC

/* Filter level IDs for the level selector */
#define FILTER_ALL          0
#define FILTER_DEBUG        1
#define FILTER_INFO         2
#define FILTER_WARN         3
#define FILTER_ERROR        4
#define FILTER_CRIT         5
#define FILTER_COUNT        6

/* Button IDs */
#define BTN_REFRESH         0
#define BTN_CLEAR           1
#define BTN_LEVEL           2
#define BTN_DUMP_SERIAL     3
#define BTN_COUNT           4

/* ================================================================== */
/*  Application State                                                 */
/* ================================================================== */

typedef struct {
    klog_entry_t  entries[ENTRY_CACHE_SIZE];
    uint32_t      entry_count;
    klog_level_t  filter_level;      /* KLOG_DEBUG means "show all" */
    int           filter_id;         /* FILTER_ALL..FILTER_CRIT */
    int           scroll_offset;     /* Pixels scrolled from top */
    int           auto_scroll;       /* 1 = snap to bottom on refresh */
    int           hovered_btn;       /* -1 = none */
    int           level_popup_open;  /* 1 = level dropdown is open */
    int           hovered_filter;    /* Hovered item in level popup */
    int           win_w;             /* Current window width */
    int           win_h;             /* Current window height */
    uint32_t      total_in_kernel;   /* Total entries in kernel ring buf */
    klog_stats_t* stats;             /* Pointer to live stats */
} console_state_t;

static console_state_t g_console;
static window_t* g_console_win = NULL;

/* ================================================================== */
/*  Helper Functions                                                  */
/* ================================================================== */

static const char* level_name(klog_level_t level) {
    switch (level) {
        case KLOG_DEBUG: return "DEBUG";
        case KLOG_INFO:  return "INFO";
        case KLOG_WARN:  return "WARN";
        case KLOG_ERROR: return "ERROR";
        case KLOG_CRIT:  return "CRIT";
        default:         return "?????";
    }
}

static const char* filter_name(int filter_id) {
    switch (filter_id) {
        case FILTER_ALL:   return "All";
        case FILTER_DEBUG: return "Debug";
        case FILTER_INFO:  return "Info";
        case FILTER_WARN:  return "Warn";
        case FILTER_ERROR: return "Error";
        case FILTER_CRIT:  return "Critical";
        default:           return "All";
    }
}

static uint32_t badge_color(klog_level_t level) {
    switch (level) {
        case KLOG_DEBUG: return COL_BADGE_DEBUG;
        case KLOG_INFO:  return COL_BADGE_INFO;
        case KLOG_WARN:  return COL_BADGE_WARN;
        case KLOG_ERROR: return COL_BADGE_ERROR;
        case KLOG_CRIT:  return COL_BADGE_CRIT;
        default:         return COL_BADGE_DEBUG;
    }
}

static uint32_t badge_bg_color(klog_level_t level) {
    switch (level) {
        case KLOG_DEBUG: return COL_BADGE_BG_DEBUG;
        case KLOG_INFO:  return COL_BADGE_BG_INFO;
        case KLOG_WARN:  return COL_BADGE_BG_WARN;
        case KLOG_ERROR: return COL_BADGE_BG_ERROR;
        case KLOG_CRIT:  return COL_BADGE_BG_CRIT;
        default:         return COL_BADGE_BG_DEBUG;
    }
}

static klog_level_t filter_to_level(int filter_id) {
    switch (filter_id) {
        case FILTER_ALL:   return KLOG_DEBUG;  /* Show everything */
        case FILTER_DEBUG: return KLOG_DEBUG;
        case FILTER_INFO:  return KLOG_INFO;
        case FILTER_WARN:  return KLOG_WARN;
        case FILTER_ERROR: return KLOG_ERROR;
        case FILTER_CRIT:  return KLOG_CRIT;
        default:           return KLOG_DEBUG;
    }
}

/* Format a timestamp (tick count) into a readable string */
static void format_timestamp(uint32_t ticks, char* buf, int buf_len) {
    uint32_t seconds = ticks / 100;
    uint32_t minutes = seconds / 60;
    uint32_t hours   = minutes / 60;
    seconds %= 60;
    minutes %= 60;

    char tmp[32];
    /* Build "HH:MM:SS" */
    int_to_str(hours, tmp);
    if (hours < 10) { buf[0] = '0'; strcpy(buf + 1, tmp); }
    else strcpy(buf, tmp);
    strcat(buf, ":");

    int pos = strlen(buf);
    if (minutes < 10) { buf[pos++] = '0'; buf[pos] = 0; }
    int_to_str(minutes, tmp);
    strcat(buf, tmp);
    strcat(buf, ":");

    pos = strlen(buf);
    if (seconds < 10) { buf[pos++] = '0'; buf[pos] = 0; }
    int_to_str(seconds, tmp);
    strcat(buf, tmp);

    /* Append tick fraction */
    uint32_t frac = ticks % 100;
    strcat(buf, ".");
    int_to_str(frac, tmp);
    if (frac < 10) strcat(buf, "0");
    strcat(buf, tmp);
}

/* ================================================================== */
/*  Data Loading                                                      */
/* ================================================================== */

static void console_refresh_entries(void) {
    g_console.total_in_kernel = klog_get_count();
    g_console.stats = klog_get_stats();

    if (g_console.total_in_kernel == 0) {
        g_console.entry_count = 0;
        return;
    }

    /* Fetch the most recent entries up to our cache size */
    uint32_t to_fetch = g_console.total_in_kernel;
    if (to_fetch > ENTRY_CACHE_SIZE) to_fetch = ENTRY_CACHE_SIZE;

    klog_entry_t raw[ENTRY_CACHE_SIZE];
    uint32_t actual = 0;
    klog_get_recent(to_fetch, raw, &actual);

    /* Filter by level */
    g_console.entry_count = 0;
    for (uint32_t i = 0; i < actual && g_console.entry_count < ENTRY_CACHE_SIZE; i++) {
        if (raw[i].level >= g_console.filter_level ||
            g_console.filter_id == FILTER_ALL) {
            g_console.entries[g_console.entry_count] = raw[i];
            g_console.entry_count++;
        }
    }

    /* Auto-scroll to bottom if enabled */
    if (g_console.auto_scroll) {
        int content_h = (int)g_console.entry_count * ENTRY_LINE_H;
        int view_h = g_console.win_h - TOOLBAR_H - STATUSBAR_H;
        g_console.scroll_offset = (content_h > view_h) ? (content_h - view_h) : 0;
    }
}

static void console_clear_display(void) {
    g_console.entry_count = 0;
    g_console.scroll_offset = 0;
}

/* ================================================================== */
/*  Drawing — Toolbar                                                 */
/* ================================================================== */

/* Button layout: each button has an x-position and width */
typedef struct {
    int x;
    int w;
    const char* label;
} btn_layout_t;

static void compute_btn_layout(btn_layout_t* btns, int toolbar_w) {
    int pad = 10;
    int gap = 8;
    int x = pad;

    /* Refresh */
    btns[BTN_REFRESH].x = x;
    btns[BTN_REFRESH].w = 70;
    btns[BTN_REFRESH].label = "Refresh";
    x += btns[BTN_REFRESH].w + gap;

    /* Clear */
    btns[BTN_CLEAR].x = x;
    btns[BTN_CLEAR].w = 56;
    btns[BTN_CLEAR].label = "Clear";
    x += btns[BTN_CLEAR].w + gap;

    /* Level selector */
    btns[BTN_LEVEL].x = x;
    btns[BTN_LEVEL].w = 90;
    btns[BTN_LEVEL].label = filter_name(g_console.filter_id);
    x += btns[BTN_LEVEL].w + gap;

    /* Dump Serial */
    btns[BTN_DUMP_SERIAL].x = x;
    btns[BTN_DUMP_SERIAL].w = 100;
    btns[BTN_DUMP_SERIAL].label = "Dump Serial";
}

static void draw_toolbar(int x, int y, int w) {
    /* Toolbar background */
    gfx_fill_rect(x, y, w, TOOLBAR_H, COL_TOOLBAR_BG);
    gfx_fill_rect(x, y + TOOLBAR_H - 1, w, 1, COL_TOOLBAR_BORDER);

    btn_layout_t btns[BTN_COUNT];
    compute_btn_layout(btns, w);

    for (int i = 0; i < BTN_COUNT; i++) {
        int bx = x + btns[i].x;
        int by = y + 5;
        int bw = btns[i].w;
        int bh = TOOLBAR_H - 10;

        uint32_t bg = COL_BTN_BG;
        if (g_console.hovered_btn == i) bg = COL_BTN_BG_HOVER;

        /* Level button gets an accent indicator */
        if (i == BTN_LEVEL) {
            gfx_fill_rounded_rect(bx, by, bw, bh, bg, 6);
            /* Small colored dot indicating current filter level */
            uint32_t dot_color = COL_ACCENT;
            if (g_console.filter_id != FILTER_ALL) {
                dot_color = badge_color(g_console.filter_level);
            }
            gfx_fill_rounded_rect(bx + 5, by + bh / 2 - 3, 6, 6, dot_color, 3);
            gfx_draw_string(bx + 15, by + (bh - CHAR_H) / 2, btns[i].label, COL_BTN_TEXT);
        } else {
            gfx_fill_rounded_rect(bx, by, bw, bh, bg, 6);
            gfx_draw_string(bx + 8, by + (bh - CHAR_H) / 2, btns[i].label, COL_BTN_TEXT);
        }

        /* Separator line between buttons */
        if (i < BTN_COUNT - 1) {
            /* Just rely on spacing */
        }
    }

    /* Shortcut hints on the right side */
    int hint_x = x + w - 170;
    gfx_draw_string(hint_x, y + (TOOLBAR_H - CHAR_H) / 2,
                    "[R]efresh  [C]lear  [Q]uit", COL_TEXT_SECONDARY);
}

/* ================================================================== */
/*  Drawing — Level Popup Dropdown                                    */
/* ================================================================== */

static void draw_level_popup(int x, int y, int w) {
    if (!g_console.level_popup_open) return;

    btn_layout_t btns[BTN_COUNT];
    compute_btn_layout(btns, w);

    int popup_x = x + btns[BTN_LEVEL].x;
    int popup_y = y + TOOLBAR_H;
    int popup_w = btns[BTN_LEVEL].w + 20;
    int popup_h = FILTER_COUNT * 26 + 8;

    /* Shadow */
    gfx_fill_rect(popup_x + 3, popup_y + 3, popup_w, popup_h, 0x30000000);

    /* Background */
    gfx_fill_rounded_rect(popup_x, popup_y, popup_w, popup_h, 0xFFFFFFFF, 8);
    gfx_stroke_rounded_rect(popup_x, popup_y, popup_w, popup_h, 0xFFD1D1D6, 8, 1);

    const char* names[] = {"All", "Debug", "Info", "Warn", "Error", "Critical"};
    uint32_t colors[] = {
        COL_ACCENT,
        COL_BADGE_DEBUG, COL_BADGE_INFO, COL_BADGE_WARN,
        COL_BADGE_ERROR, COL_BADGE_CRIT
    };

    for (int i = 0; i < FILTER_COUNT; i++) {
        int iy = popup_y + 4 + i * 26;
        int ih = 26;

        /* Hover highlight */
        if (g_console.hovered_filter == i) {
            gfx_fill_rounded_rect(popup_x + 4, iy, popup_w - 8, ih, COL_ENTRY_HOVER, 4);
        }

        /* Color dot */
        gfx_fill_rounded_rect(popup_x + 12, iy + 8, 10, 10, colors[i], 5);

        /* Label */
        gfx_draw_string(popup_x + 28, iy + (ih - CHAR_H) / 2, names[i], COL_TEXT_PRIMARY);

        /* Check mark if selected */
        if (i == g_console.filter_id) {
            gfx_draw_string(popup_x + popup_w - 22, iy + (ih - CHAR_H) / 2,
                           "\x2713", COL_ACCENT);
        }
    }
}

/* ================================================================== */
/*  Drawing — Log Entry List                                          */
/* ================================================================== */

static void draw_entry_list(int x, int y, int w, int h) {
    /* Content area background */
    gfx_fill_rect(x, y, w, h, COL_WIN_BG);

    if (g_console.entry_count == 0) {
        /* Empty state */
        gfx_draw_string(x + 20, y + 20, "No log entries to display.", COL_TEXT_SECONDARY);
        gfx_draw_string(x + 20, y + 40, "Press [R] or click Refresh to reload.", COL_TEXT_SECONDARY);
        return;
    }

    /* Apply clipping for the entry area */
    gfx_set_clip(x, y, w, h);

    int scroll = g_console.scroll_offset;
    int first_visible = scroll / ENTRY_LINE_H;
    int view_entries = (h / ENTRY_LINE_H) + 2;

    for (int i = first_visible; i < (int)g_console.entry_count &&
         i < first_visible + view_entries; i++) {
        int ey = y + i * ENTRY_LINE_H - scroll;
        int eh = ENTRY_LINE_H;

        /* Skip if entirely off-screen */
        if (ey + eh <= y || ey >= y + h) continue;

        /* Alternating row background */
        uint32_t row_bg = (i % 2 == 0) ? COL_ENTRY_BG_NORM : COL_ENTRY_BG_ALT;
        gfx_fill_rect(x, ey, w - 12, eh, row_bg);

        klog_entry_t* entry = &g_console.entries[i];
        int cx = x + 8;

        /* Timestamp column (width ~100px) */
        char ts_buf[24];
        format_timestamp(entry->timestamp, ts_buf, sizeof(ts_buf));
        gfx_draw_string(cx, ey + (eh - CHAR_H) / 2, ts_buf, COL_TEXT_TIMESTAMP);
        cx += 110;

        /* Level badge (width ~56px) */
        {
            const char* lbl = level_name(entry->level);
            int lbl_w = strlen(lbl) * CHAR_W;
            int badge_w = lbl_w + 12;
            int badge_h = 16;
            int badge_y = ey + (eh - badge_h) / 2;

            gfx_fill_rounded_rect(cx, badge_y, badge_w, badge_h,
                                  badge_bg_color(entry->level), 4);
            gfx_draw_string(cx + 6, badge_y + (badge_h - CHAR_H) / 2,
                           lbl, badge_color(entry->level));
            cx += badge_w + 8;
        }

        /* Source column (width ~100px) */
        if (entry->source) {
            char src_short[16];
            /* Show just the filename part */
            const char* slash = strrchr(entry->source, '/');
            const char* src = slash ? slash + 1 : entry->source;
            int slen = strlen(src);
            if (slen > 14) slen = 14;
            memcpy(src_short, src, slen);
            src_short[slen] = 0;

            gfx_draw_string(cx, ey + (eh - CHAR_H) / 2, src_short, COL_ACCENT);
            cx += 108;
        } else {
            cx += 108;
        }

        /* Message (remaining width) */
        {
            int msg_max_w = w - 12 - (cx - x);
            char msg_trunc[192];
            int mlen = strlen(entry->message);
            int max_chars = msg_max_w / CHAR_W;
            if (max_chars > 191) max_chars = 191;
            if (mlen > max_chars) mlen = max_chars;
            memcpy(msg_trunc, entry->message, mlen);
            msg_trunc[mlen] = 0;

            /* Choose text color based on severity */
            uint32_t msg_color = COL_TEXT_PRIMARY;
            if (entry->level >= KLOG_ERROR) msg_color = COL_BADGE_ERROR;

            gfx_draw_string(cx, ey + (eh - CHAR_H) / 2, msg_trunc, msg_color);
        }
    }

    gfx_reset_clip();

    /* Scrollbar */
    int content_h = (int)g_console.entry_count * ENTRY_LINE_H;
    if (content_h > h) {
        int sb_x = x + w - 12;
        int sb_h = h;
        int thumb_h = (h * h) / content_h;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > sb_h) thumb_h = sb_h;

        int scroll_range = content_h - h;
        int thumb_y = y;
        if (scroll_range > 0) {
            thumb_y = y + (g_console.scroll_offset * (sb_h - thumb_h)) / scroll_range;
        }
        if (thumb_y < y) thumb_y = y;
        if (thumb_y + thumb_h > y + sb_h) thumb_y = y + sb_h - thumb_h;

        /* Track */
        gfx_fill_rect(sb_x, y, 8, sb_h, COL_SCROLLBAR_TRACK);
        /* Thumb */
        gfx_fill_rounded_rect(sb_x + 1, thumb_y, 6, thumb_h, COL_SCROLLBAR_THUMB, 3);
    }
}

/* ================================================================== */
/*  Drawing — Status Bar                                              */
/* ================================================================== */

static void draw_statusbar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, STATUSBAR_H, COL_STATUSBAR_BG);
    gfx_fill_rect(x, y, w, 1, COL_STATUSBAR_BORDER);

    char status[128];

    /* Left side: entry count */
    strcpy(status, "Entries: ");
    char num[16];
    int_to_str((int)g_console.entry_count, num);
    strcat(status, num);
    strcat(status, " / ");
    int_to_str((int)g_console.total_in_kernel, num);
    strcat(status, num);
    strcat(status, " in buffer");
    gfx_draw_string(x + 12, y + (STATUSBAR_H - CHAR_H) / 2, status, COL_TEXT_SECONDARY);

    /* Right side: filter level */
    strcpy(status, "Filter: ");
    strcat(status, filter_name(g_console.filter_id));

    if (g_console.stats) {
        strcat(status, "  |  Dropped: ");
        int_to_str((int)g_console.stats->dropped, num);
        strcat(status, num);
    }

    int status_w = strlen(status) * CHAR_W;
    gfx_draw_string(x + w - status_w - 12, y + (STATUSBAR_H - CHAR_H) / 2,
                   status, COL_TEXT_SECONDARY);
}

/* ================================================================== */
/*  Main Paint Callback                                               */
/* ================================================================== */

static void console_on_paint(window_t* win, int x, int y, int w, int h) {
    /* Full background */
    gfx_fill_rect(x, y, w, h, COL_WIN_BG);

    /* Toolbar */
    draw_toolbar(x, y, w);

    /* Entry list area */
    int list_y = y + TOOLBAR_H;
    int list_h = h - TOOLBAR_H - STATUSBAR_H;
    draw_entry_list(x, list_y, w, list_h);

    /* Status bar */
    draw_statusbar(x, y + h - STATUSBAR_H, w);

    /* Level popup (drawn last, on top of everything) */
    draw_level_popup(x, y, w);
}

/* ================================================================== */
/*  Input Handling                                                    */
/* ================================================================== */

static void console_on_input(window_t* win, int key) {
    if (key == 0) return;

    /* Close popup on any key first if it's open */
    if (g_console.level_popup_open) {
        if (key == 27 || key == 'q') {  /* Escape or q closes popup */
            g_console.level_popup_open = 0;
            return;
        }
        /* Number keys 1-6 select filter level */
        if (key >= '1' && key <= '6') {
            int new_filter = key - '1';
            g_console.filter_id = new_filter;
            g_console.filter_level = filter_to_level(new_filter);
            g_console.level_popup_open = 0;
            g_console.auto_scroll = 1;
            console_refresh_entries();
            return;
        }
        g_console.level_popup_open = 0;
        return;
    }

    /* Keyboard shortcuts */
    switch (key) {
        case 'r':
        case 'R':
            g_console.auto_scroll = 1;
            console_refresh_entries();
            break;
        case 'c':
        case 'C':
            console_clear_display();
            break;
        case 'q':
        case 'Q':
            if (g_console_win) {
                ws_close(g_console_win);
            }
            break;
        case 'l':
        case 'L':
            g_console.level_popup_open = !g_console.level_popup_open;
            break;
        case 'd':
        case 'D':
            klog_dump_to_serial();
            s_printf("[Console] Log dump sent to serial port.\n");
            break;
    }
}

/* ================================================================== */
/*  Mouse Handling                                                    */
/* ================================================================== */

static int hit_test_btn(int mx, int my, int toolbar_w) {
    if (my < 5 || my >= TOOLBAR_H - 5) return -1;

    btn_layout_t btns[BTN_COUNT];
    compute_btn_layout(btns, toolbar_w);

    for (int i = 0; i < BTN_COUNT; i++) {
        if (mx >= btns[i].x && mx < btns[i].x + btns[i].w &&
            my >= 5 && my < TOOLBAR_H - 5) {
            return i;
        }
    }
    return -1;
}

static void console_on_mouse(window_t* win, int mx, int my, int btn) {
    /* Track hover state */
    g_console.hovered_btn = hit_test_btn(mx, my, g_console.win_w);
    g_console.hovered_filter = -1;

    /* Handle level popup hover */
    if (g_console.level_popup_open && mx < 120) {
        /* Compute popup bounds */
        btn_layout_t btns[BTN_COUNT];
        compute_btn_layout(btns, g_console.win_w);

        int popup_x = btns[BTN_LEVEL].x;
        int popup_y = TOOLBAR_H;
        int popup_w = btns[BTN_LEVEL].w + 20;

        if (mx >= popup_x && mx < popup_x + popup_w &&
            my >= popup_y) {
            int rel_y = my - popup_y - 4;
            if (rel_y >= 0) {
                int idx = rel_y / 26;
                if (idx >= 0 && idx < FILTER_COUNT) {
                    g_console.hovered_filter = idx;
                }
            }
        }
    }

    /* Click handling */
    if (btn != 1) return;

    /* Check level popup clicks first */
    if (g_console.level_popup_open) {
        btn_layout_t btns[BTN_COUNT];
        compute_btn_layout(btns, g_console.win_w);

        int popup_x = btns[BTN_LEVEL].x;
        int popup_y = TOOLBAR_H;
        int popup_w = btns[BTN_LEVEL].w + 20;

        if (mx >= popup_x && mx < popup_x + popup_w &&
            my >= popup_y) {
            int rel_y = my - popup_y - 4;
            if (rel_y >= 0) {
                int idx = rel_y / 26;
                if (idx >= 0 && idx < FILTER_COUNT) {
                    g_console.filter_id = idx;
                    g_console.filter_level = filter_to_level(idx);
                    g_console.auto_scroll = 1;
                    console_refresh_entries();
                }
            }
        }
        /* Close popup regardless of where we clicked */
        g_console.level_popup_open = 0;
        return;
    }

    /* Toolbar button clicks */
    int clicked = hit_test_btn(mx, my, g_console.win_w);
    switch (clicked) {
        case BTN_REFRESH:
            g_console.auto_scroll = 1;
            console_refresh_entries();
            break;
        case BTN_CLEAR:
            console_clear_display();
            break;
        case BTN_LEVEL:
            g_console.level_popup_open = !g_console.level_popup_open;
            break;
        case BTN_DUMP_SERIAL:
            klog_dump_to_serial();
            s_printf("[Console] Full log dump sent to serial port.\n");
            break;
    }
}

/* ================================================================== */
/*  Scroll Handling                                                   */
/* ================================================================== */

static void console_on_scroll(window_t* win, int delta) {
    /* Close popup if open */
    if (g_console.level_popup_open) {
        g_console.level_popup_open = 0;
        return;
    }

    g_console.auto_scroll = 0;  /* Manual scroll disables auto-scroll */

    g_console.scroll_offset -= delta * (ENTRY_LINE_H * SCROLL_STEP);

    int content_h = (int)g_console.entry_count * ENTRY_LINE_H;
    int view_h = g_console.win_h - TOOLBAR_H - STATUSBAR_H;
    int max_scroll = (content_h > view_h) ? (content_h - view_h) : 0;

    if (g_console.scroll_offset < 0) g_console.scroll_offset = 0;
    if (g_console.scroll_offset > max_scroll) g_console.scroll_offset = max_scroll;

    /* If scrolled to the very bottom, re-enable auto-scroll */
    if (g_console.scroll_offset >= max_scroll - ENTRY_LINE_H) {
        g_console.auto_scroll = 1;
    }
}

/* ================================================================== */
/*  Resize Handling                                                   */
/* ================================================================== */

static void console_on_resize(window_t* win, int new_w, int new_h) {
    g_console.win_w = new_w;
    g_console.win_h = new_h;

    /* Re-clamp scroll offset */
    int content_h = (int)g_console.entry_count * ENTRY_LINE_H;
    int view_h = new_h - TOOLBAR_H - STATUSBAR_H;
    int max_scroll = (content_h > view_h) ? (content_h - view_h) : 0;
    if (g_console.scroll_offset > max_scroll) g_console.scroll_offset = max_scroll;
}

/* ================================================================== */
/*  Menu Action Handler                                               */
/* ================================================================== */

static void console_on_menu_action(int menu_id, int item_idx) {
    if (menu_id == 0) {  /* View menu */
        switch (item_idx) {
            case 0:  /* Refresh */
                g_console.auto_scroll = 1;
                console_refresh_entries();
                break;
            case 1:  /* Clear Display */
                console_clear_display();
                break;
            case 2:  /* Dump to Serial */
                klog_dump_to_serial();
                s_printf("[Console] Log dump sent to serial port.\n");
                break;
        }
    } else if (menu_id == 1) {  /* Level menu */
        if (item_idx >= 0 && item_idx < FILTER_COUNT) {
            g_console.filter_id = item_idx;
            g_console.filter_level = filter_to_level(item_idx);
            g_console.auto_scroll = 1;
            console_refresh_entries();
        }
    }
}

/* ================================================================== */
/*  Initialization                                                    */
/* ================================================================== */

void init_console_app(void) {
    /* Initialize state */
    memset(&g_console, 0, sizeof(g_console));
    g_console.filter_level = KLOG_DEBUG;
    g_console.filter_id = FILTER_ALL;
    g_console.auto_scroll = 1;
    g_console.hovered_btn = -1;
    g_console.hovered_filter = -1;
    g_console.win_w = CONSOLE_WIN_W;
    g_console.win_h = CONSOLE_WIN_H;

    /* Load initial data */
    console_refresh_entries();

    /* Create the window */
    g_console_win = fw_create_window("Console", CONSOLE_WIN_W, CONSOLE_WIN_H,
                                      console_on_paint,
                                      console_on_input,
                                      console_on_mouse);
    if (!g_console_win) return;

    g_console_win->min_w = 400;
    g_console_win->min_h = 300;

    /* Wire up callbacks */
    g_console_win->scroll_callback = (void*)console_on_scroll;
    g_console_win->resize_callback = (void*)console_on_resize;

    /* Set up menus */
    g_console_win->menu_count = 2;

    strcpy(g_console_win->menus[0].name, "View");
    strcpy(g_console_win->menus[0].items[0].label, "Refresh");
    strcpy(g_console_win->menus[0].items[1].label, "Clear Display");
    strcpy(g_console_win->menus[0].items[2].label, "Dump to Serial");
    g_console_win->menus[0].item_count = 3;

    strcpy(g_console_win->menus[1].name, "Level");
    strcpy(g_console_win->menus[1].items[0].label, "All");
    strcpy(g_console_win->menus[1].items[1].label, "Debug");
    strcpy(g_console_win->menus[1].items[2].label, "Info");
    strcpy(g_console_win->menus[1].items[3].label, "Warn");
    strcpy(g_console_win->menus[1].items[4].label, "Error");
    g_console_win->menus[1].item_count = 5;

    g_console_win->on_menu_action = (void*)console_on_menu_action;

    /* Register in dock */
    fw_register_dock("Console", 0, g_console_win);

    /* Log our own launch */
    s_printf("[Console] Log viewer initialized — ");
    char buf[16];
    int_to_str((int)g_console.total_in_kernel, buf);
    s_printf(buf);
    s_printf(" entries in kernel ring buffer.\n");
}
