// usr/apps/process_monitor.c - CamelOS Activity Monitor
// macOS-inspired process monitor with CPU, Memory, Processes, and System tabs
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../core/scheduler.h"
#include "../../core/task.h"
#include "../../core/vmm.h"
#include "../../core/process.h"
#include "../../core/signal.h"
#include "../../core/ipc.h"
#include "../../core/pipe.h"
#include "../../hal/drivers/serial.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/cpu/timer.h"
#include "../../core/window_server.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define WIN_W           600
#define WIN_H           420
#define TAB_BAR_H       32
#define TOOLBAR_H       36
#define STATUS_BAR_H    24
#define TABLE_HEADER_H  22
#define ROW_H           20
#define MAX_DISPLAY_PROCS 64
#define REFRESH_INTERVAL 100  /* in ticks (~2 sec at 50Hz) */

/* Tab IDs */
#define TAB_CPU        0
#define TAB_MEMORY     1
#define TAB_PROCESSES  2
#define TAB_SYSTEM     3
#define TAB_COUNT      4

/* Column IDs for process table */
#define COL_PID        0
#define COL_NAME       1
#define COL_STATE      2
#define COL_PRIORITY   3
#define COL_CPU_TIME   4
#define COL_UID        5
#define COL_COUNT      6

/* Color palette — macOS Activity Monitor inspired */
#define COLOR_BG              0xFFF5F5F7
#define COLOR_TAB_BAR_BG      0xFFE8E8ED
#define COLOR_TAB_ACTIVE_BG   0xFFFFFFFF
#define COLOR_TAB_ACTIVE_LINE 0xFF007AFF
#define COLOR_TAB_TEXT        0xFF8E8E93
#define COLOR_TAB_ACTIVE_TEXT 0xFF007AFF
#define COLOR_TOOLBAR_BG      0xFFF2F2F7
#define COLOR_STATUS_BG       0xFFF2F2F7
#define COLOR_TABLE_HEADER_BG 0xFFE8E8ED
#define COLOR_TABLE_ROW_BG    0xFFFFFFFF
#define COLOR_TABLE_ALT_BG    0xFFF9F9FB
#define COLOR_TABLE_SEL_BG    0xFF007AFF
#define COLOR_TEXT_PRIMARY     0xFF1D1D1F
#define COLOR_TEXT_SECONDARY   0xFF8E8E93
#define COLOR_TEXT_WHITE       0xFFFFFFFF
#define COLOR_SEPARATOR       0xFFD1D1D6
#define COLOR_STATE_RUNNING   0xFF34C759
#define COLOR_STATE_READY     0xFF007AFF
#define COLOR_STATE_BLOCKED   0xFFFFCC00
#define COLOR_STATE_SLEEPING  0xFF8E8E93
#define COLOR_STATE_ZOMBIE    0xFFFF3B30
#define COLOR_BAR_BG          0xFFE5E5EA
#define COLOR_BAR_USED        0xFF007AFF
#define COLOR_BAR_FREE        0xFF34C759
#define COLOR_BAR_RESERVED    0xFFFF9500
#define COLOR_BTN_BG          0xFFE5E5EA
#define COLOR_BTN_TEXT        0xFF1D1D1F
#define COLOR_BTN_DANGER_BG   0xFFFF3B30
#define COLOR_BTN_DANGER_TEXT 0xFFFFFFFF
#define COLOR_CARD_BG         0xFFFFFFFF
#define COLOR_CARD_BORDER     0xFFE5E5EA
#define COLOR_SECTION_HEADER  0xFF007AFF

/* ========================================================================
 * Application State
 * ======================================================================== */

typedef struct {
    int current_tab;       /* 0=CPU, 1=Memory, 2=Processes, 3=System */
    int sort_column;       /* Which column to sort by */
    int sort_ascending;    /* 1 = ascending, 0 = descending */
    int selected_pid;      /* Currently selected PID in process list */
    int auto_refresh;      /* 1 = auto refresh on */
    uint32_t refresh_counter; /* Tick counter for auto-refresh */
    int scroll_offset;     /* Vertical scroll for process list */
} process_monitor_state_t;

static process_monitor_state_t pm_state = {
    .current_tab = TAB_PROCESSES,
    .sort_column = COL_PID,
    .sort_ascending = 1,
    .selected_pid = -1,
    .auto_refresh = 1,
    .refresh_counter = 0,
    .scroll_offset = 0
};

/* Cached process data */
typedef struct {
    int pid;
    char name[32];
    int state;
    uint8_t priority;
    uint32_t time_used;
    int uid;
    int is_app_bundle;
    uint32_t sleep_until;
    int block_reason;
} proc_entry_t;

static proc_entry_t proc_list[MAX_DISPLAY_PROCS];
static int proc_count = 0;

/* Cached scheduler / VMM stats */
static sched_stats_t* sched_stats = 0;
static frame_stats_t* frame_stats = 0;
static uint32_t kernel_free_mem = 0;

/* Button hit regions */
static int btn_refresh_x = 0, btn_refresh_y = 0;
static int btn_refresh_w = 70, btn_refresh_h = 24;
static int btn_kill_x = 0, btn_kill_y = 0;
static int btn_kill_w = 90, btn_kill_h = 24;

/* Window reference */
static Window* pm_window = 0;
static int pm_win_w = WIN_W;

/* ========================================================================
 * Data Collection
 * ======================================================================== */

