// usr/apps/disk_utility.c - CamelOS Disk Utility App
// macOS-inspired disk management: verify, repair, erase, benchmark, surface scan
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/drivers/serial.h"
#include "../../hal/drivers/ata.h"
#include "../../fs/disk.h"
#include "../../fs/pfs32.h"
#include "../../installer/disk_tools.h"
#include "../../installer/disk_health.h"
#include "../dock.h"
#include "../../core/window_server.h"

// ============================================================================
// LAYOUT CONSTANTS
// ============================================================================
#define WIN_W           550
#define WIN_H           420
#define TOOLBAR_H       38
#define SIDEBAR_W       170
#define STATUSBAR_H     24
#define BTN_H           26
#define BTN_PAD         6

// Color palette (macOS-inspired)
#define COL_BG              0xFFFFFFFF
#define COL_SIDEBAR_BG      0xFFF2F2F7
#define COL_SIDEBAR_SEL     0xFF007AFF
#define COL_SIDEBAR_SEL_BG  0xFFD6E4FC
#define COL_TOOLBAR_BG      0xFFF7F7F8
#define COL_STATUSBAR_BG    0xFFF2F2F7
#define COL_SEPARATOR       0xFFD1D1D6
#define COL_TEXT_PRIMARY    0xFF1C1C1E
#define COL_TEXT_SECONDARY  0xFF8E8E93
#define COL_TEXT_ACCENT     0xFF007AFF
#define COL_BTN_BG         0xFFE8E8ED
#define COL_BTN_TEXT       0xFF333333
#define COL_BTN_DANGER_BG  0xFFFF453A
#define COL_BTN_DANGER_TXT 0xFFFFFFFF
#define COL_BAR_BG         0xFFE5E5EA
#define COL_BAR_GREEN      0xFF34C759
#define COL_BAR_YELLOW     0xFFFFCC00
#define COL_BAR_RED        0xFFFF3B30
#define COL_HEALTH_GOOD    0xFF34C759
#define COL_HEALTH_WARN    0xFFFF9500
#define COL_HEALTH_CRIT    0xFFFF3B30
#define COL_CARD_BG        0xFFF9F9FB
#define COL_CARD_BORDER    0xFFE5E5EA

// ============================================================================
// APP STATE
// ============================================================================
typedef struct {
    int selected_disk;
    int disk_count;
    struct {
        char name[32];
        uint32_t total_blocks;
        uint32_t used_blocks;
        uint32_t free_blocks;
        char fs_type[16];
        int healthy;
    } disks[4];
    char status_msg[128];
    int operation_running;
} disk_util_state_t;

static disk_util_state_t g_state;
static Window* g_win = 0;

// Confirmation dialog state
static int confirm_active = 0;    // 0=none, 1=erase
static int confirm_cursor_blink = 0;

// Benchmark results (cached)
static DiskBenchmark g_bench;
static int g_bench_valid = 0;

// Surface scan results (cached)
static SurfaceScan g_scan;
static int g_scan_valid = 0;

// ============================================================================
// HELPERS
// ============================================================================

// Format block count into human-readable size string
static void format_block_size(uint32_t blocks, char* out) {
    // Each block is 512 bytes
    uint32_t bytes = blocks * 512;
    char num[16];

    if (bytes >= 1073741824u) {
        // GB
        uint32_t gb = bytes / 1073741824u;
        uint32_t gb_frac = (bytes % 1073741824u) / 107374182u;
        int_to_str(gb, num);
        strcpy(out, num);
        strcat(out, ".");
        int_to_str(gb_frac, num);
        strcat(out, num);
        strcat(out, " GB");
    } else if (bytes >= 1048576) {
        // MB
        uint32_t mb = bytes / 1048576;
        int_to_str(mb, num);
        strcpy(out, num);
        strcat(out, " MB");
    } else if (bytes >= 1024) {
        // KB
        uint32_t kb = bytes / 1024;
        int_to_str(kb, num);
        strcpy(out, num);
        strcat(out, " KB");
    } else {
        int_to_str(bytes, num);
        strcpy(out, num);
        strcat(out, " B");
    }
}

// Draw a rounded button; returns the x position past the button
static int draw_button(int x, int y, int w, const char* label, uint32_t bg, uint32_t fg) {
    gfx_fill_rounded_rect(x, y, w, BTN_H, bg, 5);
    gfx_stroke_rounded_rect(x, y, w, BTN_H, 0x30000000, 5, 1);
    int text_w = strlen(label) * 8;
    gfx_draw_string(x + (w - text_w) / 2, y + (BTN_H - 12) / 2, label, fg);
    return x + w + BTN_PAD;
}

