// hal/cpu/cpu_info.c - Centralized CPU detection module
// Uses CPUID instruction to detect CPU vendor, brand, features, and topology.
// PIC-compliant: saves/restores %ebx around CPUID for 32-bit position-independent code.
// Compiles with -m32 -mno-sse -mno-mmx (no SSE/MMX in detection code itself).

#include "cpu_info.h"
#include "../../include/string.h"

// --- Internal state ---
static cpu_info_t g_cpu_info;
static int g_cpu_info_detected = 0;

// --- PIC-safe CPUID wrapper ---
// In 32-bit PIC code, %ebx is used by the GOT pointer.
// We must save/restore it around the CPUID instruction to avoid corruption.
static void cpuid_query(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile(
        "pushl %%ebx\n\t"        // Save PIC register
        "cpuid\n\t"
        "movl %%ebx, %1\n\t"     // Store ebx result before restoring
        "popl %%ebx\n\t"         // Restore PIC register
        : "=a"(*eax), "=r"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
    );
}

// CPUID with sub-leaf (ecx input), also PIC-safe
static void cpuid_query_subleaf(uint32_t leaf, uint32_t subleaf,
                                 uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "cpuid\n\t"
        "movl %%ebx, %1\n\t"
        "popl %%ebx\n\t"
        : "=a"(*eax), "=r"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

// --- Helper: trim leading spaces from a string in-place ---
static void trim_leading_spaces(char *s) {
    char *start = s;
    while (*start == ' ') start++;
    if (start != s) {
        // memmove to handle overlap
        char *d = s;
        while (*start) *d++ = *start++;
        *d = '\0';
    }
}

// --- Detection ---
void cpu_info_detect(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_leaf, max_ext_leaf;

    // Zero out the structure
    memset(&g_cpu_info, 0, sizeof(g_cpu_info));

    // --- Leaf 0: Vendor string and max basic leaf ---
    cpuid_query(0, &max_leaf, &ebx, &ecx, &edx);
    // Vendor string is in EBX:EDX:ECX (12 bytes)
    memcpy(g_cpu_info.vendor, &ebx, 4);
    memcpy(g_cpu_info.vendor + 4, &edx, 4);
    memcpy(g_cpu_info.vendor + 8, &ecx, 4);
    g_cpu_info.vendor[12] = '\0';

    // --- Leaf 1: Family, model, stepping, and features ---
    if (max_leaf >= 1) {
        uint32_t sig_eax, sig_ebx, sig_ecx, sig_edx;
        cpuid_query(1, &sig_eax, &sig_ebx, &sig_ecx, &sig_edx);

        // Extract signature fields
        g_cpu_info.stepping = sig_eax & 0x0F;
        g_cpu_info.model    = (sig_eax >> 4) & 0x0F;
        g_cpu_info.family   = (sig_eax >> 8) & 0x0F;

        // Extended model and family (for families >= 0xF)
        int ext_model  = (sig_eax >> 16) & 0x0F;
        int ext_family = (sig_eax >> 20) & 0xFF;

        if (g_cpu_info.family == 0x0F) {
            g_cpu_info.family += ext_family;
            g_cpu_info.model += (ext_model << 4);
        }

        // --- Feature flags from EDX ---
        if (sig_edx & (1 << 0))  g_cpu_info.features |= CPU_FEATURE_FPU;
        if (sig_edx & (1 << 23)) g_cpu_info.features |= CPU_FEATURE_MMX;
        if (sig_edx & (1 << 25)) g_cpu_info.features |= CPU_FEATURE_SSE;
        if (sig_edx & (1 << 26)) g_cpu_info.features |= CPU_FEATURE_SSE2;
        if (sig_edx & (1 << 24)) g_cpu_info.features |= CPU_FEATURE_FXSR;
        if (sig_edx & (1 << 9))  g_cpu_info.features |= CPU_FEATURE_APIC;
        if (sig_edx & (1 << 4))  g_cpu_info.features |= CPU_FEATURE_TSC;

        // --- Feature flags from ECX ---
        if (sig_ecx & (1 << 0))  g_cpu_info.features |= CPU_FEATURE_SSE3;
        if (sig_ecx & (1 << 9))  g_cpu_info.features |= CPU_FEATURE_SSSE3;
        if (sig_ecx & (1 << 19)) g_cpu_info.features |= CPU_FEATURE_SSE41;
        if (sig_ecx & (1 << 20)) g_cpu_info.features |= CPU_FEATURE_SSE42;
        if (sig_ecx & (1 << 28)) g_cpu_info.features |= CPU_FEATURE_AVX;

        // --- Core count ---
        // Try Intel leaf 0x0B (Extended Topology) first
        // Then fall back to leaf 0x04 (Deterministic Cache Parameters)
        // Then AMD leaf 0x80000008
        // Default to 1 core

        g_cpu_info.num_cores = 1;

        if (max_leaf >= 0x0B) {
            // Intel Extended Topology Enum
            uint32_t teax, tebx, tecx, tedx;
            cpuid_query_subleaf(0x0B, 0, &teax, &tebx, &tecx, &tedx);
            // EBX[15:0] = number of logical processors at this level
            // ECX[15:8] = level type (1 = SMT, 2 = Core)
            if (tebx & 0xFFFF) {
                int level_type = (tecx >> 8) & 0xFF;
                if (level_type == 2) {
                    // Level 0 already gave us core level
                    g_cpu_info.num_cores = tebx & 0xFFFF;
                } else {
                    // Try level 1 for core count
                    cpuid_query_subleaf(0x0B, 1, &teax, &tebx, &tecx, &tedx);
                    level_type = (tecx >> 8) & 0xFF;
                    if (level_type == 2) {
                        g_cpu_info.num_cores = tebx & 0xFFFF;
                    }
                }
            }
        }

        if (g_cpu_info.num_cores <= 1 && max_leaf >= 4) {
            // Intel Deterministic Cache Parameters
            uint32_t ceax, cebx, cecx, cedx;
            cpuid_query_subleaf(4, 0, &ceax, &cebx, &cecx, &cedx);
            // EAX[31:26] + 1 = number of cores per package
            g_cpu_info.num_cores = ((ceax >> 26) & 0x3F) + 1;
        }
    }

    // --- Extended leaves: Brand string ---
    cpuid_query(0x80000000, &max_ext_leaf, &ebx, &ecx, &edx);

    if (max_ext_leaf >= 0x80000004) {
        // Brand string from leaves 0x80000002, 0x80000003, 0x80000004
        // Each leaf returns 16 bytes in EAX:EBX:ECX:EDX = 48 bytes total
        uint32_t *p = (uint32_t *)g_cpu_info.brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_query(leaf, &eax, &ebx, &ecx, &edx);
            *p++ = eax;
            *p++ = ebx;
            *p++ = ecx;
            *p++ = edx;
        }
        g_cpu_info.brand[48] = '\0';
        trim_leading_spaces(g_cpu_info.brand);
    } else {
        strcpy(g_cpu_info.brand, "Unknown CPU");
    }

    // --- AMD core count from extended leaf 0x80000008 ---
    if (max_ext_leaf >= 0x80000008) {
        // On AMD, ECX[7:0] + 1 = number of cores
        // Only use if we didn't get a good count from basic leaves
        if (g_cpu_info.num_cores <= 1) {
            cpuid_query(0x80000008, &eax, &ebx, &ecx, &edx);
            int amd_cores = (ecx & 0xFF) + 1;
            if (amd_cores > 1) {
                g_cpu_info.num_cores = amd_cores;
            }
        }
    }

    // --- Architecture ---
    // Check for long mode (64-bit) support via extended feature leaf
    if (max_ext_leaf >= 0x80000001) {
        cpuid_query(0x80000001, &eax, &ebx, &ecx, &edx);
        if (edx & (1 << 29)) {
            // Long mode supported
            g_cpu_info.architecture = 64;
        } else {
            g_cpu_info.architecture = 32;
        }
    } else {
        // Default: 32-bit (CamelOS is 32-bit)
        g_cpu_info.architecture = 32;
    }

    g_cpu_info_detected = 1;
}

