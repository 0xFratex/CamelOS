// usr/lib/timer.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

// Configuration: Kernel typically runs Timer at 50Hz
#define TICKS_PER_SEC 50

// 1. Get raw system ticks
uint32_t timer_get_ticks() {
    if(sys && sys->get_ticks) return sys->get_ticks();
    return 0;
}

// 2. Get seconds since boot
int timer_get_seconds() {
    return timer_get_ticks() / TICKS_PER_SEC;
}

// 3. Blocking Sleep (in milliseconds)
// Relies on the kernel tick counter advancing via interrupts
void timer_sleep(int ms) {
    if (!sys || !sys->get_ticks) return;
    
    uint32_t start = sys->get_ticks();
    uint32_t ticks_to_wait = (ms * TICKS_PER_SEC) / 1000;
    
    // Ensure at least 1 tick wait for small non-zero values
    if(ticks_to_wait == 0 && ms > 0) ticks_to_wait = 1;

    uint32_t target = start + ticks_to_wait;

    // Simple polling loop. In a multitasking system, we would yield.
    while (sys->get_ticks() < target) {
        asm volatile("nop");
    }
}

// 4. Stopwatch Structure & Functions
typedef struct {
    uint32_t start_tick;
    int running;
} stopwatch_t;

void* timer_sw_create() {
    if(!sys) return 0;
    stopwatch_t* sw = (stopwatch_t*)sys->malloc(sizeof(stopwatch_t));
    if(sw) {
        sw->start_tick = sys->get_ticks();
        sw->running = 1;
    }
    return sw;
}

int timer_sw_elapsed_ms(void* handle) {
    if(!sys || !handle) return 0;
    stopwatch_t* sw = (stopwatch_t*)handle;
    uint32_t now = sys->get_ticks();
    uint32_t diff = now - sw->start_tick;
    
    // Convert ticks to ms
    return diff * (1000 / TICKS_PER_SEC);
}

void timer_sw_reset(void* handle) {
    if(!sys || !handle) return;
    stopwatch_t* sw = (stopwatch_t*)handle;
    sw->start_tick = sys->get_ticks();
}

static cdl_symbol_t my_symbols[] = {
    { "ticks", (void*)timer_get_ticks },
    { "seconds", (void*)timer_get_seconds },
    { "sleep", (void*)timer_sleep },
    { "sw_new", (void*)timer_sw_create },
    { "sw_ms", (void*)timer_sw_elapsed_ms },
    { "sw_rst", (void*)timer_sw_reset }
};

static cdl_exports_t my_exports = {
    .lib_name = "timer", 
    .version = 1, 
    .symbol_count = 6, 
    .symbols = my_symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &my_exports;
}