static void collect_process_data(void) {
    proc_count = 0;

    /* Try process_list() API first */
    process_info_t infos[MAX_DISPLAY_PROCS];
    int listed = process_list(infos, MAX_DISPLAY_PROCS);

    if (listed > 0) {
        for (int i = 0; i < listed && i < MAX_DISPLAY_PROCS; i++) {
            proc_entry_t* e = &proc_list[i];
            e->pid = infos[i].pid;
            strncpy(e->name, infos[i].name, 31);
            e->name[31] = 0;
            e->state = infos[i].state;
            e->priority = SCHED_PRIORITY_DEFAULT;
            e->time_used = infos[i].cpu_ticks;
            e->uid = infos[i].uid;
            e->is_app_bundle = 0;
            e->sleep_until = 0;
            e->block_reason = 0;
        }
        proc_count = listed;
    } else {
        /* Fallback: walk task_list_head circular linked list */
        extern task_t* task_list_head;
        extern task_t* current_task;

        if (task_list_head) {
            task_t* cur = task_list_head;
            int count = 0;
            do {
                if (count >= MAX_DISPLAY_PROCS) break;
                proc_entry_t* e = &proc_list[count];
                e->pid = cur->id;
                strncpy(e->name, cur->name, 31);
                e->name[31] = 0;
                e->state = cur->state;
                e->priority = cur->priority;
                e->time_used = cur->time_used;
                e->uid = cur->uid;
                e->is_app_bundle = cur->is_app_bundle;
                e->sleep_until = cur->sleep_until;
                e->block_reason = cur->block_reason;
                count++;
                cur = cur->next;
            } while (cur && cur != task_list_head);
            proc_count = count;
        }
    }

    /* Collect system stats */
    sched_stats = scheduler_get_stats();
    frame_stats = vmm_get_frame_stats();
    extern uint32_t k_get_free_mem(void);
    kernel_free_mem = k_get_free_mem();
}

/* Simple insertion sort for the process list */
static int proc_compare(const proc_entry_t* a, const proc_entry_t* b) {
    int result = 0;
    switch (pm_state.sort_column) {
        case COL_PID:      result = a->pid - b->pid; break;
        case COL_NAME:     result = strcmp(a->name, b->name); break;
        case COL_STATE:    result = a->state - b->state; break;
        case COL_PRIORITY: result = (int)a->priority - (int)b->priority; break;
        case COL_CPU_TIME: result = (a->time_used > b->time_used) ? 1 :
                                   (a->time_used < b->time_used) ? -1 : 0; break;
        case COL_UID:      result = a->uid - b->uid; break;
        default:           result = a->pid - b->pid; break;
    }
    return pm_state.sort_ascending ? result : -result;
}

static void sort_process_list(void) {
    for (int i = 1; i < proc_count; i++) {
        proc_entry_t tmp = proc_list[i];
        int j = i - 1;
        while (j >= 0 && proc_compare(&proc_list[j], &tmp) > 0) {
            proc_list[j + 1] = proc_list[j];
            j--;
        }
        proc_list[j + 1] = tmp;
    }
}

/* ========================================================================
 * Drawing Helpers
 * ======================================================================== */

static void draw_rounded_card(int x, int y, int w, int h) {
    gfx_fill_rounded_rect(x, y, w, h, COLOR_CARD_BG, 8);
    gfx_draw_rect(x, y, w, h, COLOR_CARD_BORDER);
}

static void draw_section_header(int x, int y, const char* title) {
    gfx_draw_string(x, y, title, COLOR_SECTION_HEADER);
}

static void draw_stat_row(int x, int y, int label_w, const char* label, const char* value) {
    gfx_draw_string(x, y, label, COLOR_TEXT_SECONDARY);
    gfx_draw_string(x + label_w, y, value, COLOR_TEXT_PRIMARY);
}

/* Draw a horizontal utilization bar */
static void draw_util_bar(int x, int y, int w, int h, uint32_t used_color, float pct) {
    /* Background */
    gfx_fill_rounded_rect(x, y, w, h, COLOR_BAR_BG, 4);
    /* Filled portion */
    int fill_w = (int)(w * pct);
    if (fill_w < 0) fill_w = 0;
    if (fill_w > w) fill_w = w;
    if (fill_w > 2) {
        gfx_fill_rounded_rect(x, y, fill_w, h, used_color, 4);
    }
}

/* Draw a small pill/badge */
static void draw_badge(int x, int y, const char* text, uint32_t bg, uint32_t fg) {
    int tw = strlen(text) * 8;
    int bw = tw + 12;
    gfx_fill_rounded_rect(x, y, bw, 18, bg, 4);
    gfx_draw_string(x + 6, y + 2, text, fg);
}

/* Format number with commas (simple, up to 999,999,999) */
static void format_number(uint32_t n, char* buf) {
    char tmp[16];
    int_to_str((int)n, tmp);
    int len = strlen(tmp);
    int commas = (len - 1) / 3;
    int new_len = len + commas;
    buf[new_len] = 0;
    int j = new_len - 1;
    for (int i = len - 1, count = 0; i >= 0; i--, count++) {
        if (count > 0 && count % 3 == 0) {
            buf[j--] = ',';
        }
        buf[j--] = tmp[i];
    }
}

static void format_mem_kb(uint32_t kb, char* buf) {
    if (kb >= 1024 * 1024) {
        uint32_t gb = kb / (1024 * 1024);
        uint32_t gb_frac = (kb % (1024 * 1024)) / (1024 * 102);
        char n[16], f[16];
        int_to_str((int)gb, n);
        int_to_str((int)gb_frac, f);
        strcpy(buf, n);
        strcat(buf, ".");
        strcat(buf, f);
        strcat(buf, " GB");
    } else if (kb >= 1024) {
        uint32_t mb = kb / 1024;
        char n[16];
        int_to_str((int)mb, n);
        strcpy(buf, n);
        strcat(buf, " MB");
    } else {
        char n[16];
        int_to_str((int)kb, n);
        strcpy(buf, n);
        strcat(buf, " KB");
    }
}

/* ========================================================================
 * Tab Bar Drawing
 * ======================================================================== */