// Draw a labeled info row (label: value) and return next y
static int draw_info_row(int x, int y, const char* label, const char* value, uint32_t val_color) {
    gfx_draw_string(x, y, label, COL_TEXT_SECONDARY);
    gfx_draw_string(x + 110, y, value, val_color);
    return y + 20;
}

// ============================================================================
// DISK DISCOVERY
// ============================================================================

static void discover_disks(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.selected_disk = 0;
    g_state.disk_count = 0;

    s_printf("[DiskUtility] Discovering disks...\n");

    // Probe IDE devices
    for (int i = 0; i < 2; i++) {
        if (ide_devices[i].present && ide_devices[i].sectors > 0) {
            int idx = g_state.disk_count;
            strncpy(g_state.disks[idx].name, ide_devices[i].model, 31);
            g_state.disks[idx].name[31] = 0;
            // Trim trailing spaces from model name
            int len = strlen(g_state.disks[idx].name);
            while (len > 0 && g_state.disks[idx].name[len - 1] == ' ') {
                g_state.disks[idx].name[--len] = 0;
            }

            g_state.disks[idx].total_blocks = (uint32_t)ide_devices[i].sectors;

            // Query PFS32 stats for usage info
            pfs32_stats_t stats;
            if (pfs32_get_stats(&stats) == 0) {
                g_state.disks[idx].used_blocks = stats.total_sectors_used;
                g_state.disks[idx].free_blocks = stats.blocks_free;
                strcpy(g_state.disks[idx].fs_type, "PFS32");
                g_state.disks[idx].healthy = 1;
            } else {
                // Fallback: derive from total
                g_state.disks[idx].used_blocks = g_state.disks[idx].total_blocks / 2;
                g_state.disks[idx].free_blocks = g_state.disks[idx].total_blocks / 2;
                strcpy(g_state.disks[idx].fs_type, "Unknown");
                g_state.disks[idx].healthy = 0;
            }

            g_state.disk_count++;

            s_printf("[DiskUtility] Found disk: ");
            s_printf(g_state.disks[idx].name);
            s_printf("\n");
        }
    }

    // Always add the primary PFS32 volume if no IDE devices found
    if (g_state.disk_count == 0) {
        int idx = 0;
        strcpy(g_state.disks[idx].name, "CamelOS Disk");
        g_state.disks[idx].total_blocks = disk_total_blocks;

        pfs32_stats_t stats;
        if (pfs32_get_stats(&stats) == 0) {
            g_state.disks[idx].used_blocks = stats.total_sectors_used;
            g_state.disks[idx].free_blocks = stats.blocks_free;
            strcpy(g_state.disks[idx].fs_type, "PFS32");
            g_state.disks[idx].healthy = 1;
        } else {
            g_state.disks[idx].used_blocks = g_state.disks[idx].total_blocks / 2;
            g_state.disks[idx].free_blocks = g_state.disks[idx].total_blocks / 2;
            strcpy(g_state.disks[idx].fs_type, "PFS32");
            g_state.disks[idx].healthy = 1;
        }
        g_state.disk_count = 1;
    }

    // Add a virtual "Recovery HD" entry (macOS style)
    if (g_state.disk_count < 4) {
        int idx = g_state.disk_count;
        strcpy(g_state.disks[idx].name, "Recovery HD");
        g_state.disks[idx].total_blocks = 204800;  // ~100 MB
        g_state.disks[idx].used_blocks = 102400;
        g_state.disks[idx].free_blocks = 102400;
        strcpy(g_state.disks[idx].fs_type, "PFS32");
        g_state.disks[idx].healthy = 1;
        g_state.disk_count++;
    }

    strcpy(g_state.status_msg, "Ready");
    g_state.operation_running = 0;
}

// ============================================================================
// DRAWING: TOOLBAR
// ============================================================================

