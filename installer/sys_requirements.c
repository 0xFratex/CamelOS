// installer/sys_requirements.c - System Requirements Check Implementation

#include "sys_requirements.h"
#include "../hal/drivers/ata.h"
#include "../hal/drivers/vga.h"
#include "../hal/drivers/keyboard.h"
#include "../hal/drivers/mouse.h"
#include "../core/string.h"
#include "../core/memory.h"
#include "../hal/video/gfx_hal.h"
#include "../common/ports.h"

// Global check result
static RequirementsCheck g_req;

// Minimum requirements
#define MIN_RAM_MB          64
#define REC_RAM_MB          256
#define MIN_STORAGE_MB      128
#define REC_STORAGE_MB      1024
#define MIN_DISPLAY_W       640
#define MIN_DISPLAY_H       480

// --- Initialization ---

void sys_requirements_init(void) {
    memset(&g_req, 0, sizeof(g_req));
    g_req.requirement_count = 0;
}

// Add a requirement to the list
static void add_requirement(const char* name, const char* desc, 
                           uint32_t min, uint32_t rec, uint32_t detected,
                           RequirementStatus status, const char* status_text) {
    if (g_req.requirement_count >= 8) return;
    
    SystemRequirement* req = &g_req.requirements[g_req.requirement_count++];
    req->name = name;
    req->description = desc;
    req->minimum = min;
    req->recommended = rec;
    req->detected = detected;
    req->status = status;
    req->status_text = status_text;
}

// --- CPU Check ---

int sys_check_cpu(void) {
    // Check CPU using CPUID instruction
    uint32_t eax, ebx, ecx, edx;
    
    // Get vendor string
    char vendor[13] = {0};
    asm volatile("cpuid" : "=b"(ebx), "=d"(edx), "=c"(ecx) : "a"(0));
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = 0;
    
    strcpy(g_req.cpu_vendor, vendor);
    
    // Check for supported CPU
    if (strcmp(vendor, "GenuineIntel") == 0 || 
        strcmp(vendor, "AuthenticAMD") == 0 ||
        strcmp(vendor, "CentaurHauls") == 0) {
        g_req.cpu_supported = 1;
    } else {
        g_req.cpu_supported = 0;
    }
    
    // Get CPU model name (if supported)
    asm volatile("cpuid" : "=a"(eax) : "a"(0x80000000));
    if (eax >= 0x80000004) {
        char model[49] = {0};
        for (int i = 0; i < 3; i++) {
            asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) 
                        : "a"(0x80000002 + i));
            memcpy(model + i * 16, &eax, 4);
            memcpy(model + i * 16 + 4, &ebx, 4);
            memcpy(model + i * 16 + 8, &ecx, 4);
            memcpy(model + i * 16 + 12, &edx, 4);
        }
        // Trim leading spaces
        char* model_start = model;
        while (*model_start == ' ') model_start++;
        strcpy(g_req.cpu_model, model_start);
    } else {
        strcpy(g_req.cpu_model, "Unknown CPU");
    }
    
    // Assume 32-bit for now (Camel OS is 32-bit)
    g_req.cpu_bits = 32;
    
    // Add requirement
    RequirementStatus status = g_req.cpu_supported ? REQ_STATUS_PASS : REQ_STATUS_FAIL;
    add_requirement("CPU", g_req.cpu_model, 1, 1, 1, status, 
                   g_req.cpu_supported ? "Supported" : "Not supported");
    
    return g_req.cpu_supported;
}

// --- Memory Check ---

int sys_check_memory(void) {
    // Get memory info from kernel
    extern uint32_t total_memory_kb;
    g_req.ram_total_kb = total_memory_kb;
    g_req.ram_available_kb = total_memory_kb;  // Simplified
    
    uint32_t ram_mb = g_req.ram_total_kb / 1024;
    
    RequirementStatus status;
    const char* status_text;
    
    if (ram_mb >= REC_RAM_MB) {
        status = REQ_STATUS_PASS;
        status_text = "Excellent";
    } else if (ram_mb >= MIN_RAM_MB) {
        status = REQ_STATUS_WARNING;
        status_text = "Minimum met";
        g_req.warnings_count++;
    } else {
        status = REQ_STATUS_FAIL;
        status_text = "Insufficient";
        g_req.errors_count++;
    }
    
    add_requirement("Memory", "System RAM", MIN_RAM_MB, REC_RAM_MB, ram_mb, status, status_text);
    
    return status != REQ_STATUS_FAIL;
}

// --- Storage Check ---