static void draw_tab_bar(int x, int y, int w) {
    /* Tab bar background */
    gfx_fill_rect(x, y, w, TAB_BAR_H, COLOR_TAB_BAR_BG);
    gfx_draw_rect(x, y + TAB_BAR_H - 1, w, 1, COLOR_SEPARATOR);

    const char* tab_names[] = {"CPU", "Memory", "Processes", "System"};
    int tab_w = w / TAB_COUNT;

    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = x + i * tab_w;
        int is_active = (i == pm_state.current_tab);

        if (is_active) {
            gfx_fill_rect(tx, y, tab_w, TAB_BAR_H - 1, COLOR_TAB_ACTIVE_BG);
            /* Active indicator line at bottom */
            gfx_fill_rect(tx + 4, y + TAB_BAR_H - 3, tab_w - 8, 3, COLOR_TAB_ACTIVE_LINE);
        } else {
            gfx_fill_rect(tx, y, tab_w, TAB_BAR_H - 1, COLOR_TAB_BAR_BG);
        }

        /* Tab separator (except last) */
        if (i < TAB_COUNT - 1) {
            gfx_fill_rect(tx + tab_w - 1, y + 6, 1, TAB_BAR_H - 12, COLOR_SEPARATOR);
        }

        /* Tab label */
        int text_w = strlen(tab_names[i]) * 8;
        gfx_draw_string(tx + (tab_w - text_w) / 2, y + 9,
                        tab_names[i],
                        is_active ? COLOR_TAB_ACTIVE_TEXT : COLOR_TAB_TEXT);
    }
}

/* ========================================================================
 * Toolbar Drawing
 * ======================================================================== */

static void draw_toolbar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, TOOLBAR_H, COLOR_TOOLBAR_BG);
    gfx_draw_rect(x, y + TOOLBAR_H - 1, w, 1, COLOR_SEPARATOR);

    int cx = x + 10;

    /* Refresh button */
    btn_refresh_x = cx;
    btn_refresh_y = y + 6;
    gfx_fill_rounded_rect(cx, y + 6, btn_refresh_w, btn_refresh_h, COLOR_BTN_BG, 4);
    gfx_draw_string(cx + 12, y + 11, "Refresh", COLOR_BTN_TEXT);
    cx += btn_refresh_w + 8;

    /* Kill Process button */
    btn_kill_x = cx;
    btn_kill_y = y + 6;
    gfx_fill_rounded_rect(cx, y + 6, btn_kill_w, btn_kill_h, COLOR_BTN_DANGER_BG, 4);
    gfx_draw_string(cx + 8, y + 11, "Kill Process", COLOR_BTN_DANGER_TEXT);
    cx += btn_kill_w + 16;

    /* Auto-refresh indicator */
    if (pm_state.auto_refresh) {
        draw_badge(cx, y + 8, "Auto", 0xFFE8F5E9, 0xFF34C759);
    }
}

/* ========================================================================
 * Status Bar Drawing
 * ======================================================================== */

static void draw_status_bar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, STATUS_BAR_H, COLOR_STATUS_BG);
    gfx_draw_rect(x, y, w, 1, COLOR_SEPARATOR);

    /* Process count */
    char buf[64];
    strcpy(buf, "Processes: ");
    char num[16];
    int_to_str(proc_count, num);
    strcat(buf, num);
    gfx_draw_string(x + 10, y + 5, buf, COLOR_TEXT_SECONDARY);

    /* Current time */
    int hour, minute, second;
    sys_get_time(&hour, &minute, &second);
    char time_buf[32];
    int_to_str(hour < 10 ? 0 : hour / 10, num); strcpy(time_buf, num);
    int_to_str(hour % 10, num); strcat(time_buf, num);
    strcat(time_buf, ":");
    int_to_str(minute < 10 ? 0 : minute / 10, num); strcat(time_buf, num);
    int_to_str(minute % 10, num); strcat(time_buf, num);
    strcat(time_buf, ":");
    int_to_str(second < 10 ? 0 : second / 10, num); strcat(time_buf, num);
    int_to_str(second % 10, num); strcat(time_buf, num);

    int tw = strlen(time_buf) * 8;
    gfx_draw_string(x + w - tw - 12, y + 5, time_buf, COLOR_TEXT_SECONDARY);
}

/* ========================================================================
 * Processes Tab
 * ======================================================================== */

static const char* state_name(int state) {
    switch (state) {
        case TASK_STATE_RUNNING: return "Running";
        case TASK_STATE_READY:   return "Ready";
        case TASK_STATE_BLOCKED: return "Blocked";
        case TASK_STATE_ZOMBIE:  return "Zombie";
        case TASK_STATE_SLEEPING:return "Sleeping";
        default:                 return "Unknown";
    }
}

static uint32_t state_color(int state) {
    switch (state) {
        case TASK_STATE_RUNNING: return COLOR_STATE_RUNNING;
        case TASK_STATE_READY:   return COLOR_STATE_READY;
        case TASK_STATE_BLOCKED: return COLOR_STATE_BLOCKED;
        case TASK_STATE_ZOMBIE:  return COLOR_STATE_ZOMBIE;
        case TASK_STATE_SLEEPING:return COLOR_STATE_SLEEPING;
        default:                 return COLOR_TEXT_SECONDARY;
    }
}