static void draw_toolbar(int x, int y, int w) {
    // Toolbar background
    gfx_fill_rect(x, y, w, TOOLBAR_H, COL_TOOLBAR_BG);
    gfx_draw_rect(x, y + TOOLBAR_H - 1, w, 1, COL_SEPARATOR);

    int bx = x + BTN_PAD;
    int by = y + (TOOLBAR_H - BTN_H) / 2;

    int running = g_state.operation_running;
    uint32_t btn_bg = running ? 0xFFD1D1D6 : COL_BTN_BG;
    uint32_t btn_fg = running ? 0xFF999999 : COL_BTN_TEXT;

    // Verify Disk
    bx = draw_button(bx, by, 72, "Verify", btn_bg, btn_fg);

    // Repair Disk
    bx = draw_button(bx, by, 66, "Repair", btn_bg, btn_fg);

    // Erase (danger)
    uint32_t erase_bg = running ? 0xFFD1D1D6 : COL_BTN_DANGER_BG;
    uint32_t erase_fg = running ? 0xFF999999 : COL_BTN_DANGER_TXT;
    bx = draw_button(bx, by, 56, "Erase", erase_bg, erase_fg);

    // Benchmark
    bx = draw_button(bx, by, 82, "Benchmark", btn_bg, btn_fg);

    // Surface Scan
    draw_button(bx, by, 90, "Surface Scan", btn_bg, btn_fg);
}

// ============================================================================
// DRAWING: SIDEBAR (disk list)
// ============================================================================

static void draw_sidebar(int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, COL_SIDEBAR_BG);
    // Right separator
    gfx_draw_rect(x + w - 1, y, 1, h, COL_SEPARATOR);

    // Header
    gfx_draw_string(x + 12, y + 8, "DISKS", COL_TEXT_SECONDARY);

    int item_y = y + 30;
    int item_h = 56;

    for (int i = 0; i < g_state.disk_count; i++) {
        int iy = item_y + i * item_h;
        if (iy + item_h > y + h) break;

        int is_sel = (i == g_state.selected_disk);

        // Selection highlight
        if (is_sel) {
            gfx_fill_rounded_rect(x + 4, iy, w - 10, item_h - 4, COL_SIDEBAR_SEL_BG, 6);
        }

        // Disk icon (small rounded rect representing a disk)
        int icon_x = x + 14;
        int icon_y = iy + 8;
        gfx_fill_rounded_rect(icon_x, icon_y, 28, 28, is_sel ? COL_SIDEBAR_SEL : 0xFF8E8E93, 5);
        // Disk label line on icon
        gfx_fill_rect(icon_x + 6, icon_y + 12, 16, 2, 0xFFFFFFFF);
        gfx_fill_rect(icon_x + 6, icon_y + 17, 12, 2, 0xFFFFFFFF);

        // Disk name
        gfx_draw_string(icon_x + 34, icon_y + 2,
                        g_state.disks[i].name,
                        is_sel ? COL_SIDEBAR_SEL : COL_TEXT_PRIMARY);

        // Filesystem type
        gfx_draw_string(icon_x + 34, icon_y + 14,
                        g_state.disks[i].fs_type,
                        COL_TEXT_SECONDARY);

        // Size
        char size_str[32];
        format_block_size(g_state.disks[i].total_blocks, size_str);
        gfx_draw_string(icon_x + 34, icon_y + 14 + 14,
                        size_str,
                        COL_TEXT_SECONDARY);
    }
}

// ============================================================================
// DRAWING: UTILIZATION BAR
// ============================================================================

static void draw_utilization_bar(int x, int y, int w, uint32_t used, uint32_t total) {
    int bar_h = 14;

    // Background track
    gfx_fill_rounded_rect(x, y, w, bar_h, COL_BAR_BG, 4);

    // Compute percentage
    uint32_t pct = 0;
    if (total > 0) {
        pct = (used * 100) / total;
        if (pct > 100) pct = 100;
    }

    // Fill bar
    int fill_w = (w * pct) / 100;
    if (fill_w < 2) fill_w = 2;
    if (fill_w > w) fill_w = w;

    // Color: green < 70%, yellow < 90%, red >= 90%
    uint32_t bar_color;
    if (pct < 70) {
        bar_color = COL_BAR_GREEN;
    } else if (pct < 90) {
        bar_color = COL_BAR_YELLOW;
    } else {
        bar_color = COL_BAR_RED;
    }

    gfx_fill_rounded_rect(x, y, fill_w, bar_h, bar_color, 4);

    // Percentage text centered
    char pct_str[16];
    int_to_str(pct, pct_str);
    strcat(pct_str, "%");
    int text_w = strlen(pct_str) * 8;
    int text_x = x + (w - text_w) / 2;
    // Use white text on colored bar, or dark on empty bar
    if (pct > 30) {
        gfx_draw_string(text_x, y + 1, pct_str, 0xFFFFFFFF);
    } else {
        gfx_draw_string(text_x, y + 1, pct_str, COL_TEXT_SECONDARY);
    }
}

// ============================================================================
// DRAWING: RIGHT PANEL (disk info)
// ============================================================================

