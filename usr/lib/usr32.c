// usr/lib/usr32.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

void usr32_msgbox(const char* title, const char* msg) {
    if(sys) {
        sys->print("[USR32] MsgBox: "); sys->print(msg); sys->print("\n");
    }
}

void* usr32_create_window(const char* title, int w, int h) {
    if(!sys) return 0;
    return sys->create_window(title, w, h, 0, 0, 0);
}

static cdl_symbol_t my_symbols[] = {
    { "msgbox", (void*)usr32_msgbox },
    { "create_window", (void*)usr32_create_window }
};

static cdl_exports_t my_exports = {
    .lib_name = "usr32", .version = 1, .symbol_count = 2, .symbols = my_symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &my_exports;
}