// hal/cpu/cpu_info.h - Centralized CPU detection module
// Provides CPUID-based CPU info: vendor, brand, features, architecture
// This module is the single source of truth for CPU detection in CamelOS.
#ifndef CPU_INFO_H
#define CPU_INFO_H

#include "../../include/types.h"

// --- CPU Feature Flags (bitfield in cpu_info_t.features) ---
#define CPU_FEATURE_FPU    0x00000001
#define CPU_FEATURE_MMX    0x00000002
#define CPU_FEATURE_SSE    0x00000004
#define CPU_FEATURE_SSE2   0x00000008
#define CPU_FEATURE_SSE3   0x00000010
#define CPU_FEATURE_SSSE3  0x00000020
#define CPU_FEATURE_SSE41  0x00000040
#define CPU_FEATURE_SSE42  0x00000080
#define CPU_FEATURE_AVX    0x00000100
#define CPU_FEATURE_FXSR   0x00000200
#define CPU_FEATURE_APIC   0x00000400
#define CPU_FEATURE_TSC    0x00000800

// --- CPU Info Structure ---
typedef struct {
    char vendor[13];        // e.g., "GenuineIntel", "AuthenticAMD"
    char brand[49];         // Full brand string from CPUID leaves 0x80000002-0x80000004
    int architecture;       // 32 or 64
    int family;             // CPU family
    int model;              // CPU model
    int stepping;           // CPU stepping
    uint32_t features;      // Bitfield of CPU features
    int num_cores;          // Number of cores (if detectable)
} cpu_info_t;

// --- Public API ---

// Run CPUID detection and populate internal cpu_info_t
void cpu_info_detect(void);

// Get cached CPU info (call cpu_info_detect() first)
cpu_info_t* cpu_info_get(void);

// Return architecture name string: "x86", "x86_64", etc.
const char* cpu_info_arch_name(void);

// Return human-readable vendor name: "Intel", "AMD", "VIA", "Unknown"
const char* cpu_info_vendor_name(void);

// Check if a specific feature flag is supported
int cpu_info_has_feature(uint32_t feature);

#endif
