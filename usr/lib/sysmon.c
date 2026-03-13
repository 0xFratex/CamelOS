// usr/lib/sysmon.c
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

// Moving average for smoother CPU visualization
#define HISTORY_SIZE 10
static int cpu_samples[HISTORY_SIZE];
static int sample_idx = 0;
static int initialized = 0;

// Pseudo-random generator for CPU simulation (since we don't have hardware counters exposed yet)
// In a real OS, this would read /proc/stat or kernel counters.
int calculate_cpu_load() {
    static unsigned int seed = 12345;
    seed = seed * 1103515245 + 12345;
    
    // Base load + random spike
    int base = 5; 
    int spike = (int)((seed / 65536) % 25);
    
    // Simulate load based on system ticks (activity)
    int tick_delta = sys->get_ticks() % 100;
    if (tick_delta < 20) spike += 30; // Busy period
    
    return base + spike;
}

int sysmon_get_cpu_usage() {
    if (!sys) return 0;
    
    if (!initialized) {
        for(int i=0; i<HISTORY_SIZE; i++) cpu_samples[i] = 5;
        initialized = 1;
    }

    // Add new sample
    cpu_samples[sample_idx] = calculate_cpu_load();
    sample_idx = (sample_idx + 1) % HISTORY_SIZE;

    // Calculate Average
    int sum = 0;
    for(int i=0; i<HISTORY_SIZE; i++) sum += cpu_samples[i];
    
    int avg = sum / HISTORY_SIZE;
    if (avg > 100) avg = 100;
    return avg;
}

void sysmon_get_ram_usage(uint32_t* used_mb, uint32_t* total_mb) {
    if (!sys) return;
    *used_mb = sys->mem_used() / 1024 / 1024;
    *total_mb = sys->mem_total() / 1024 / 1024;
    if (*total_mb == 0) *total_mb = 1;
}

static cdl_symbol_t my_symbols[] = {
    { "cpu", (void*)sysmon_get_cpu_usage },
    { "ram", (void*)sysmon_get_ram_usage }
};

static cdl_exports_t my_exports = {
    .lib_name = "SysMon", .version = 1, .symbol_count = 2, .symbols = my_symbols
};

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    return &my_exports;
}