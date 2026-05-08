// usr/apps/about.c - CamelOS About App
// A simple system information window
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../../core/window_server.h"
#include "../../hal/drivers/serial.h"

#define ABOUT_WIN_W  400
#define ABOUT_WIN_H  340

// System info strings
static char sys_mem_str[64] = "";
static char sys_disk_str[64] = "";
static char hw_cpu_model[64] = "Unknown";
static char hw_cpu_arch[16] = "x86 (32-bit)";

static void about_read_system_info(void) {
    // Memory
    extern uint32_t k_get_free_mem();
    extern uint32_t k_get_total_mem();
    uint32_t total = k_get_total_mem() / (1024 * 1024);
    uint32_t free_mem = k_get_free_mem() / (1024 * 1024);
    sprintf(sys_mem_str, "%d MB (%d MB free)", total, free_mem);

    // Disk
    extern int sys_fs_stat(const char* path, int* total_blocks, int* free_blocks);
    int total_blocks = 0, free_blocks = 0;
    sys_fs_stat("/", &total_blocks, &free_blocks);
    int total_mb = (total_blocks * 4096) / (1024 * 1024);
    int free_mb = (free_blocks * 4096) / (1024 * 1024);
    sprintf(sys_disk_str, "%d MB (%d MB free)", total_mb, free_mb);

    // CPU
    char buf[256];
    int len = sys_fs_read("/proc/cpuinfo", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        char* model = strstr(buf, "model=");
        if (model) {
            model += 6;
            int i = 0;
            while (model[i] && model[i] != '\n' && i < 63) {
                hw_cpu_model[i] = model[i];
                i++;
            }
            hw_cpu_model[i] = 0;
        }
    }
}

static void about_on_paint(window_t* win, int x, int y, int w, int h) {
    (void)win;

    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);

    int cy = y + 20;

    // CamelOS Logo/Title
    gfx_draw_string_scaled(x + 30, cy, "Camel", 0xFF007AFF, 3);
    cy += 40;
    gfx_draw_string_scaled(x + 30, cy, "OS", 0xFF333333, 2);
    cy += 35;

    gfx_draw_string(x + 30, cy, "Version 3.0 (APFS+ Compatible)", 0xFF666666);
    cy += 25;

    gfx_draw_rect(x + 30, cy, w - 60, 1, 0xFFE0E0E0);
    cy += 15;

    gfx_draw_string(x + 30, cy, "A macOS-inspired operating system", 0xFF333333);
    cy += 20;
    gfx_draw_string(x + 30, cy, "with .app/.dmg compatibility.", 0xFF333333);
    cy += 30;

    // System Info
    gfx_draw_string(x + 30, cy, "System Information:", 0xFF007AFF);
    cy += 22;

    gfx_draw_string(x + 40, cy, "Memory:", 0xFF888888);
    gfx_draw_string(x + 120, cy, sys_mem_str, 0xFF333333);
    cy += 20;

    gfx_draw_string(x + 40, cy, "Disk:", 0xFF888888);
    gfx_draw_string(x + 120, cy, sys_disk_str, 0xFF333333);
    cy += 20;

    gfx_draw_string(x + 40, cy, "FS:", 0xFF888888);
    gfx_draw_string(x + 120, cy, "PFS32 v3.0", 0xFF333333);
    cy += 20;

    gfx_draw_string(x + 40, cy, "CPU:", 0xFF888888);
    gfx_draw_string(x + 120, cy, hw_cpu_model, 0xFF333333);
    cy += 20;

    gfx_draw_string(x + 40, cy, "Arch:", 0xFF888888);
    gfx_draw_string(x + 120, cy, hw_cpu_arch, 0xFF333333);
    cy += 30;

    gfx_draw_rect(x + 30, cy, w - 60, 1, 0xFFE0E0E0);
    cy += 10;
    gfx_draw_string(x + 30, cy, "Built with love by 0xFratex", 0xFF999999);
}

static void about_on_mouse(window_t* win, int mx, int my, int btn) {
    (void)win; (void)mx; (void)my; (void)btn;
}

static void about_on_input(window_t* win, int key) {
    (void)win; (void)key;
}

void init_about_app(void) {
    about_read_system_info();

    window_t* w = ws_create_window("About CamelOS", ABOUT_WIN_W, ABOUT_WIN_H,
                                    (void*)about_on_paint,
                                    (void*)about_on_input,
                                    (void*)about_on_mouse);
    if (w) {
        ws_center_window(w);
    }
}