static void draw_info_panel(int x, int y, int w, int h) {
    // Background
    gfx_fill_rect(x, y, w, h, COL_BG);

    if (g_state.disk_count == 0) {
        gfx_draw_string(x + 20, y + 30, "No disks found.", COL_TEXT_SECONDARY);
        return;
    }

    int idx = g_state.selected_disk;
    if (idx < 0 || idx >= g_state.disk_count) idx = 0;

    int cx = x + 20;
    int cy = y + 16;

    // ---- Disk Name Header ----
    gfx_draw_string_scaled(cx, cy, g_state.disks[idx].name, COL_TEXT_PRIMARY, 2);
    cy += 32;

    // Separator
    gfx_draw_rect(x + 16, cy, w - 32, 1, COL_SEPARATOR);
    cy += 12;

    // ---- Info Card ----
    int card_h = 150;
    gfx_fill_rounded_rect(cx - 4, cy, w - 32, card_h, COL_CARD_BG, 8);
    gfx_stroke_rounded_rect(cx - 4, cy, w - 32, card_h, COL_CARD_BORDER, 8, 1);

    int iy = cy + 12;

    // Name
    char name_buf[32];
    strncpy(name_buf, g_state.disks[idx].name, 31);
    name_buf[31] = 0;
    iy = draw_info_row(cx + 8, iy, "Name:", name_buf, COL_TEXT_PRIMARY);

    // Type
    iy = draw_info_row(cx + 8, iy, "Type:", g_state.disks[idx].fs_type, COL_TEXT_ACCENT);

    // Total Size
    char size_str[32];
    format_block_size(g_state.disks[idx].total_blocks, size_str);
    iy = draw_info_row(cx + 8, iy, "Total Size:", size_str, COL_TEXT_PRIMARY);

    // Used Space
    format_block_size(g_state.disks[idx].used_blocks, size_str);
    iy = draw_info_row(cx + 8, iy, "Used Space:", size_str, COL_TEXT_PRIMARY);

    // Free Space
    format_block_size(g_state.disks[idx].free_blocks, size_str);
    iy = draw_info_row(cx + 8, iy, "Free Space:", size_str, COL_TEXT_PRIMARY);

    // Block count
    char blocks_str[32];
    int_to_str(g_state.disks[idx].total_blocks, blocks_str);
    strcat(blocks_str, " blocks");
    iy = draw_info_row(cx + 8, iy, "Blocks:", blocks_str, COL_TEXT_SECONDARY);

    cy += card_h + 14;

    // ---- Utilization Bar ----
    gfx_draw_string(cx, cy, "Capacity", COL_TEXT_PRIMARY);
    cy += 18;

    draw_utilization_bar(cx, cy, w - 40,
                         g_state.disks[idx].used_blocks,
                         g_state.disks[idx].total_blocks);
    cy += 28;

    // ---- Disk Health ----
    gfx_draw_string(cx, cy, "Disk Health", COL_TEXT_PRIMARY);
    cy += 20;

    // Health status card
    int health_card_h = 36;
    gfx_fill_rounded_rect(cx - 4, cy, w - 32, health_card_h, COL_CARD_BG, 8);
    gfx_stroke_rounded_rect(cx - 4, cy, w - 32, health_card_h, COL_CARD_BORDER, 8, 1);

    // Health indicator dot + text
    int dot_x = cx + 8;
    int dot_y = cy + (health_card_h - 12) / 2;
    uint32_t dot_color;
    const char* health_text;

    // Try getting real health status
    HealthStatus hs = disk_health_get_status(idx);
    switch (hs) {
        case HEALTH_STATUS_GOOD:
            dot_color = COL_HEALTH_GOOD;
            health_text = "Verified - No problems found";
            break;
        case HEALTH_STATUS_WARNING:
            dot_color = COL_HEALTH_WARN;
            health_text = "Warning - Issues detected";
            break;
        case HEALTH_STATUS_CRITICAL:
            dot_color = COL_HEALTH_CRIT;
            health_text = "Critical - Replace disk soon";
            break;
        default:
            // Fallback: use our internal flag
            if (g_state.disks[idx].healthy) {
                dot_color = COL_HEALTH_GOOD;
                health_text = "Verified - No problems found";
            } else {
                dot_color = COL_HEALTH_WARN;
                health_text = "Unknown - Run verify to check";
            }
            break;
    }

    gfx_fill_rounded_rect(dot_x, dot_y, 12, 12, dot_color, 6);
    gfx_draw_string(dot_x + 20, dot_y, health_text, COL_TEXT_PRIMARY);

    cy += health_card_h + 14;

    // ---- Benchmark Results (if available) ----
    if (g_bench_valid) {
        gfx_draw_string(cx, cy, "Benchmark Results", COL_TEXT_PRIMARY);
        cy += 20;

        int bench_h = 56;
        gfx_fill_rounded_rect(cx - 4, cy, w - 32, bench_h, COL_CARD_BG, 8);
        gfx_stroke_rounded_rect(cx - 4, cy, w - 32, bench_h, COL_CARD_BORDER, 8, 1);

        char num_str[32];

        int_to_str(g_bench.read_speed_kb, num_str);
        strcat(num_str, " KB/s");
        draw_info_row(cx + 8, cy + 8, "Read:", num_str, COL_TEXT_PRIMARY);

        int_to_str(g_bench.write_speed_kb, num_str);
        strcat(num_str, " KB/s");
        draw_info_row(cx + 8, cy + 28, "Write:", num_str, COL_TEXT_PRIMARY);

        cy += bench_h + 10;
    }

    // ---- Surface Scan Results (if available) ----
    if (g_scan_valid) {
        gfx_draw_string(cx, cy, "Surface Scan Results", COL_TEXT_PRIMARY);
        cy += 20;

        int scan_h = 36;
        gfx_fill_rounded_rect(cx - 4, cy, w - 32, scan_h, COL_CARD_BG, 8);
        gfx_stroke_rounded_rect(cx - 4, cy, w - 32, scan_h, COL_CARD_BORDER, 8, 1);

        char num_str[32];
        int_to_str(g_scan.damaged_sectors, num_str);
        strcat(num_str, " damaged");

        uint32_t scan_col = (g_scan.damaged_sectors == 0) ? COL_HEALTH_GOOD : COL_HEALTH_CRIT;
        gfx_fill_rounded_rect(cx + 8, cy + 10, 12, 12, scan_col, 6);
        gfx_draw_string(cx + 26, cy + 10, num_str, COL_TEXT_PRIMARY);

        int_to_str(g_scan.slow_sectors, num_str);
        strcat(num_str, " slow");
        gfx_draw_string(cx + 26 + strlen(num_str) * 8 + 20, cy + 10, num_str, COL_TEXT_SECONDARY);

        cy += scan_h + 10;
    }
}