// --- Accessors ---

cpu_info_t* cpu_info_get(void) {
    if (!g_cpu_info_detected) {
        cpu_info_detect();
    }
    return &g_cpu_info;
}

const char* cpu_info_arch_name(void) {
    if (!g_cpu_info_detected) cpu_info_detect();

    if (g_cpu_info.architecture == 64) {
        return "x86_64";
    }
    return "x86";
}

const char* cpu_info_vendor_name(void) {
    if (!g_cpu_info_detected) cpu_info_detect();

    if (strcmp(g_cpu_info.vendor, "GenuineIntel") == 0)  return "Intel";
    if (strcmp(g_cpu_info.vendor, "AuthenticAMD") == 0)   return "AMD";
    if (strcmp(g_cpu_info.vendor, "CentaurHauls") == 0)   return "VIA";
    if (strcmp(g_cpu_info.vendor, "CyrixInstead") == 0)   return "Cyrix";
    if (strcmp(g_cpu_info.vendor, "TransmetaCPU") == 0)   return "Transmeta";
    if (strcmp(g_cpu_info.vendor, "GenuineTMx86") == 0)   return "Transmeta";
    if (strcmp(g_cpu_info.vendor, "Geode by NSC") == 0)   return "National Semiconductor";
    if (strcmp(g_cpu_info.vendor, "VIA VIA VIA ") == 0)   return "VIA";
    return "Unknown";
}

int cpu_info_has_feature(uint32_t feature) {
    if (!g_cpu_info_detected) cpu_info_detect();
    return (g_cpu_info.features & feature) != 0;
}