static void draw_processes_tab(int x, int y, int w, int h) {
    int table_y = y;
    int table_h = h;

    /* Column widths */
    int col_x[COL_COUNT];
    int col_w[COL_COUNT];
    int cx = x + 8;

    col_x[COL_PID] = cx;      col_w[COL_PID] = 42;  cx += col_w[COL_PID];
    col_x[COL_NAME] = cx;     col_w[COL_NAME] = 140; cx += col_w[COL_NAME];
    col_x[COL_STATE] = cx;    col_w[COL_STATE] = 72;  cx += col_w[COL_STATE];
    col_x[COL_PRIORITY] = cx; col_w[COL_PRIORITY] = 56; cx += col_w[COL_PRIORITY];
    col_x[COL_CPU_TIME] = cx; col_w[COL_CPU_TIME] = 72; cx += col_w[COL_CPU_TIME];
    col_x[COL_UID] = cx;      col_w[COL_UID] = 40;

    /* Table header */
    gfx_fill_rect(x, table_y, w, TABLE_HEADER_H, COLOR_TABLE_HEADER_BG);
    gfx_draw_rect(x, table_y + TABLE_HEADER_H - 1, w, 1, COLOR_SEPARATOR);

    const char* headers[] = {"PID", "Name", "State", "Pri", "CPU", "UID"};
    for (int c = 0; c < COL_COUNT; c++) {
        uint32_t col = COLOR_TEXT_SECONDARY;
        if (c == pm_state.sort_column) col = COLOR_TAB_ACTIVE_TEXT;

        gfx_draw_string(col_x[c], table_y + 4, headers[c], col);

        /* Sort indicator arrow */
        if (c == pm_state.sort_column) {
            char arrow[4];
            strcpy(arrow, pm_state.sort_ascending ? " ^" : " v");
            gfx_draw_string(col_x[c] + strlen(headers[c]) * 8, table_y + 4, arrow, COLOR_TAB_ACTIVE_TEXT);
        }
    }

    /* Table rows */
    int row_start = table_y + TABLE_HEADER_H;
    int max_visible = (table_h - TABLE_HEADER_H) / ROW_H;
    if (max_visible < 0) max_visible = 0;

    /* Clamp scroll */
    int max_scroll = (proc_count > max_visible) ? (proc_count - max_visible) : 0;
    if (pm_state.scroll_offset > max_scroll) pm_state.scroll_offset = max_scroll;
    if (pm_state.scroll_offset < 0) pm_state.scroll_offset = 0;

    for (int i = 0; i < max_visible; i++) {
        int idx = i + pm_state.scroll_offset;
        if (idx >= proc_count) break;

        int ry = row_start + i * ROW_H;
        proc_entry_t* p = &proc_list[idx];
        int is_selected = (p->pid == pm_state.selected_pid);

        /* Row background */
        uint32_t row_bg = (i % 2 == 0) ? COLOR_TABLE_ROW_BG : COLOR_TABLE_ALT_BG;
        if (is_selected) row_bg = COLOR_TABLE_SEL_BG;
        gfx_fill_rect(x, ry, w, ROW_H, row_bg);

        /* PID */
        char num[16];
        int_to_str(p->pid, num);
        gfx_draw_string(col_x[COL_PID], ry + 3, num,
                        is_selected ? COLOR_TEXT_WHITE : COLOR_TEXT_PRIMARY);

        /* Name */
        char name_buf[18];
        strncpy(name_buf, p->name, 17);
        name_buf[17] = 0;
        gfx_draw_string(col_x[COL_NAME], ry + 3, name_buf,
                        is_selected ? COLOR_TEXT_WHITE : COLOR_TEXT_PRIMARY);

        /* State (color-coded) */
        uint32_t s_col = is_selected ? COLOR_TEXT_WHITE : state_color(p->state);
        const char* s_name = state_name(p->state);
        /* Draw colored dot before state name */
        if (!is_selected) {
            gfx_fill_rounded_rect(col_x[COL_STATE], ry + 6, 8, 8, state_color(p->state), 4);
            gfx_draw_string(col_x[COL_STATE] + 12, ry + 3, s_name, s_col);
        } else {
            gfx_draw_string(col_x[COL_STATE], ry + 3, s_name, s_col);
        }

        /* Priority */
        int_to_str((int)p->priority, num);
        gfx_draw_string(col_x[COL_PRIORITY], ry + 3, num,
                        is_selected ? COLOR_TEXT_WHITE : COLOR_TEXT_PRIMARY);

        /* CPU Time */
        int_to_str((int)p->time_used, num);
        gfx_draw_string(col_x[COL_CPU_TIME], ry + 3, num,
                        is_selected ? COLOR_TEXT_WHITE : COLOR_TEXT_PRIMARY);

        /* UID */
        int_to_str(p->uid, num);
        gfx_draw_string(col_x[COL_UID], ry + 3, num,
                        is_selected ? COLOR_TEXT_WHITE : COLOR_TEXT_PRIMARY);
    }

    /* Vertical scrollbar */
    if (proc_count > max_visible && max_visible > 0) {
        int sb_x = x + w - 10;
        int sb_h = table_h - TABLE_HEADER_H;
        int thumb_h = (sb_h * max_visible) / proc_count;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int scroll_range = proc_count - max_visible;
        int thumb_y = row_start;
        if (scroll_range > 0) {
            thumb_y = row_start + (pm_state.scroll_offset * (sb_h - thumb_h)) / scroll_range;
        }
        gfx_fill_rect(sb_x, row_start, 8, sb_h, 0x20C0C0C0);
        gfx_fill_rounded_rect(sb_x + 1, thumb_y, 6, thumb_h, 0xFFC0C0C0, 3);
    }
}

/* ========================================================================
 * CPU Tab
 * ======================================================================== */

