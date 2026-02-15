#include "../../sys/cdl_defs.h"
#include "../lib/camel_framework.h"

static kernel_api_t* sys = 0;

// Modern Dark Theme
#define C_BG      0xFF1E1E1E
#define C_SIDEBAR 0xFF252526
#define C_ACCENT  0xFF007ACC
#define C_TEXT    0xFFCCCCCC
#define C_GRID    0xFF333333

#define HIST_LEN 60
int cpu_hist[HIST_LEN];
int ram_hist[HIST_LEN];
int head = 0;
int mode = 0; // 0=CPU, 1=RAM

void on_paint(int x, int y, int w, int h) {
    // Data Update
    cpu_hist[head] = (sys->get_ticks() % 60) + 10;
    uint32_t used = sys->mem_used() / 1024 / 1024;
    uint32_t total = sys->mem_total() / 1024 / 1024;
    if(total == 0) total = 1;
    ram_hist[head] = (used * 100) / total;
    head = (head + 1) % HIST_LEN;

    // Sidebar
    int sb_w = 80;
    sys->draw_rect(x, y, sb_w, h, C_SIDEBAR);

    // Sidebar Tabs
    sys->draw_rect(x, y+20, sb_w, 30, (mode==0)?C_ACCENT:C_SIDEBAR);
    sys->draw_text(x+25, y+30, "CPU", 0xFFFFFFFF);

    sys->draw_rect(x, y+55, sb_w, 30, (mode==1)?C_ACCENT:C_SIDEBAR);
    sys->draw_text(x+25, y+65, "RAM", 0xFFFFFFFF);

    // Main Area
    int mx = x + sb_w;
    int mw = w - sb_w;
    sys->draw_rect(mx, y, mw, h, C_BG);

    // Grid
    sys->draw_rect(mx+10, y+10, mw-20, h-20, 0xFF000000); // Black Graph BG
    sys->draw_rect(mx+10, y+ (h/2), mw-20, 1, C_GRID);

    // Plot
    int* data = (mode == 0) ? cpu_hist : ram_hist;
    int bar_w = (mw-20) / HIST_LEN;
    if (bar_w < 1) bar_w = 1;

    for(int i=0; i<HIST_LEN; i++) {
        int idx = (head + i) % HIST_LEN;
        int val = data[idx];
        if(val > 100) val = 100;

        int bar_h = (val * (h-22)) / 100;
        int bx = mx + 10 + (i * bar_w);
        int by = y + h - 10 - bar_h;

        uint32_t col = (mode==0) ? 0xFF4CAF50 : 0xFF2196F3;
        sys->draw_rect(bx, by, bar_w, bar_h, col);
    }

    // Values
    char buf[32];
    sys->itoa((mode==0)?data[(head-1+HIST_LEN)%HIST_LEN]:used, buf);
    sys->draw_text(mx+20, y+20, (mode==0)?"CPU %":"MB Used", C_TEXT);
    sys->draw_text(mx+80, y+20, buf, 0xFFFFFFFF);
}

void on_mouse(int x, int y, int btn) {
    if (x < 80) {
        if (y > 20 && y < 50) mode = 0;
        if (y > 55 && y < 85) mode = 1;
    }
}

static cdl_exports_t exports = { .lib_name = "Waterhole", .version = 4 };
cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    for(int i=0;i<HIST_LEN;i++) { cpu_hist[i]=0; ram_hist[i]=0; }
    void* win = sys->create_window("Activity Monitor", 400, 250, on_paint, 0, on_mouse);
    return &exports;
}