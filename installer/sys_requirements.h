// installer/sys_requirements.h - System Requirements Check Header
#ifndef SYS_REQUIREMENTS_H
#define SYS_REQUIREMENTS_H

#include "../include/types.h"

// Requirement status
typedef enum {
    REQ_STATUS_UNKNOWN,
    REQ_STATUS_CHECKING,
    REQ_STATUS_PASS,
    REQ_STATUS_WARNING,
    REQ_STATUS_FAIL
} RequirementStatus;

// Individual requirement
typedef struct {
    const char* name;
    const char* description;
    uint32_t minimum;
    uint32_t recommended;
    uint32_t detected;
    RequirementStatus status;
    const char* status_text;
} SystemRequirement;

// Requirements check result
typedef struct {
    // CPU requirements
    int cpu_supported;
    int cpu_bits;           // 32 or 64
    char cpu_vendor[13];
    char cpu_model[49];
    int cpu_speed_mhz;
    
    // Memory requirements
    uint32_t ram_total_kb;
    uint32_t ram_available_kb;
    
    // Storage requirements
    uint32_t storage_total_sectors;
    uint32_t storage_total_mb;
    int storage_device_count;
    
    // Display requirements
    int vga_supported;
    int display_width;
    int display_height;
    
    // Input devices
    int keyboard_present;
    int mouse_present;
    
    // Network (optional)
    int network_supported;
    char network_mac[18];
    
    // Overall result
    int can_install;
    int warnings_count;
    int errors_count;
    
    // Detailed requirements array
    SystemRequirement requirements[8];
    int requirement_count;
} RequirementsCheck;

// Public API
void sys_requirements_init(void);
void sys_requirements_check(void);
RequirementsCheck* sys_requirements_get(void);

// Individual checks
int sys_check_cpu(void);
int sys_check_memory(void);
int sys_check_storage(void);
int sys_check_display(void);
int sys_check_input(void);

// Rendering
void sys_requirements_render_summary(int x, int y);
void sys_requirements_render_details(int x, int y);

// Helpers
const char* sys_requirements_status_icon(RequirementStatus status);
uint32_t sys_requirements_status_color(RequirementStatus status);

#endif // SYS_REQUIREMENTS_H