static void draw_cpu_tab(int x, int y, int w, int h) {
    int cy = y + 16;
    int card_w = w - 24;
    int card_x = x + 12;

    /* === Scheduler Statistics Card === */
    draw_section_header(card_x, cy, "Scheduler Statistics");
    cy += 22;

    draw_rounded_card(card_x, cy, card_w, 100);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char num[32];

        if (sched_stats) {
            draw_stat_row(lx, sy, 180, "Context Switches:",     (format_number(sched_stats->context_switches, num), num)); sy += 22;
            draw_stat_row(lx, sy, 180, "Total Tasks:",         (format_number(sched_stats->total_tasks, num), num)); sy += 22;
            draw_stat_row(lx, sy, 180, "Tasks Created:",       (format_number(sched_stats->tasks_created, num), num)); sy += 22;
            draw_stat_row(lx, sy, 180, "Tasks Destroyed:",     (format_number(sched_stats->tasks_destroyed, num), num));
        } else {
            gfx_draw_string(lx, sy, "(Scheduler stats unavailable)", COLOR_TEXT_SECONDARY);
        }
    }
    cy += 112;

    /* === CPU Usage by Process (Bar Chart) === */
    draw_section_header(card_x, cy, "CPU Time by Process");
    cy += 22;

    /* Find max time_used for scaling */
    uint32_t max_time = 1;
    int bar_count = proc_count < 8 ? proc_count : 8;
    for (int i = 0; i < bar_count; i++) {
        if (proc_list[i].time_used > max_time)
            max_time = proc_list[i].time_used;
    }

    int bar_area_h = bar_count * 28 + 12;
    draw_rounded_card(card_x, cy, card_w, bar_area_h);

    int by = cy + 8;
    for (int i = 0; i < bar_count; i++) {
        proc_entry_t* p = &proc_list[i];

        /* Process name (truncated) */
        char name_buf[14];
        strncpy(name_buf, p->name, 13);
        name_buf[13] = 0;
        gfx_draw_string(card_x + 16, by + 2, name_buf, COLOR_TEXT_PRIMARY);

        /* Bar */
        int bar_x = card_x + 130;
        int bar_w = card_w - 190;
        float pct = (float)p->time_used / (float)max_time;
        draw_util_bar(bar_x, by + 2, bar_w, 14, COLOR_BAR_USED, pct);

        /* Value label */
        char num[16];
        int_to_str((int)p->time_used, num);
        gfx_draw_string(bar_x + bar_w + 6, by + 2, num, COLOR_TEXT_SECONDARY);

        by += 28;
    }
    cy += bar_area_h + 8;

    /* CPU load estimate from context switches */
    if (sched_stats && sched_stats->context_switches > 0) {
        draw_section_header(card_x, cy, "Activity Summary");
        cy += 22;
        draw_rounded_card(card_x, cy, card_w, 50);
        {
            int sy = cy + 12;
            int lx = card_x + 16;
            char num[32];
            format_number(sched_stats->context_switches, num);
            char line[80];
            strcpy(line, "Total context switches: ");
            strcat(line, num);
            gfx_draw_string(lx, sy, line, COLOR_TEXT_PRIMARY);

            sy += 22;
            /* Simple activity level */
            const char* level = "Low";
            uint32_t cs = sched_stats->context_switches;
            if (cs > 100000) level = "High";
            else if (cs > 10000) level = "Medium";
            uint32_t level_col = 0xFF34C759;
            if (cs > 100000) level_col = 0xFFFF3B30;
            else if (cs > 10000) level_col = 0xFFFF9500;
            gfx_draw_string(lx, sy, "Activity Level: ", COLOR_TEXT_SECONDARY);
            draw_badge(lx + 120, sy - 2, level, level_col, COLOR_TEXT_WHITE);
        }
    }
}

/* ========================================================================
 * Memory Tab
 * ======================================================================== */

