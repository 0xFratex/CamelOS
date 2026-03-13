// usr/apps/nettools_cdl.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

#define LOG_LINES 14
#define LOG_COLS 45
char log_buffer[LOG_LINES][LOG_COLS];
int log_head = 0;

void net_log(const char* msg) {
    if (log_head >= LOG_LINES) {
        for(int i=0; i<LOG_LINES-1; i++) {
            sys->memset(log_buffer[i], 0, LOG_COLS);
            sys->strcpy(log_buffer[i], log_buffer[i+1]);
        }
        sys->memset(log_buffer[LOG_LINES-1], 0, LOG_COLS);
        log_head = LOG_LINES - 1;
    }
    sys->memset(log_buffer[log_head], 0, LOG_COLS);
    int i = 0;
    while(msg[i] && i < LOG_COLS-1) {
        log_buffer[log_head][i] = msg[i];
        i++;
    }
    log_buffer[log_head][i] = 0;
    log_head++;
    sys->print("[NetTools] "); sys->print(msg); sys->print("\n");
}

void cmd_list_dev() {
    char ip[16], mac[20];
    if (sys->net_get_interface_info("eth0", ip, mac) == 0) {
        net_log("1. eth0 (RTL8139) - UP");
        char buf[64];
        sys->sprintf(buf, "   IP: %s", ip); net_log(buf);
        sys->sprintf(buf, "   MAC: %s", mac); net_log(buf);
    } else {
        net_log("No active interfaces found.");
    }
}

void on_paint(int x, int y, int w, int h) {
    sys->draw_rect(x, y, w, h, 0xFF1E1E1E);
    int start_y = y + 10;
    for(int i=0; i<LOG_LINES; i++) {
        if(log_buffer[i][0] != 0) {
            sys->draw_text(x + 10, start_y + (i * 14), log_buffer[i], 0xFF00FF00);
        }
    }
    sys->draw_rect(x, y + h - 24, w, 24, 0xFF303030);
    sys->draw_text(x + 10, y + h - 18, "Keys: [L] List [P] Ping [W] Wifi", 0xFFCCCCCC);
}

void on_input(int key) {
    if (key == 'l' || key == 'L') cmd_list_dev();
    if (key == 'w' || key == 'W') {
        net_log("[WIFI] Scanning...");
        // In a real implementation this would query the driver
        net_log("No wireless extensions.");
    }
    if (key == 'p' || key == 'P') {
        char buf[64];
        net_log("Pinging 8.8.8.8...");
        int res = sys->ping("8.8.8.8", buf, 64);
        if (res >= 0) {
            char msg[64]; sys->sprintf(msg, "Reply: %s", buf); net_log(msg);
        } else net_log("Request Timed Out.");
    }
}

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    for(int i=0; i<LOG_LINES; i++) sys->memset(log_buffer[i], 0, LOG_COLS);
    net_log("NetTools v1.2 Ready.");
    void* win = sys->create_window("Network Tools", 320, 240, on_paint, on_input, 0);
    cmd_list_dev();
    return 0;
}