// ============================================================================
// DRAWING: STATUS BAR
// ============================================================================

static void draw_status_bar(int x, int y, int w) {
    gfx_fill_rect(x, y, w, STATUSBAR_H, COL_STATUSBAR_BG);
    gfx_draw_rect(x, y, w, 1, COL_SEPARATOR);

    // Status icon (small dot)
    uint32_t dot_col = COL_HEALTH_GOOD;
    if (g_state.operation_running) dot_col = COL_BAR_YELLOW;
    else if (strstr(g_state.status_msg, "Error") ||
             strstr(g_state.status_msg, "Failed") ||
             strstr(g_state.status_msg, "failed"))
        dot_col = COL_HEALTH_CRIT;

    gfx_fill_rounded_rect(x + 10, y + (STATUSBAR_H - 10) / 2, 8, 8, dot_col, 4);

    // Status text
    gfx_draw_string(x + 24, y + (STATUSBAR_H - 12) / 2, g_state.status_msg, COL_TEXT_SECONDARY);

    // Disk count on right
    char count_str[32];
    int_to_str(g_state.disk_count, count_str);
    strcat(count_str, " disk(s)");
    int cw = strlen(count_str) * 8;
    gfx_draw_string(x + w - cw - 12, y + (STATUSBAR_H - 12) / 2, count_str, COL_TEXT_SECONDARY);
}

// ============================================================================
// DRAWING: CONFIRMATION DIALOG (for Erase)
// ============================================================================