static void draw_memory_tab(int x, int y, int w, int h) {
    int cy = y + 16;
    int card_w = w - 24;
    int card_x = x + 12;

    /* === Physical Memory (VMM Frame Stats) === */
    draw_section_header(card_x, cy, "Physical Memory");
    cy += 22;

    int phys_card_h = 130;
    draw_rounded_card(card_x, cy, card_w, phys_card_h);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char buf[32], num[32];

        if (frame_stats) {
            /* Total memory in KB (each frame = 4KB) */
            uint32_t total_kb = frame_stats->total_frames * 4;
            uint32_t used_kb  = frame_stats->used_frames * 4;
            uint32_t free_kb  = frame_stats->free_frames * 4;
            uint32_t res_kb   = frame_stats->reserved_frames * 4;

            format_mem_kb(total_kb, buf);
            draw_stat_row(lx, sy, 140, "Total:", buf); sy += 22;

            format_mem_kb(used_kb, buf);
            draw_stat_row(lx, sy, 140, "Used:", buf); sy += 22;

            format_mem_kb(free_kb, buf);
            draw_stat_row(lx, sy, 140, "Free:", buf); sy += 22;

            format_mem_kb(res_kb, buf);
            draw_stat_row(lx, sy, 140, "Reserved:", buf); sy += 24;

            /* Memory utilization bar (stacked: used | reserved | free) */
            int bar_x = lx;
            int bar_w = card_w - 32;
            int bar_h = 20;
            gfx_fill_rounded_rect(bar_x, sy, bar_w, bar_h, COLOR_BAR_BG, 4);

            if (frame_stats->total_frames > 0) {
                int used_w = (bar_w * frame_stats->used_frames) / frame_stats->total_frames;
                int res_w  = (bar_w * frame_stats->reserved_frames) / frame_stats->total_frames;
                int free_w = bar_w - used_w - res_w;
                if (free_w < 0) free_w = 0;

                int bx = bar_x;
                if (used_w > 2) {
                    gfx_fill_rounded_rect(bx, sy, used_w, bar_h, COLOR_BAR_USED, 4);
                    bx += used_w;
                }
                if (res_w > 2) {
                    gfx_fill_rounded_rect(bx, sy, res_w, bar_h, COLOR_BAR_RESERVED, 4);
                    bx += res_w;
                }
                if (free_w > 2) {
                    gfx_fill_rounded_rect(bx, sy, free_w, bar_h, COLOR_BAR_FREE, 4);
                }
            }

            /* Legend below bar */
            sy += 28;
            int leg_x = lx;
            gfx_fill_rounded_rect(leg_x, sy + 1, 10, 10, COLOR_BAR_USED, 2);
            gfx_draw_string(leg_x + 14, sy, "Used", COLOR_TEXT_SECONDARY);
            leg_x += 60;
            gfx_fill_rounded_rect(leg_x, sy + 1, 10, 10, COLOR_BAR_RESERVED, 2);
            gfx_draw_string(leg_x + 14, sy, "Reserved", COLOR_TEXT_SECONDARY);
            leg_x += 80;
            gfx_fill_rounded_rect(leg_x, sy + 1, 10, 10, COLOR_BAR_FREE, 2);
            gfx_draw_string(leg_x + 14, sy, "Free", COLOR_TEXT_SECONDARY);
        } else {
            gfx_draw_string(lx, sy, "(VMM frame stats unavailable)", COLOR_TEXT_SECONDARY);
        }
    }
    cy += phys_card_h + 12;

    /* === Kernel Heap Memory === */
    draw_section_header(card_x, cy, "Kernel Heap");
    cy += 22;

    draw_rounded_card(card_x, cy, card_w, 70);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char buf[32];

        format_mem_kb(kernel_free_mem / 1024, buf);
        draw_stat_row(lx, sy, 140, "Free Heap:", buf); sy += 22;

        /* Simple heap utilization estimate */
        extern uint32_t k_get_free_mem(void);
        uint32_t free_mem = k_get_free_mem();
        /* Approximate: use a reasonable total for display */
        uint32_t total_approx = 16 * 1024 * 1024; /* 16 MB typical heap */
        float pct = 1.0f - ((float)free_mem / (float)total_approx);
        if (pct < 0) pct = 0;
        if (pct > 1) pct = 1;

        char pct_buf[16];
        int pct_int = (int)(pct * 100);
        int_to_str(pct_int, buf);
        strcpy(pct_buf, buf);
        strcat(pct_buf, "%");
        draw_stat_row(lx, sy, 140, "Heap Usage:", pct_buf); sy += 24;

        draw_util_bar(lx, sy, card_w - 32, 14, COLOR_BAR_USED, pct);
    }
    cy += 82;

    /* === Frame Statistics Detail === */
    if (frame_stats) {
        draw_section_header(card_x, cy, "Frame Allocator");
        cy += 22;

        draw_rounded_card(card_x, cy, card_w, 70);
        {
            int sy = cy + 12;
            int lx = card_x + 16;
            char num[32];

            format_number(frame_stats->total_frames, num);
            draw_stat_row(lx, sy, 140, "Total Frames:", num); sy += 22;
            format_number(frame_stats->used_frames, num);
            draw_stat_row(lx, sy, 140, "Used Frames:", num); sy += 22;
            format_number(frame_stats->free_frames, num);
            draw_stat_row(lx, sy, 140, "Free Frames:", num);
        }
    }
}

/* ========================================================================
 * System Tab
 * ======================================================================== */

static void draw_system_tab(int x, int y, int w, int h) {
    int cy = y + 16;
    int card_w = w - 24;
    int card_x = x + 12;

    /* === System Overview === */
    draw_section_header(card_x, cy, "System Overview");
    cy += 22;

    draw_rounded_card(card_x, cy, card_w, 92);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char num[32], buf[64];

        /* Uptime - timer runs at configured frequency, divide by 50 for seconds */
        uint32_t ticks = get_tick_count();
        uint32_t uptime_sec = ticks / 50;
        uint32_t uptime_min = uptime_sec / 60;
        uint32_t uptime_hr  = uptime_min / 60;
        uptime_sec %= 60;
        uptime_min %= 60;

        strcpy(buf, "");
        int_to_str((int)uptime_hr, num); strcat(buf, num); strcat(buf, "h ");
        int_to_str((int)uptime_min, num); strcat(buf, num); strcat(buf, "m ");
        int_to_str((int)uptime_sec, num); strcat(buf, num); strcat(buf, "s");
        draw_stat_row(lx, sy, 140, "Uptime:", buf); sy += 22;

        int_to_str(proc_count, num);
        draw_stat_row(lx, sy, 140, "Total Processes:", num); sy += 22;

        /* Running / blocked counts */
        int running = 0, blocked = 0, sleeping = 0, zombie = 0;
        for (int i = 0; i < proc_count; i++) {
            switch (proc_list[i].state) {
                case TASK_STATE_RUNNING: running++; break;
                case TASK_STATE_BLOCKED: blocked++; break;
                case TASK_STATE_SLEEPING:sleeping++; break;
                case TASK_STATE_ZOMBIE:  zombie++; break;
            }
        }

        strcpy(buf, "");
        int_to_str(running, num); strcat(buf, num);
        strcat(buf, " run / ");
        int_to_str(blocked, num); strcat(buf, num);
        strcat(buf, " blocked / ");
        int_to_str(sleeping, num); strcat(buf, num);
        strcat(buf, " sleep");
        draw_stat_row(lx, sy, 140, "State Breakdown:", buf);
    }
    cy += 104;

    /* === Scheduler Statistics === */
    draw_section_header(card_x, cy, "Scheduler");
    cy += 22;

    draw_rounded_card(card_x, cy, card_w, 92);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char num[32];

        if (sched_stats) {
            format_number(sched_stats->context_switches, num);
            draw_stat_row(lx, sy, 180, "Context Switches:", num); sy += 22;
            format_number(sched_stats->tasks_created, num);
            draw_stat_row(lx, sy, 180, "Tasks Created:", num); sy += 22;
            format_number(sched_stats->tasks_destroyed, num);
            draw_stat_row(lx, sy, 180, "Tasks Destroyed:", num); sy += 22;

            /* Throughput */
            int throughput = (int)(sched_stats->tasks_created - sched_stats->tasks_destroyed);
            int_to_str(throughput, num);
            draw_stat_row(lx, sy, 180, "Active (net):", num);
        } else {
            gfx_draw_string(lx, sy, "(Scheduler stats unavailable)", COLOR_TEXT_SECONDARY);
        }
    }
    cy += 104;

    /* === IPC / Pipes === */
    draw_section_header(card_x, cy, "Inter-Process Communication");
    cy += 22;

    draw_rounded_card(card_x, cy, card_w, 70);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char num[16];

        /* Count active IPC ports from the internal port table */
        extern ipc_port_t ipc_ports[IPC_MAX_PORTS];
        int active_ports = 0;
        for (int i = 0; i < IPC_MAX_PORTS; i++) {
            if (ipc_ports[i].in_use) active_ports++;
        }
        int_to_str(active_ports, num);
        draw_stat_row(lx, sy, 140, "IPC Ports:", num); sy += 22;

        /* Pipe statistics from pipe_get_stats() */
        pipe_stats_t* pstats = pipe_get_stats();
        if (pstats) {
            int_to_str(pstats->active_pipes, num);
            draw_stat_row(lx, sy, 140, "Active Pipes:", num); sy += 22;
            int_to_str(pstats->active_fifos, num);
            draw_stat_row(lx, sy, 140, "Named FIFOs:", num);
        } else {
            gfx_draw_string(lx, sy, "(Pipe stats unavailable)", COLOR_TEXT_SECONDARY);
        }
    }
    cy += 82;

    /* === Memory Quick Summary === */
    draw_section_header(card_x, cy, "Memory Summary");
    cy += 22;

    draw_rounded_card(card_x, cy, card_w, 50);
    {
        int sy = cy + 12;
        int lx = card_x + 16;
        char buf[32];

        if (frame_stats) {
            uint32_t total_kb = frame_stats->total_frames * 4;
            uint32_t used_kb  = frame_stats->used_frames * 4;
            format_mem_kb(used_kb, buf);
            draw_stat_row(lx, sy, 140, "Phys. Used:", buf); sy += 22;

            format_mem_kb(kernel_free_mem / 1024, buf);
            draw_stat_row(lx, sy, 140, "Heap Free:", buf);
        } else {
            format_mem_kb(kernel_free_mem / 1024, buf);
            draw_stat_row(lx, sy, 140, "Heap Free:", buf);
        }
    }
}