int sys_check_storage(void) {
    g_req.storage_device_count = 0;
    g_req.storage_total_sectors = 0;
    
    // Check IDE devices
    for (int i = 0; i < 2; i++) {
        if (ide_devices[i].present) {
            g_req.storage_device_count++;
            g_req.storage_total_sectors += ide_devices[i].sectors;
        }
    }
    
    g_req.storage_total_mb = g_req.storage_total_sectors / 2048;
    
    RequirementStatus status;
    const char* status_text;
    
    if (g_req.storage_total_mb >= REC_STORAGE_MB) {
        status = REQ_STATUS_PASS;
        status_text = "Excellent";
    } else if (g_req.storage_total_mb >= MIN_STORAGE_MB) {
        status = REQ_STATUS_WARNING;
        status_text = "Minimum met";
        g_req.warnings_count++;
    } else {
        status = REQ_STATUS_FAIL;
        status_text = "Insufficient";
        g_req.errors_count++;
    }
    
    add_requirement("Storage", "Disk Space", MIN_STORAGE_MB, REC_STORAGE_MB, 
                   g_req.storage_total_mb, status, status_text);
    
    return status != REQ_STATUS_FAIL;
}

// --- Display Check ---

int sys_check_display(void) {
    // Check VGA support
    g_req.vga_supported = 1;  // Assume VGA is supported
    
    // Get display resolution
    g_req.display_width = screen_w ? screen_w : 1024;
    g_req.display_height = screen_h ? screen_h : 768;
    
    RequirementStatus status;
    const char* status_text;
    
    if (g_req.display_width >= 1024 && g_req.display_height >= 768) {
        status = REQ_STATUS_PASS;
        status_text = "Excellent";
    } else if (g_req.display_width >= MIN_DISPLAY_W && g_req.display_height >= MIN_DISPLAY_H) {
        status = REQ_STATUS_WARNING;
        status_text = "Minimum met";
        g_req.warnings_count++;
    } else {
        status = REQ_STATUS_FAIL;
        status_text = "Resolution too low";
        g_req.errors_count++;
    }
    
    add_requirement("Display", "Screen Resolution", MIN_DISPLAY_W, 1024, 
                   g_req.display_width, status, status_text);
    
    return status != REQ_STATUS_FAIL;
}

// --- Input Check ---

int sys_check_input(void) {
    // Check keyboard
    g_req.keyboard_present = 1;  // Assume keyboard present
    
    // Check mouse
    g_req.mouse_present = 1;  // Assume mouse present
    
    RequirementStatus status = REQ_STATUS_PASS;
    const char* status_text = "OK";
    
    if (!g_req.keyboard_present) {
        status = REQ_STATUS_FAIL;
        status_text = "Keyboard required";
        g_req.errors_count++;
    } else if (!g_req.mouse_present) {
        status = REQ_STATUS_WARNING;
        status_text = "No mouse detected";
        g_req.warnings_count++;
    }
    
    add_requirement("Input", "Keyboard & Mouse", 1, 2, 
                   g_req.keyboard_present + g_req.mouse_present, status, status_text);
    
    return g_req.keyboard_present;
}

// --- Full Check ---

void sys_requirements_check(void) {
    sys_requirements_init();
    
    g_req.warnings_count = 0;
    g_req.errors_count = 0;
    
    // Run all checks
    int cpu_ok = sys_check_cpu();
    int ram_ok = sys_check_memory();
    int storage_ok = sys_check_storage();
    int display_ok = sys_check_display();
    int input_ok = sys_check_input();
    
    // Overall result
    g_req.can_install = cpu_ok && ram_ok && storage_ok && display_ok && input_ok;
}

RequirementsCheck* sys_requirements_get(void) {
    return &g_req;
}

// --- Rendering Helpers ---

const char* sys_requirements_status_icon(RequirementStatus status) {
    switch (status) {
        case REQ_STATUS_PASS:    return "✓";
        case REQ_STATUS_WARNING: return "!";
        case REQ_STATUS_FAIL:    return "✗";
        default:                 return "?";
    }
}

uint32_t sys_requirements_status_color(RequirementStatus status) {
    switch (status) {
        case REQ_STATUS_PASS:    return 0xFF34C759;  // Green
        case REQ_STATUS_WARNING: return 0xFFFF9500;  // Orange
        case REQ_STATUS_FAIL:    return 0xFFFF3B30;  // Red
        default:                 return 0xFF8E8E93;  // Grey
    }
}

// --- Rendering ---