static void draw_confirm_dialog(int x, int y, int w, int h) {
    if (!confirm_active) return;

    // Dimmed overlay
    gfx_fill_rect(x, y, w, h, 0x40000000);

    // Dialog box
    int dw = 300, dh = 120;
    int dx = x + (w - dw) / 2;
    int dy = y + (h - dh) / 2;

    gfx_fill_rounded_rect(dx, dy, dw, dh, 0xFFFFFFFF, 12);
    gfx_stroke_rounded_rect(dx, dy, dw, dh, 0xFFD1D1D6, 12, 1);

    // Warning icon
    gfx_fill_rounded_rect(dx + 20, dy + 16, 24, 24, COL_BTN_DANGER_BG, 4);
    gfx_draw_string(dx + 28, dy + 20, "!", 0xFFFFFFFF);

    // Title
    gfx_draw_string(dx + 52, dy + 16, "Erase Disk", COL_TEXT_PRIMARY);
    // Subtitle
    if (g_state.selected_disk >= 0 && g_state.selected_disk < g_state.disk_count) {
        gfx_draw_string(dx + 52, dy + 32,
                        g_state.disks[g_state.selected_disk].name,
                        COL_TEXT_SECONDARY);
    }

    // Warning text
    gfx_draw_string(dx + 20, dy + 52, "This will permanently erase all data!", COL_BTN_DANGER_BG);

    // Buttons
    int bw = 80;
    // Cancel
    draw_button(dx + dw / 2 - bw - 16, dy + dh - 40, bw, "Cancel", COL_BTN_BG, COL_BTN_TEXT);
    // Erase (confirm)
    draw_button(dx + dw / 2 + 16, dy + dh - 40, bw, "Erase", COL_BTN_DANGER_BG, COL_BTN_DANGER_TXT);
}

// ============================================================================
// MAIN PAINT CALLBACK
// ============================================================================

static void disk_util_on_paint(window_t* win, int x, int y, int w, int h) {
    // Full background
    gfx_fill_rect(x, y, w, h, COL_BG);

    // Toolbar
    draw_toolbar(x, y, w);

    // Content area (between toolbar and statusbar)
    int content_y = y + TOOLBAR_H;
    int content_h = h - TOOLBAR_H - STATUSBAR_H;

    // Sidebar
    draw_sidebar(x, content_y, SIDEBAR_W, content_h);

    // Right info panel
    draw_info_panel(x + SIDEBAR_W, content_y, w - SIDEBAR_W, content_h);

    // Status bar
    draw_status_bar(x, y + h - STATUSBAR_H, w);

    // Confirmation dialog overlay
    if (confirm_active) {
        draw_confirm_dialog(x, y, w, h);
    }
}

// ============================================================================
// OPERATIONS
// ============================================================================

static void op_verify_disk(void) {
    if (g_state.operation_running) return;
    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Verifying disk...");

    s_printf("[DiskUtility] Starting verify (read-only fsck)...\n");

    int result = pfs32_fsck(0);  // read-only check

    if (result == 0) {
        strcpy(g_state.status_msg, "Verify complete: No problems found");
        s_printf("[DiskUtility] Verify: OK\n");
    } else {
        char buf[128];
        strcpy(buf, "Verify complete: Errors found (code ");
        char num[16];
        int_to_str(result, num);
        strcat(buf, num);
        strcat(buf, ")");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
        s_printf("[DiskUtility] Verify: errors found\n");
    }

    g_state.operation_running = 0;
}

static void op_repair_disk(void) {
    if (g_state.operation_running) return;
    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Repairing disk...");

    s_printf("[DiskUtility] Starting repair (fsck with repair)...\n");

    int result = pfs32_fsck(1);  // repair mode

    if (result == 0) {
        strcpy(g_state.status_msg, "Repair complete: Volume repaired successfully");
        s_printf("[DiskUtility] Repair: OK\n");
    } else {
        char buf[128];
        strcpy(buf, "Repair complete: Issues remain (code ");
        char num[16];
        int_to_str(result, num);
        strcat(buf, num);
        strcat(buf, ")");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
        s_printf("[DiskUtility] Repair: issues remain\n");
    }

    // Refresh disk info after repair
    discover_disks();
    g_state.operation_running = 0;
}

static void op_erase_disk(void) {
    if (g_state.operation_running) return;
    if (g_state.selected_disk < 0 || g_state.selected_disk >= g_state.disk_count) return;

    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Erasing disk...");

    s_printf("[DiskUtility] Formatting disk...\n");

    int result = pfs32_format("CamelOS", disk_total_blocks);

    if (result == 0) {
        strcpy(g_state.status_msg, "Erase complete: Disk formatted as PFS32");
        s_printf("[DiskUtility] Erase: OK\n");
    } else {
        char buf[128];
        strcpy(buf, "Erase failed (code ");
        char num[16];
        int_to_str(result, num);
        strcat(buf, num);
        strcat(buf, ")");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
        s_printf("[DiskUtility] Erase: failed\n");
    }

    // Refresh disk info after erase
    discover_disks();
    g_state.operation_running = 0;
}