/* ========================================================================
 * Main Paint Handler
 * ======================================================================== */

static void process_monitor_on_paint(window_t* win, int x, int y, int w, int h) {
    /* Background */
    gfx_fill_rect(x, y, w, h, COLOR_BG);

    /* Tab bar */
    draw_tab_bar(x, y, w);

    /* Toolbar */
    draw_toolbar(x, y + TAB_BAR_H, w);

    /* Content area */
    int content_y = y + TAB_BAR_H + TOOLBAR_H;
    int content_h = h - TAB_BAR_H - TOOLBAR_H - STATUS_BAR_H;

    /* Clip content background */
    gfx_fill_rect(x, content_y, w, content_h, COLOR_BG);

    switch (pm_state.current_tab) {
        case TAB_CPU:        draw_cpu_tab(x, content_y, w, content_h); break;
        case TAB_MEMORY:     draw_memory_tab(x, content_y, w, content_h); break;
        case TAB_PROCESSES:  draw_processes_tab(x, content_y, w, content_h); break;
        case TAB_SYSTEM:     draw_system_tab(x, content_y, w, content_h); break;
    }

    /* Status bar */
    draw_status_bar(x, y + h - STATUS_BAR_H, w);
}

/* ========================================================================
 * Input Handlers
 * ======================================================================== */

static void process_monitor_on_mouse(window_t* win, int mx, int my, int btn) {
    if (btn != 1) return;  /* Left click only */

    /* Tab bar click */
    if (my >= 0 && my < TAB_BAR_H) {
        int tab_w = pm_win_w / TAB_COUNT;
        int tab = mx / tab_w;
        if (tab >= 0 && tab < TAB_COUNT) {
            if (pm_state.current_tab != tab) {
                pm_state.current_tab = tab;
                pm_state.scroll_offset = 0;
            }
        }
        return;
    }

    /* Toolbar click */
    if (my >= TAB_BAR_H && my < TAB_BAR_H + TOOLBAR_H) {
        /* Refresh button */
        if (mx >= btn_refresh_x && mx < btn_refresh_x + btn_refresh_w &&
            my >= btn_refresh_y && my < btn_refresh_y + btn_refresh_h) {
            collect_process_data();
            sort_process_list();
            s_printf("[Activity Monitor] Manual refresh\n");
            return;
        }

        /* Kill Process button */
        if (mx >= btn_kill_x && mx < btn_kill_x + btn_kill_w &&
            my >= btn_kill_y && my < btn_kill_y + btn_kill_h) {
            if (pm_state.selected_pid > 0) {
                s_printf("[Activity Monitor] Killing PID %d\n", pm_state.selected_pid);
                process_kill(pm_state.selected_pid, SIGKILL);
                pm_state.selected_pid = -1;
                collect_process_data();
                sort_process_list();
            }
            return;
        }
        return;
    }

    /* Process table click (only in Processes tab) */
    if (pm_state.current_tab == TAB_PROCESSES) {
        int table_y = TAB_BAR_H + TOOLBAR_H;

        /* Header click for sorting */
        if (my >= table_y && my < table_y + TABLE_HEADER_H) {
            /* Column widths same as drawing */
            int col_x[COL_COUNT];
            int col_w[COL_COUNT];
            int cx = 8;
            col_x[COL_PID] = cx;      col_w[COL_PID] = 42;  cx += col_w[COL_PID];
            col_x[COL_NAME] = cx;     col_w[COL_NAME] = 140; cx += col_w[COL_NAME];
            col_x[COL_STATE] = cx;    col_w[COL_STATE] = 72;  cx += col_w[COL_STATE];
            col_x[COL_PRIORITY] = cx; col_w[COL_PRIORITY] = 56; cx += col_w[COL_PRIORITY];
            col_x[COL_CPU_TIME] = cx; col_w[COL_CPU_TIME] = 72; cx += col_w[COL_CPU_TIME];
            col_x[COL_UID] = cx;      col_w[COL_UID] = 40;

            for (int c = 0; c < COL_COUNT; c++) {
                if (mx >= col_x[c] && mx < col_x[c] + col_w[c]) {
                    if (pm_state.sort_column == c) {
                        pm_state.sort_ascending = !pm_state.sort_ascending;
                    } else {
                        pm_state.sort_column = c;
                        pm_state.sort_ascending = 1;
                    }
                    sort_process_list();
                    break;
                }
            }
            return;
        }

        /* Row click for selection */
        int row_start = table_y + TABLE_HEADER_H;
        if (my >= row_start) {
            int row_idx = (my - row_start) / ROW_H;
            int actual_idx = row_idx + pm_state.scroll_offset;
            if (actual_idx >= 0 && actual_idx < proc_count) {
                pm_state.selected_pid = proc_list[actual_idx].pid;
            }
            return;
        }
    }
}

