// usr/lib/proc.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

int proc_get_pid() { return 1; }
void proc_yield() { }

static cdl_symbol_t my_symbols[] = {
    { "get_pid", (void*)proc_get_pid },
    { "yield", (void*)proc_yield }
};

static cdl_exports_t my_exports = {
    .lib_name = "proc", .version = 1, .symbol_count = 2, .symbols = my_symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &my_exports;
}