void sys_requirements_render_summary(int x, int y) {
    // Background
    gfx_fill_rounded_rect(x, y, 400, 200, 0xFFF2F2F7, 12);
    gfx_draw_rect(x, y, 400, 200, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "System Requirements", 0xFF1C1C1E);
    
    // Overall status
    const char* overall_text = g_req.can_install ? "Ready to Install" : "Cannot Install";
    uint32_t overall_color = g_req.can_install ? 0xFF34C759 : 0xFFFF3B30;
    
    gfx_fill_rounded_rect(x + 240, y + 8, 140, 28, overall_color, 6);
    gfx_draw_string(x + 250, y + 14, overall_text, 0xFFFFFFFF);
    
    // Requirements list
    int req_y = y + 48;
    for (int i = 0; i < g_req.requirement_count; i++) {
        SystemRequirement* req = &g_req.requirements[i];
        
        // Status icon
        gfx_draw_string(x + 16, req_y, sys_requirements_status_icon(req->status), 
                       sys_requirements_status_color(req->status));
        
        // Name
        gfx_draw_string(x + 36, req_y, req->name, 0xFF1C1C1E);
        
        // Detected value
        char val_str[32];
        if (req->detected > 0) {
            int_to_str(req->detected, val_str);
            strcat(val_str, " MB");
        } else {
            strcpy(val_str, req->status_text);
        }
        
        int val_x = x + 300 - strlen(val_str) * 6;
        gfx_draw_string(val_x, req_y, val_str, sys_requirements_status_color(req->status));
        
        req_y += 20;
    }
    
    // Summary
    if (g_req.warnings_count > 0 || g_req.errors_count > 0) {
        char summary[64];
        strcpy(summary, "");
        
        if (g_req.errors_count > 0) {
            char err[16];
            int_to_str(g_req.errors_count, err);
            strcat(summary, err);
            strcat(summary, " error");
            if (g_req.errors_count > 1) strcat(summary, "s");
        }
        
        if (g_req.warnings_count > 0) {
            if (g_req.errors_count > 0) strcat(summary, ", ");
            char warn[16];
            int_to_str(g_req.warnings_count, warn);
            strcat(summary, warn);
            strcat(summary, " warning");
            if (g_req.warnings_count > 1) strcat(summary, "s");
        }
        
        gfx_draw_string(x + 16, y + 180, summary, 0xFF8E8E93);
    }
}

void sys_requirements_render_details(int x, int y) {
    // Full-screen detailed view
    gfx_fill_rounded_rect(x, y, 600, 400, 0xFFFFFFFF, 12);
    gfx_draw_rect(x, y, 600, 400, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string_scaled(x + 16, y + 16, "System Information", 0xFF1C1C1E, 2);
    
    // CPU info
    int section_y = y + 60;
    gfx_draw_string(x + 20, section_y, "Processor", 0xFF8E8E93);
    gfx_draw_string(x + 20, section_y + 20, g_req.cpu_model, 0xFF1C1C1E);
    gfx_draw_string(x + 20, section_y + 40, "Vendor:", 0xFF8E8E93);
    gfx_draw_string(x + 90, section_y + 40, g_req.cpu_vendor, 0xFF1C1C1E);
    
    // Memory info
    section_y += 80;
    gfx_draw_string(x + 20, section_y, "Memory", 0xFF8E8E93);
    char ram_str[32];
    int_to_str(g_req.ram_total_kb / 1024, ram_str);
    strcat(ram_str, " MB RAM");
    gfx_draw_string(x + 20, section_y + 20, ram_str, 0xFF1C1C1E);
    
    // Storage info
    section_y += 60;
    gfx_draw_string(x + 20, section_y, "Storage", 0xFF8E8E93);
    char storage_str[64];
    int_to_str(g_req.storage_device_count, storage_str);
    strcat(storage_str, " drive");
    if (g_req.storage_device_count != 1) strcat(storage_str, "s");
    strcat(storage_str, ", ");
    char size_str[32];
    int_to_str(g_req.storage_total_mb, size_str);
    strcat(storage_str, size_str);
    strcat(storage_str, " MB total");
    gfx_draw_string(x + 20, section_y + 20, storage_str, 0xFF1C1C1E);
    
    // Display info
    section_y += 60;
    gfx_draw_string(x + 20, section_y, "Display", 0xFF8E8E93);
    char display_str[32];
    int_to_str(g_req.display_width, display_str);
    strcat(display_str, " x ");
    char h_str[16];
    int_to_str(g_req.display_height, h_str);
    strcat(display_str, h_str);
    gfx_draw_string(x + 20, section_y + 20, display_str, 0xFF1C1C1E);
    
    // Input devices
    section_y += 60;
    gfx_draw_string(x + 20, section_y, "Input Devices", 0xFF8E8E93);
    char input_str[64] = "";
    if (g_req.keyboard_present) strcat(input_str, "Keyboard");
    if (g_req.mouse_present) {
        if (g_req.keyboard_present) strcat(input_str, ", ");
        strcat(input_str, "Mouse");
    }
    gfx_draw_string(x + 20, section_y + 20, input_str, 0xFF1C1C1E);
    
    // Compatibility status
    section_y += 80;
    uint32_t status_bg = g_req.can_install ? 0xFFE8F5E9 : 0xFFFFEBEE;
    uint32_t status_color = g_req.can_install ? 0xFF34C759 : 0xFFFF3B30;
    
    gfx_fill_rounded_rect(x + 20, section_y, 560, 50, status_bg, 8);
    
    const char* status_text = g_req.can_install ? 
        "✓ Your system is compatible with Camel OS" :
        "✗ Your system does not meet minimum requirements";
    gfx_draw_string(x + 40, section_y + 18, status_text, status_color);
}
