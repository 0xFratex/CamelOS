#include "../apps/mujs.h"
#include "../../sys/cdl_defs.h"

kernel_api_t* sys = 0;

static js_State *g_J = 0;

// These are the functions the browser will call
int jscore_init(void) {
    g_J = js_newstate(NULL, NULL, 0);
    if (!g_J) return -1;
    // Register any global functions your scripts need (console.log, etc.)
    return 0;
}

const char* jscore_eval(const char *source) {
    if (!g_J) return "Error: JS not initialised";
    if (js_dostring(g_J, source) != 0) {
        const char *err = js_trystring(g_J, -1, "unknown error");
        js_pop(g_J, 1);
        return err;
    }
    return "ok";
}

void jscore_cleanup(void) {
    if (g_J) {
        js_freestate(g_J);
        g_J = 0;
    }
}

// CDL export table
static cdl_symbol_t symbols[] = {
    { "jscore_init",    jscore_init },
    { "jscore_eval",    jscore_eval },
    { "jscore_cleanup", jscore_cleanup }
};

static cdl_exports_t exports = {
    .lib_name = "jscore",
    .version  = 100,
    .symbol_count = 3,
    .symbols = symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &exports;
}