// usr/apps/disk_utility.c - CamelOS Disk Utility App
// macOS-inspired disk management with pizza chart visualization
// Features: verify, repair, erase, mount/unmount, disk info, pie chart
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/video/gfx_hal.h"
#include "../../core/window_server.h"
#include "../dock.h"
#include "../../fs/pfs32.h"
#include "../../hal/drivers/serial.h"

// ============================================================================
// LAYOUT CONSTANTS
// ============================================================================
#define WIN_W           600
#define WIN_H           450
#define TOOLBAR_H       38
#define SIDEBAR_W       170
#define STATUSBAR_H     24
#define BTN_H           26
#define BTN_PAD         6

// ============================================================================
// COLOR PALETTE (macOS-inspired, ARGB)
// ============================================================================
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
#define COL_CARD_BG        0xFFF9F9FB
#define COL_CARD_BORDER    0xFFE5E5EA
#define COL_CHART_USED     0xFF007AFF   // macOS blue for used space
#define COL_CHART_FREE     0xFFE8E8ED   // Light gray for free space
#define COL_CHART_BORDER   0xFFD1D1D6   // Border around chart

// ============================================================================
// APP STATE
// ============================================================================
typedef struct {
    char name[32];
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    char fs_type[16];
    int mounted;
    int healthy;
} disk_entry_t;

typedef struct {
    int selected_disk;
    int disk_count;
    disk_entry_t disks[4];
    char status_msg[128];
    int operation_running;
} disk_util_state_t;

static disk_util_state_t g_state;
static window_t* g_win = 0;

// Confirmation dialog state
static int confirm_active = 0;    // 0=none, 1=erase

// ============================================================================
// INTEGER MATH HELPERS (no <math.h>)
// ============================================================================

// Integer square root using Newton's method
static int isqrt(int n) {
    if (n <= 0) return 0;
    if (n < 4) return 1;
    int x = n / 2;
    for (int i = 0; i < 16; i++) {
        int nx = (x + n / x) / 2;
        if (nx >= x) break;
        x = nx;
    }
    return x;
}

// Integer atan2 — returns degrees 0..359 counter-clockwise from east
// Standard math convention: atan2(y, x)
static int iatan2_deg(int y, int x) {
    int ax = x < 0 ? -x : x;
    int ay = y < 0 ? -y : y;

    if (ax + ay == 0) return 0;

    int angle;
    if (ax > ay) {
        // Closer to horizontal: small angle from x-axis
        angle = (ay * 45) / ax;          // 0..45
    } else {
        // Closer to vertical: large angle from x-axis
        angle = 90 - (ax * 45) / ay;     // 45..90
    }

    // Adjust for quadrant
    if (x < 0)  angle = 180 - angle;
    if (y < 0)  angle = 360 - angle;

    return angle % 360;
}

// Angle clockwise from north (12 o'clock) in degrees 0..359
// In screen coords: north = dy negative, east = dx positive
static int angle_cw_from_north(int dy, int dx) {
    // CW from north = standard atan2(dx, -dy)
    return iatan2_deg(dx, -dy);
}

// ============================================================================
// PIZZA / PIE CHART (standalone, reusable)
// ============================================================================