static void op_benchmark(void) {
    if (g_state.operation_running) return;
    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Running benchmark...");

    s_printf("[DiskUtility] Starting disk benchmark...\n");

    disk_benchmark_init(&g_bench);
    int result = disk_benchmark_run(g_state.selected_disk, &g_bench, BENCHMARK_FULL);

    if (result == 0 || g_bench.test_complete) {
        g_bench_valid = 1;
        char buf[128];
        strcpy(buf, "Benchmark: Read ");
        char num[16];
        int_to_str(g_bench.read_speed_kb, num);
        strcat(buf, num);
        strcat(buf, " KB/s  Write ");
        int_to_str(g_bench.write_speed_kb, num);
        strcat(buf, num);
        strcat(buf, " KB/s");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
        s_printf("[DiskUtility] Benchmark: complete\n");
    } else {
        strcpy(g_state.status_msg, "Benchmark failed or not available");
        g_bench_valid = 0;
        s_printf("[DiskUtility] Benchmark: failed\n");
    }

    g_state.operation_running = 0;
}

static void op_surface_scan(void) {
    if (g_state.operation_running) return;
    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Running surface scan...");

    s_printf("[DiskUtility] Starting surface scan...\n");

    surface_scan_init(&g_scan);
    int result = surface_scan_start(g_state.selected_disk, &g_scan, 0, disk_total_blocks);

    if (result == 0 || g_scan.scan_complete) {
        g_scan_valid = 1;
        char buf[128];
        strcpy(buf, "Scan complete: ");
        char num[16];
        int_to_str(g_scan.damaged_sectors, num);
        strcat(buf, num);
        strcat(buf, " damaged, ");
        int_to_str(g_scan.slow_sectors, num);
        strcat(buf, num);
        strcat(buf, " slow sectors");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
        s_printf("[DiskUtility] Surface scan: complete\n");
    } else {
        strcpy(g_state.status_msg, "Surface scan failed or not available");
        g_scan_valid = 0;
        s_printf("[DiskUtility] Surface scan: failed\n");
    }

    g_state.operation_running = 0;
}

// ============================================================================
// MOUSE HANDLER
// ============================================================================

static void disk_util_on_mouse(window_t* win, int x, int y, int btn) {
    if (btn != 1) return;

    // ---- Confirmation dialog active? ----
    if (confirm_active) {
        int dw = 300, dh = 120;
        int dx = (WIN_W - dw) / 2;
        int dy = (WIN_H - dh) / 2;
        int bw = 80;

        // Cancel button
        int cancel_x = dx + dw / 2 - bw - 16;
        int cancel_y = dy + dh - 40;
        if (x >= cancel_x && x < cancel_x + bw && y >= cancel_y && y < cancel_y + BTN_H) {
            confirm_active = 0;
            strcpy(g_state.status_msg, "Erase cancelled");
            return;
        }

        // Erase (confirm) button
        int erase_x = dx + dw / 2 + 16;
        int erase_y = dy + dh - 40;
        if (x >= erase_x && x < erase_x + bw && y >= erase_y && y < erase_y + BTN_H) {
            confirm_active = 0;
            op_erase_disk();
            return;
        }
        return;  // Consume click while dialog is open
    }

    // ---- Toolbar clicks ----
    if (y >= 0 && y < TOOLBAR_H) {
        int bx = BTN_PAD;
        int by = (TOOLBAR_H - BTN_H) / 2;

        // Verify
        if (x >= bx && x < bx + 72) {
            op_verify_disk();
            return;
        }
        bx += 72 + BTN_PAD;

        // Repair
        if (x >= bx && x < bx + 66) {
            op_repair_disk();
            return;
        }
        bx += 66 + BTN_PAD;

        // Erase - show confirmation
        if (x >= bx && x < bx + 56) {
            if (!g_state.operation_running) {
                confirm_active = 1;
                strcpy(g_state.status_msg, "Confirm disk erase?");
            }
            return;
        }
        bx += 56 + BTN_PAD;

        // Benchmark
        if (x >= bx && x < bx + 82) {
            op_benchmark();
            return;
        }
        bx += 82 + BTN_PAD;

        // Surface Scan
        if (x >= bx && x < bx + 90) {
            op_surface_scan();
            return;
        }

        return;
    }

    // ---- Sidebar clicks (disk selection) ----
    int content_y = TOOLBAR_H;
    int content_h = WIN_H - TOOLBAR_H - STATUSBAR_H;

    if (x < SIDEBAR_W && y >= content_y && y < content_y + content_h) {
        int item_y = content_y + 30;
        int item_h = 56;
        int clicked = (y - item_y) / item_h;
        if (clicked >= 0 && clicked < g_state.disk_count) {
            g_state.selected_disk = clicked;
            g_bench_valid = 0;  // Reset benchmark display for new disk
            g_scan_valid = 0;   // Reset scan display for new disk
            char buf[64];
            strcpy(buf, "Selected: ");
            strcat(buf, g_state.disks[clicked].name);
            strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
            g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
        }
    }
}

