// usr/lib/math.c
// Simple math library for CDL testing - no string dependencies

#include "../../sys/cdl_defs.h"

// Store Kernel API internally
static kernel_api_t* k_api = 0;

// --- Actual Library Functions ---

int math_add(int a, int b) {
    return a + b;
}

int math_sub(int a, int b) {
    return a - b;
}

int math_mul(int a, int b) {
    return a * b;
}

int math_div(int a, int b) {
    if(b == 0) {
        return 0;
    }
    return a / b;
}

// Helper function that library users can call
int math_is_even(int num) {
    return (num % 2) == 0;
}

// --- Export Table ---
// We must use static structs to keep them in the binary data section
static cdl_symbol_t my_symbols[] = {
    { "add", (void*)math_add },
    { "sub", (void*)math_sub },
    { "mul", (void*)math_mul },
    { "div", (void*)math_div },
    { "is_even", (void*)math_is_even }
};

static cdl_exports_t my_exports = {
    .lib_name = "CamelMath",
    .version = 1,
    .symbol_count = 5,
    .symbols = my_symbols
};

// --- Entry Point ---
// This is the first function in our flat binary
cdl_exports_t* cdl_main(kernel_api_t* api) {
    k_api = api; // Save kernel pointers for later use
    
    if(k_api && k_api->print) {
        k_api->print("Math Library Initialized!\n");
    }
    
    return &my_exports;
}