// Draw a Bresenham circle outline (1px thick)
static void draw_circle_outline(int cx, int cy, int r, uint32_t color) {
    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        gfx_put_pixel(cx + x, cy + y, color);
        gfx_put_pixel(cx - x, cy + y, color);
        gfx_put_pixel(cx + x, cy - y, color);
        gfx_put_pixel(cx - x, cy - y, color);
        gfx_put_pixel(cx + y, cy + x, color);
        gfx_put_pixel(cx - y, cy + x, color);
        gfx_put_pixel(cx + y, cy - x, color);
        gfx_put_pixel(cx - y, cy - x, color);

        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

// Draw a filled pizza/pie chart centered at (cx, cy) with given radius.
// used_pct: 0..100 percentage of disk used
// used_color / free_color: ARGB fill colors for each sector
//
// Algorithm: scanline rasterization with per-pixel angle classification.
// For each row, compute the horizontal span within the circle, then for
// each pixel determine its angle clockwise from 12 o'clock.  Pixels
// whose angle < used_pct*360/100 get used_color, others get free_color.
// Consecutive same-color pixels on a scanline are batched into a single
// gfx_fill_rect call for performance.
void draw_pizza_chart(int cx, int cy, int radius, int used_pct,
                      uint32_t used_color, uint32_t free_color)
{
    if (radius <= 0) return;
    if (used_pct < 0)  used_pct = 0;
    if (used_pct > 100) used_pct = 100;

    // Boundary angle in degrees (CW from north)
    int boundary_deg = (used_pct * 360) / 100;

    int r2 = radius * radius;

    // ---- Fill sectors scanline by scanline ----
    for (int dy = -radius; dy <= radius; dy++) {
        int y = cy + dy;
        int dx2 = r2 - dy * dy;
        if (dx2 < 0) continue;
        int dx = isqrt(dx2);

        int left  = cx - dx;
        int right = cx + dx;

        // Walk across the span, batching consecutive same-color pixels
        int run_start  = left;
        int run_is_used = -1;   // -1 = no run yet

        for (int px = left; px <= right; px++) {
            int ddx = px - cx;
            int ang = angle_cw_from_north(dy, ddx);
            int is_used = (ang < boundary_deg) ? 1 : 0;

            if (is_used != run_is_used) {
                // Flush previous run
                if (run_is_used >= 0 && px > run_start) {
                    uint32_t col = run_is_used ? used_color : free_color;
                    gfx_fill_rect(run_start, y, px - run_start, 1, col);
                }
                run_start  = px;
                run_is_used = is_used;
            }
        }
        // Flush last run on this scanline
        if (run_is_used >= 0) {
            uint32_t col = run_is_used ? used_color : free_color;
            gfx_fill_rect(run_start, y, (right - run_start) + 1, 1, col);
        }
    }

    // ---- Thin border circle ----
    draw_circle_outline(cx, cy, radius, COL_CHART_BORDER);
    // Inner highlight ring for depth
    if (radius > 4) {
        draw_circle_outline(cx, cy, radius - 1, COL_CHART_BORDER);
    }
}

// ============================================================================
// HELPERS
// ============================================================================

// Format block count into human-readable size string
static void format_block_size(uint32_t blocks, char* out) {
    uint32_t bytes = blocks * 512;
    char num[16];

    if (bytes >= 1073741824u) {
        uint32_t gb = bytes / 1073741824u;
        uint32_t gb_frac = (bytes % 1073741824u) / 107374182u;
        int_to_str(gb, num);
        strcpy(out, num);
        strcat(out, ".");
        int_to_str(gb_frac, num);
        strcat(out, num);
        strcat(out, " GB");
    } else if (bytes >= 1048576) {
        uint32_t mb = bytes / 1048576;
        int_to_str(mb, num);
        strcpy(out, num);
        strcat(out, " MB");
    } else if (bytes >= 1024) {
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
static int draw_button(int x, int y, int w, const char* label,
                       uint32_t bg, uint32_t fg)
{
    gfx_fill_rounded_rect(x, y, w, BTN_H, bg, 5);
    gfx_stroke_rounded_rect(x, y, w, BTN_H, 0x30000000, 5, 1);
    int text_w = strlen(label) * 8;
    gfx_draw_string(x + (w - text_w) / 2, y + (BTN_H - 12) / 2, label, fg);
    return x + w + BTN_PAD;
}

// Draw a labeled info row (label: value) and return next y
static int draw_info_row(int x, int y, const char* label,
                         const char* value, uint32_t val_color)
{
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

    // Try to read PFS32 stats for the primary volume
    pfs32_stats_t stats;
    int have_stats = (pfs32_get_stats(&stats) == 0);

    // Primary CamelOS Disk
    {
        int idx = g_state.disk_count;
        strcpy(g_state.disks[idx].name, "CamelOS Disk");

        if (have_stats) {
            // Derive total from used + free
            g_state.disks[idx].used_blocks  = stats.total_sectors_used;
            g_state.disks[idx].free_blocks  = stats.blocks_free;
            g_state.disks[idx].total_blocks = stats.total_sectors_used + stats.blocks_free;
            strcpy(g_state.disks[idx].fs_type, "PFS32");
            g_state.disks[idx].healthy = 1;
        } else {
            // Fallback: use memory-based estimate
            extern uint32_t k_get_total_mem(void);
            uint32_t total_mem = k_get_total_mem();
            g_state.disks[idx].total_blocks = (total_mem > 0) ? total_mem / 512 : 8192;
            g_state.disks[idx].used_blocks  = g_state.disks[idx].total_blocks / 2;
            g_state.disks[idx].free_blocks  = g_state.disks[idx].total_blocks / 2;
            strcpy(g_state.disks[idx].fs_type, "PFS32");
            g_state.disks[idx].healthy = 1;
        }
        g_state.disks[idx].mounted = 1;
        g_state.disk_count++;
    }

    // Recovery HD entry (macOS style)
    if (g_state.disk_count < 4) {
        int idx = g_state.disk_count;
        strcpy(g_state.disks[idx].name, "Recovery HD");
        g_state.disks[idx].total_blocks = 204800;   // ~100 MB
        g_state.disks[idx].used_blocks  = 102400;
        g_state.disks[idx].free_blocks  = 102400;
        strcpy(g_state.disks[idx].fs_type, "PFS32");
        g_state.disks[idx].mounted = 1;
        g_state.disks[idx].healthy = 1;
        g_state.disk_count++;
    }

    // Virtual disk image (macOS style .dmg)
    if (g_state.disk_count < 4) {
        int idx = g_state.disk_count;
        strcpy(g_state.disks[idx].name, "Install Media");
        g_state.disks[idx].total_blocks = 65536;    // ~32 MB
        g_state.disks[idx].used_blocks  = 49152;
        g_state.disks[idx].free_blocks  = 16384;
        strcpy(g_state.disks[idx].fs_type, "PFS32");
        g_state.disks[idx].mounted = 0;   // Not mounted by default
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

    // Mount / Unmount (toggle label)
    int idx = g_state.selected_disk;
    const char* mount_label = "Unmount";
    if (idx >= 0 && idx < g_state.disk_count && !g_state.disks[idx].mounted) {
        mount_label = "Mount";
    }
    draw_button(bx, by, 78, mount_label, btn_bg, btn_fg);
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
            gfx_fill_rounded_rect(x + 4, iy, w - 10, item_h - 4,
                                  COL_SIDEBAR_SEL_BG, 6);
        }

        // Disk icon (small rounded rect representing a disk drive)
        int icon_x = x + 14;
        int icon_y = iy + 8;
        uint32_t icon_bg = is_sel ? COL_SIDEBAR_SEL : 0xFF8E8E93;
        if (!g_state.disks[i].mounted) icon_bg = 0xFFD1D1D6;
        gfx_fill_rounded_rect(icon_x, icon_y, 28, 28, icon_bg, 5);
        // Disk label lines on icon
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
        gfx_draw_string(icon_x + 34, icon_y + 26, size_str,
                        COL_TEXT_SECONDARY);
    }
}

// ============================================================================
// DRAWING: RIGHT PANEL (disk info + pizza chart)
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
    int cy = y + 14;

    // ---- Disk Name Header ----
    gfx_draw_string_scaled(cx, cy, g_state.disks[idx].name,
                           COL_TEXT_PRIMARY, 2);
    cy += 30;

    // Mount status badge
    if (g_state.disks[idx].mounted) {
        gfx_fill_rounded_rect(cx + strlen(g_state.disks[idx].name) * 16 + 12,
                              cy - 24, 52, 18, 0xFFE8F5E9, 4);
        gfx_draw_string(cx + strlen(g_state.disks[idx].name) * 16 + 18,
                        cy - 22, "Mounted", COL_HEALTH_GOOD);
    } else {
        gfx_fill_rounded_rect(cx + strlen(g_state.disks[idx].name) * 16 + 12,
                              cy - 24, 64, 18, 0xFFFFEBEE, 4);
        gfx_draw_string(cx + strlen(g_state.disks[idx].name) * 16 + 18,
                        cy - 22, "Unmounted", COL_BAR_RED);
    }

    // Separator
    gfx_draw_rect(x + 16, cy, w - 32, 1, COL_SEPARATOR);
    cy += 10;

    // ---- Pizza Chart ----
    // Compute used percentage
    uint32_t used_pct = 0;
    if (g_state.disks[idx].total_blocks > 0) {
        used_pct = (g_state.disks[idx].used_blocks * 100)
                 / g_state.disks[idx].total_blocks;
        if (used_pct > 100) used_pct = 100;
    }

    // Choose chart color based on usage
    uint32_t chart_used_color = COL_CHART_USED;
    if (used_pct >= 90)      chart_used_color = COL_BAR_RED;
    else if (used_pct >= 70) chart_used_color = COL_BAR_YELLOW;

    int chart_radius = 68;
    int chart_cx = x + w / 2;
    int chart_cy = cy + chart_radius + 4;

    draw_pizza_chart(chart_cx, chart_cy, chart_radius, used_pct,
                     chart_used_color, COL_CHART_FREE);

    // Center label inside chart (percentage)
    char pct_str[16];
    int_to_str(used_pct, pct_str);
    strcat(pct_str, "%");
    int pct_w = strlen(pct_str) * 8 * 2;
    gfx_draw_string_scaled(chart_cx - pct_w / 2, chart_cy - 14,
                           pct_str, COL_TEXT_PRIMARY, 2);

    cy = chart_cy + chart_radius + 12;

    // ---- Legend ----
    int legend_x = chart_cx - 80;
    // Used swatch
    gfx_fill_rounded_rect(legend_x, cy, 14, 14, chart_used_color, 3);
    char legend_used[32];
    strcpy(legend_used, "Used ");
    char num_tmp[16];
    format_block_size(g_state.disks[idx].used_blocks, num_tmp);
    strcat(legend_used, num_tmp);
    gfx_draw_string(legend_x + 20, cy + 1, legend_used, COL_TEXT_PRIMARY);

    // Free swatch
    int free_legend_x = legend_x + 160;
    gfx_fill_rounded_rect(free_legend_x, cy, 14, 14, COL_CHART_FREE, 3);
    char legend_free[32];
    strcpy(legend_free, "Free ");
    format_block_size(g_state.disks[idx].free_blocks, num_tmp);
    strcat(legend_free, num_tmp);
    gfx_draw_string(free_legend_x + 20, cy + 1, legend_free, COL_TEXT_PRIMARY);

    cy += 28;

    // Separator
    gfx_draw_rect(x + 16, cy, w - 32, 1, COL_SEPARATOR);
    cy += 10;

    // ---- Info Card ----
    int card_h = 120;
    gfx_fill_rounded_rect(cx - 4, cy, w - 32, card_h, COL_CARD_BG, 8);
    gfx_stroke_rounded_rect(cx - 4, cy, w - 32, card_h, COL_CARD_BORDER, 8, 1);

    int iy = cy + 10;

    // Filesystem Type
    iy = draw_info_row(cx + 8, iy, "Filesystem:", g_state.disks[idx].fs_type,
                       COL_TEXT_ACCENT);

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
        dot_col = COL_BAR_RED;

    gfx_fill_rounded_rect(x + 10, y + (STATUSBAR_H - 10) / 2,
                          8, 8, dot_col, 4);

    // Status text
    gfx_draw_string(x + 24, y + (STATUSBAR_H - 12) / 2,
                    g_state.status_msg, COL_TEXT_SECONDARY);

    // Disk count on right
    char count_str[32];
    int_to_str(g_state.disk_count, count_str);
    strcat(count_str, " disk(s)");
    int cw = strlen(count_str) * 8;
    gfx_draw_string(x + w - cw - 12, y + (STATUSBAR_H - 12) / 2,
                    count_str, COL_TEXT_SECONDARY);
}

// ============================================================================
// DRAWING: CONFIRMATION DIALOG (for Erase)
// ============================================================================

static void draw_confirm_dialog(int x, int y, int w, int h) {
    if (!confirm_active) return;

    // Dimmed overlay
    gfx_fill_rect(x, y, w, h, 0x40000000);

    // Dialog box
    int dw = 300, dh = 140;
    int dx = x + (w - dw) / 2;
    int dy = y + (h - dh) / 2;

    gfx_fill_rounded_rect(dx, dy, dw, dh, 0xFFFFFFFF, 12);
    gfx_stroke_rounded_rect(dx, dy, dw, dh, 0xFFD1D1D6, 12, 1);

    // Warning icon
    gfx_fill_rounded_rect(dx + 20, dy + 16, 24, 24, COL_BTN_DANGER_BG, 4);
    gfx_draw_string(dx + 28, dy + 20, "!", 0xFFFFFFFF);

    // Title
    gfx_draw_string(dx + 52, dy + 16, "Erase Disk", COL_TEXT_PRIMARY);

    // Subtitle with disk name
    if (g_state.selected_disk >= 0 && g_state.selected_disk < g_state.disk_count) {
        gfx_draw_string(dx + 52, dy + 32,
                        g_state.disks[g_state.selected_disk].name,
                        COL_TEXT_SECONDARY);
    }

    // Warning text
    gfx_draw_string(dx + 20, dy + 58,
                    "This will permanently erase all data!",
                    COL_BTN_DANGER_BG);

    // Buttons
    int bw = 80;
    draw_button(dx + dw / 2 - bw - 16, dy + dh - 44, bw, "Cancel",
                COL_BTN_BG, COL_BTN_TEXT);
    draw_button(dx + dw / 2 + 16, dy + dh - 44, bw, "Erase",
                COL_BTN_DANGER_BG, COL_BTN_DANGER_TXT);
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

    // Right info panel with pizza chart
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

    int result = pfs32_fsck(0);  // read-only check

    if (result == 0) {
        strcpy(g_state.status_msg, "Verify complete: No problems found");
    } else {
        char buf[128];
        strcpy(buf, "Verify: Errors found (code ");
        char num[16];
        int_to_str(result, num);
        strcat(buf, num);
        strcat(buf, ")");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
    }

    g_state.operation_running = 0;
}

static void op_repair_disk(void) {
    if (g_state.operation_running) return;
    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Repairing disk...");

    int result = pfs32_fsck(1);  // repair mode

    if (result == 0) {
        strcpy(g_state.status_msg, "Repair complete: Volume repaired");
    } else {
        char buf[128];
        strcpy(buf, "Repair: Issues remain (code ");
        char num[16];
        int_to_str(result, num);
        strcat(buf, num);
        strcat(buf, ")");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
    }

    // Refresh disk info after repair
    discover_disks();
    g_state.operation_running = 0;
}

static void op_erase_disk(void) {
    if (g_state.operation_running) return;
    if (g_state.selected_disk < 0 || g_state.selected_disk >= g_state.disk_count)
        return;

    g_state.operation_running = 1;
    strcpy(g_state.status_msg, "Erasing disk...");

    // Use total_blocks from disk state
    int result = pfs32_format("CamelOS", g_state.disks[g_state.selected_disk].total_blocks);

    if (result == 0) {
        strcpy(g_state.status_msg, "Erase complete: Disk formatted as PFS32");
    } else {
        char buf[128];
        strcpy(buf, "Erase failed (code ");
        char num[16];
        int_to_str(result, num);
        strcat(buf, num);
        strcat(buf, ")");
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
    }

    // Refresh disk info after erase
    discover_disks();
    g_state.operation_running = 0;
}

static void op_toggle_mount(void) {
    if (g_state.operation_running) return;
    if (g_state.selected_disk < 0 || g_state.selected_disk >= g_state.disk_count)
        return;

    int idx = g_state.selected_disk;

    if (g_state.disks[idx].mounted) {
        // Unmount
        g_state.disks[idx].mounted = 0;
        char buf[64];
        strcpy(buf, "Unmounted: ");
        strcat(buf, g_state.disks[idx].name);
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
    } else {
        // Mount
        g_state.disks[idx].mounted = 1;
        char buf[64];
        strcpy(buf, "Mounted: ");
        strcat(buf, g_state.disks[idx].name);
        strncpy(g_state.status_msg, buf, sizeof(g_state.status_msg) - 1);
        g_state.status_msg[sizeof(g_state.status_msg) - 1] = 0;
    }
}

// ============================================================================
// MOUSE HANDLER
// ============================================================================

static void disk_util_on_mouse(window_t* win, int x, int y, int btn) {
    if (btn != 1) return;

    // ---- Confirmation dialog active? ----
    if (confirm_active) {
        int dw = 300, dh = 140;
        int dx = (WIN_W - dw) / 2;
        int dy = (WIN_H - dh) / 2;
        int bw = 80;

        // Cancel button
        int cancel_x = dx + dw / 2 - bw - 16;
        int cancel_y = dy + dh - 44;
        if (x >= cancel_x && x < cancel_x + bw &&
            y >= cancel_y && y < cancel_y + BTN_H) {
            confirm_active = 0;
            strcpy(g_state.status_msg, "Erase cancelled");
            return;
        }

        // Erase (confirm) button
        int erase_x = dx + dw / 2 + 16;
        int erase_y = dy + dh - 44;
        if (x >= erase_x && x < erase_x + bw &&
            y >= erase_y && y < erase_y + BTN_H) {
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

        // Verify (width 72)
        if (x >= bx && x < bx + 72) {
            op_verify_disk();
            return;
        }
        bx += 72 + BTN_PAD;

        // Repair (width 66)
        if (x >= bx && x < bx + 66) {
            op_repair_disk();
            return;
        }
        bx += 66 + BTN_PAD;

        // Erase (width 56) — show confirmation
        if (x >= bx && x < bx + 56) {
            if (!g_state.operation_running) {
                confirm_active = 1;
                strcpy(g_state.status_msg, "Confirm disk erase?");
            }
            return;
        }
        bx += 56 + BTN_PAD;

        // Mount/Unmount (width 78)
        if (x >= bx && x < bx + 78) {
            op_toggle_mount();
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
        case 'm': case 'M':
            op_toggle_mount();
            break;
        case 128:  // KEY_UP
            if (g_state.selected_disk > 0)
                g_state.selected_disk--;
            break;
        case 129:  // KEY_DOWN
            if (g_state.selected_disk < g_state.disk_count - 1)
                g_state.selected_disk++;
            break;
    }
}

// ============================================================================
// SCROLL HANDLER
// ============================================================================

static void disk_util_on_scroll(window_t* win, int delta) {
    int new_sel = g_state.selected_disk - delta;
    if (new_sel < 0) new_sel = 0;
    if (new_sel >= g_state.disk_count) new_sel = g_state.disk_count - 1;
    if (new_sel != g_state.selected_disk) {
        g_state.selected_disk = new_sel;
    }
}

// ============================================================================
// APP ENTRY POINT
// ============================================================================

void init_disk_utility_app(void) {
    // Discover available disks
    discover_disks();

    // Create main window using window server API
    g_win = ws_create_window("Disk Utility", WIN_W, WIN_H,
                              disk_util_on_paint,
                              disk_util_on_input,
                              disk_util_on_mouse);
    if (!g_win) {
        s_printf("[DiskUtility] Failed to create window!\n");
        return;
    }

    g_win->min_w = 420;

    // Wire up scroll callback
    g_win->scroll_callback = (void*)disk_util_on_scroll;

    // Register in dock
    dock_register("Disk Utility", 6, g_win);

    s_printf("[DiskUtility] Ready. Found ");
    char num[8];
    int_to_str(g_state.disk_count, num);
    s_printf(num);
    s_printf(" disk(s)\n");
}