// ============================================================================
// INPUT HANDLER
// ============================================================================

static void disk_util_on_input(window_t* win, int key) {
    if (key == 0) return;

    // Escape cancels confirmation dialog
    if (confirm_active) {
        if (key == 27) {
            confirm_active = 0;
            strcpy(g_state.status_msg, "Erase cancelled");
        }
        return;
    }

    // Keyboard shortcuts
    switch (key) {
        case 'v': case 'V':
            op_verify_disk();
            break;
        case 'r': case 'R':
            op_repair_disk();
            break;
        case 'b': case 'B':
            op_benchmark();
            break;
        case 's': case 'S':
            op_surface_scan();
            break;
        case 128:  // KEY_UP
            if (g_state.selected_disk > 0) {
                g_state.selected_disk--;
                g_bench_valid = 0;
                g_scan_valid = 0;
            }
            break;
        case 129:  // KEY_DOWN
            if (g_state.selected_disk < g_state.disk_count - 1) {
                g_state.selected_disk++;
                g_bench_valid = 0;
                g_scan_valid = 0;
            }
            break;
    }
}

// ============================================================================
// SCROLL HANDLER
// ============================================================================

static void disk_util_on_scroll(window_t* win, int delta) {
    // Scroll changes selected disk in sidebar
    int new_sel = g_state.selected_disk - delta;
    if (new_sel < 0) new_sel = 0;
    if (new_sel >= g_state.disk_count) new_sel = g_state.disk_count - 1;
    if (new_sel != g_state.selected_disk) {
        g_state.selected_disk = new_sel;
        g_bench_valid = 0;
        g_scan_valid = 0;
    }
}

// ============================================================================
// MENU ACTION HANDLER
// ============================================================================

static void disk_util_on_menu_action(int menu_id, int item_idx) {
    if (menu_id == 0) {  // File menu
        if (item_idx == 0) {  // Refresh
            discover_disks();
            g_bench_valid = 0;
            g_scan_valid = 0;
            strcpy(g_state.status_msg, "Disk list refreshed");
        }
        // item 1 = Close (handled by window server)
    } else if (menu_id == 1) {  // Disk menu
        switch (item_idx) {
            case 0: op_verify_disk(); break;
            case 1: op_repair_disk(); break;
            case 2:  // Erase from menu - show confirmation
                if (!g_state.operation_running) {
                    confirm_active = 1;
                    strcpy(g_state.status_msg, "Confirm disk erase?");
                }
                break;
            case 3: op_benchmark(); break;
            case 4: op_surface_scan(); break;
        }
    }
}

// ============================================================================
// APP ENTRY POINT
// ============================================================================

void init_disk_utility_app(void) {
    s_printf("[DiskUtility] Initializing Disk Utility app...\n");

    // Discover available disks
    discover_disks();

    // Create main window
    g_win = fw_create_window("Disk Utility", WIN_W, WIN_H,
                              disk_util_on_paint,
                              disk_util_on_input,
                              disk_util_on_mouse);
    if (!g_win) {
        s_printf("[DiskUtility] Failed to create window!\n");
        return;
    }

    window_t* w = (window_t*)g_win;
    w->min_w = 420;

    // Wire up callbacks
    w->scroll_callback = (void*)disk_util_on_scroll;
    w->on_menu_action = (void*)disk_util_on_menu_action;

    // Set up menus
    w->menu_count = 2;

    // File menu
    strcpy(w->menus[0].name, "File");
    strcpy(w->menus[0].items[0].label, "Refresh");
    strcpy(w->menus[0].items[1].label, "Close");
    w->menus[0].item_count = 2;

    // Disk menu
    strcpy(w->menus[1].name, "Disk");
    strcpy(w->menus[1].items[0].label, "Verify Disk");
    strcpy(w->menus[1].items[1].label, "Repair Disk");
    strcpy(w->menus[1].items[2].label, "Erase Disk");
    strcpy(w->menus[1].items[3].label, "Benchmark");
    strcpy(w->menus[1].items[4].label, "Surface Scan");
    w->menus[1].item_count = 5;

    // Register in dock
    fw_register_dock("Disk Utility", 2, g_win);

    s_printf("[DiskUtility] Ready. Found ");
    char num[8];
    int_to_str(g_state.disk_count, num);
    s_printf(num);
    s_printf(" disk(s)\n");
}