static void process_monitor_on_input(window_t* win, int key) {
    if (key == 0) return;

    /* Tab shortcuts */
    if (key == '1') { pm_state.current_tab = TAB_CPU; pm_state.scroll_offset = 0; }
    else if (key == '2') { pm_state.current_tab = TAB_MEMORY; pm_state.scroll_offset = 0; }
    else if (key == '3') { pm_state.current_tab = TAB_PROCESSES; pm_state.scroll_offset = 0; }
    else if (key == '4') { pm_state.current_tab = TAB_SYSTEM; pm_state.scroll_offset = 0; }
    else if (key == 'r' || key == 'R') {
        collect_process_data();
        sort_process_list();
    }
    /* Arrow keys for process list navigation */
    else if (key == 128) { /* Up */
        if (pm_state.current_tab == TAB_PROCESSES && pm_state.selected_pid >= 0) {
            /* Find current selection and move up */
            for (int i = 0; i < proc_count; i++) {
                if (proc_list[i].pid == pm_state.selected_pid && i > 0) {
                    pm_state.selected_pid = proc_list[i - 1].pid;
                    if (i - 1 < pm_state.scroll_offset)
                        pm_state.scroll_offset = i - 1;
                    break;
                }
            }
        }
    }
    else if (key == 129) { /* Down */
        if (pm_state.current_tab == TAB_PROCESSES && pm_state.selected_pid >= 0) {
            for (int i = 0; i < proc_count; i++) {
                if (proc_list[i].pid == pm_state.selected_pid && i < proc_count - 1) {
                    pm_state.selected_pid = proc_list[i + 1].pid;
                    int max_visible = 12; /* approximate */
                    if (i + 1 >= pm_state.scroll_offset + max_visible)
                        pm_state.scroll_offset = i + 1 - max_visible + 1;
                    break;
                }
            }
        }
    }
}

static void process_monitor_on_scroll(window_t* win, int delta) {
    if (pm_state.current_tab == TAB_PROCESSES) {
        pm_state.scroll_offset -= delta * 3;
        if (pm_state.scroll_offset < 0) pm_state.scroll_offset = 0;
    }
}

static void process_monitor_on_resize(window_t* win, int new_w, int new_h) {
    pm_win_w = new_w;
}

/* ========================================================================
 * Menu Action Handler
 * ======================================================================== */

static void process_monitor_on_menu_action(int menu_id, int item_idx) {
    if (menu_id == 0) { /* File menu */
        if (item_idx == 0) { /* Refresh */
            collect_process_data();
            sort_process_list();
        }
    } else if (menu_id == 1) { /* View menu */
        if (item_idx >= 0 && item_idx < TAB_COUNT) {
            pm_state.current_tab = item_idx;
            pm_state.scroll_offset = 0;
        }
    }
}

/* ========================================================================
 * App Entry Point
 * ======================================================================== */

void init_process_monitor_app(void) {
    /* Initial data collection */
    collect_process_data();
    sort_process_list();

    s_printf("[Activity Monitor] Initialized with %d processes\n", proc_count);

    /* Create window */
    pm_window = fw_create_window("Activity Monitor", WIN_W, WIN_H,
                                  process_monitor_on_paint,
                                  process_monitor_on_input,
                                  process_monitor_on_mouse);
    if (!pm_window) {
        s_printf("[Activity Monitor] Failed to create window\n");
        return;
    }

    pm_window->min_w = 480;
    pm_window->scroll_callback = (void*)process_monitor_on_scroll;
    pm_window->resize_callback = (void*)process_monitor_on_resize;

    /* Setup menus */
    pm_window->menu_count = 2;

    strcpy(pm_window->menus[0].name, "File");
    strcpy(pm_window->menus[0].items[0].label, "Refresh");
    strcpy(pm_window->menus[0].items[1].label, "Close");
    pm_window->menus[0].item_count = 2;

    strcpy(pm_window->menus[1].name, "View");
    strcpy(pm_window->menus[1].items[0].label, "CPU");
    strcpy(pm_window->menus[1].items[1].label, "Memory");
    strcpy(pm_window->menus[1].items[2].label, "Processes");
    strcpy(pm_window->menus[1].items[3].label, "System");
    pm_window->menus[1].item_count = 4;

    pm_window->on_menu_action = (void*)process_monitor_on_menu_action;

    /* Register in dock */
    fw_register_dock("Monitor", 5, pm_window);
}
