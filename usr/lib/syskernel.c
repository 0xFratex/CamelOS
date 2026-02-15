// usr/lib/syskernel.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

int k_get_ticks() { return 0; }
void k_log(const char* msg) { if(sys) sys->print(msg); }

static cdl_symbol_t my_symbols[] = {
    { "get_ticks", (void*)k_get_ticks },
    { "log", (void*)k_log }
};

static cdl_exports_t my_exports = {
    .lib_name = "kernel", .version = 1, .symbol_count = 2, .symbols = my_symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &my_exports;
}