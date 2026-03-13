// installer/disk_health.c - Disk Health Monitoring Implementation
// Simulates SMART-like disk health monitoring

#include "disk_health.h"
#include "../hal/drivers/ata.h"
#include "../common/ports.h"
#include "../core/string.h"
#include "../hal/video/gfx_hal.h"

// Global health data for up to 4 drives
static DiskHealth g_disk_health[4];
static int g_health_initialized = 0;

// SMART attribute definitions
static const struct {
    uint16_t id;
    const char* name;
} smart_attr_defs[] = {
    {0x01, "Read Error Rate"},
    {0x03, "Spin-Up Time"},
    {0x04, "Start/Stop Count"},
    {0x05, "Reallocated Sectors"},
    {0x07, "Seek Error Rate"},
    {0x09, "Power-On Hours"},
    {0x0A, "Spin Retry Count"},
    {0x0B, "Recalibration Retries"},
    {0x0C, "Power Cycle Count"},
    {0xC2, "Temperature"},
    {0xC4, "Reallocation Count"},
    {0xC5, "Pending Sectors"},
    {0xC6, "Offline Uncorrectable"},
    {0xC7, "CRC Errors"},
    {0xBF, "G-Sense Error Rate"},
    {0x00, NULL}
};

// --- Initialization ---

void disk_health_init(void) {
    for (int i = 0; i < 4; i++) {
        memset(&g_disk_health[i], 0, sizeof(DiskHealth));
        g_disk_health[i].drive_index = i;
        g_disk_health[i].health_score = -1; // Unknown
    }
    g_health_initialized = 1;
}

// --- SMART Simulation ---

// Simulate reading SMART data from drive
// In a real OS, this would use ATA SMART commands
void disk_health_read_smart(int drive_index) {
    if (drive_index < 0 || drive_index >= 4) return;
    
    DiskHealth* dh = &g_disk_health[drive_index];
    
    if (!ide_devices[drive_index].present) {
        dh->health_score = -1;
        return;
    }
    
    // Copy basic info from IDE detection
    memcpy(dh->model, ide_devices[drive_index].model, 40);
    dh->model[40] = 0;
    dh->total_sectors = ide_devices[drive_index].sectors;
    
    // Simulate SMART attributes
    // In a real implementation, we would issue SMART READ DATA command
    // For now, we simulate based on drive characteristics
    
    dh->smart_supported = 1; // Assume SMART is supported
    dh->smart_enabled = 1;   // Assume SMART is enabled
    
    // Simulate some SMART attributes with reasonable values
    dh->attr_count = 0;
    
    // Power-On Hours (simulated based on some calculation)
    dh->power_on_hours = 1000 + (dh->total_sectors % 5000);
    dh->attributes[dh->attr_count].id = 0x09;
    dh->attributes[dh->attr_count].name = "Power-On Hours";
    dh->attributes[dh->attr_count].value = 100 - (dh->power_on_hours / 1000);
    dh->attributes[dh->attr_count].worst = dh->attributes[dh->attr_count].value;
    dh->attributes[dh->attr_count].threshold = 0;
    dh->attributes[dh->attr_count].status = 0;
    dh->attr_count++;
    
    // Spin-Up Count
    dh->spin_up_count = 500 + (drive_index * 100);
    dh->attributes[dh->attr_count].id = 0x04;
    dh->attributes[dh->attr_count].name = "Start/Stop Count";
    dh->attributes[dh->attr_count].value = 100 - (dh->spin_up_count / 100);
    dh->attributes[dh->attr_count].worst = dh->attributes[dh->attr_count].value;
    dh->attributes[dh->attr_count].threshold = 20;
    dh->attributes[dh->attr_count].status = 0;
    dh->attr_count++;
    
    // Reallocated Sectors (key health indicator)
    dh->reallocated_sectors = drive_index == 0 ? 0 : (drive_index * 2);
    dh->attributes[dh->attr_count].id = 0x05;
    dh->attributes[dh->attr_count].name = "Reallocated Sectors";
    dh->attributes[dh->attr_count].value = 100 - dh->reallocated_sectors;
    dh->attributes[dh->attr_count].worst = dh->attributes[dh->attr_count].value;
    dh->attributes[dh->attr_count].threshold = 5;
    dh->attributes[dh->attr_count].status = dh->reallocated_sectors > 5 ? 2 : 
                                            (dh->reallocated_sectors > 0 ? 1 : 0);
    dh->attr_count++;
    
    // Pending Sectors
    dh->pending_sectors = 0;
    dh->attributes[dh->attr_count].id = 0xC5;
    dh->attributes[dh->attr_count].name = "Pending Sectors";
    dh->attributes[dh->attr_count].value = 100;
    dh->attributes[dh->attr_count].worst = 100;
    dh->attributes[dh->attr_count].threshold = 0;
    dh->attributes[dh->attr_count].status = 0;
    dh->attr_count++;
    
    // CRC Errors
    dh->crc_errors = 0;
    dh->attributes[dh->attr_count].id = 0xC7;
    dh->attributes[dh->attr_count].name = "CRC Errors";
    dh->attributes[dh->attr_count].value = 100;
    dh->attributes[dh->attr_count].worst = 100;
    dh->attributes[dh->attr_count].threshold = 0;
    dh->attributes[dh->attr_count].status = 0;
    dh->attr_count++;
    
    // Temperature (simulated)
    dh->temperature = 35 + (drive_index * 3) + (dh->power_on_hours % 5);
    dh->attributes[dh->attr_count].id = 0xC2;
    dh->attributes[dh->attr_count].name = "Temperature";
    dh->attributes[dh->attr_count].value = dh->temperature < 50 ? 100 : 
                                          (dh->temperature < 60 ? 50 : 25);
    dh->attributes[dh->attr_count].worst = dh->attributes[dh->attr_count].value;
    dh->attributes[dh->attr_count].threshold = 0;
    dh->attributes[dh->attr_count].status = dh->temperature > 55 ? 1 : 0;
    dh->attr_count++;
    
    // Calculate overall health score
    int score = 100;
    for (int i = 0; i < dh->attr_count; i++) {
        if (dh->attributes[i].status == 2) {
            score -= 30;
        } else if (dh->attributes[i].status == 1) {
            score -= 10;
        }
    }
    
    // Factor in age
    if (dh->power_on_hours > 50000) {
        score -= 20;
    } else if (dh->power_on_hours > 20000) {
        score -= 10;
    }
    
    dh->health_score = score > 0 ? score : 0;
}

void disk_health_scan(int drive_index) {
    if (!g_health_initialized) {
        disk_health_init();
    }
    
    disk_health_read_smart(drive_index);
}

DiskHealth* disk_health_get(int drive_index) {
    if (drive_index < 0 || drive_index >= 4) {
        return NULL;
    }
    return &g_disk_health[drive_index];
}

int disk_health_get_score(int drive_index) {
    DiskHealth* dh = disk_health_get(drive_index);
    return dh ? dh->health_score : -1;
}

HealthStatus disk_health_get_status(int drive_index) {
    DiskHealth* dh = disk_health_get(drive_index);
    if (!dh || dh->health_score < 0) {
        return HEALTH_STATUS_UNKNOWN;
    }
    if (dh->health_score >= 80) {
        return HEALTH_STATUS_GOOD;
    }
    if (dh->health_score >= 50) {
        return HEALTH_STATUS_WARNING;
    }
    return HEALTH_STATUS_CRITICAL;
}

const char* disk_health_status_string(HealthStatus status) {
    switch (status) {
        case HEALTH_STATUS_GOOD:     return "Good";
        case HEALTH_STATUS_WARNING:  return "Warning";
        case HEALTH_STATUS_CRITICAL: return "Critical";
        default:                     return "Unknown";
    }
}

int disk_health_run_test(int drive_index, int test_type) {
    (void)test_type;
    
    // In a real implementation, this would start a SMART self-test
    // For now, just re-read the SMART data
    disk_health_read_smart(drive_index);
    return 1;
}

// --- Rendering ---

void disk_health_render_summary(int x, int y, int drive_index) {
    DiskHealth* dh = disk_health_get(drive_index);
    if (!dh) return;
    
    HealthStatus status = disk_health_get_status(drive_index);
    
    // Health indicator color
    uint32_t status_color;
    switch (status) {
        case HEALTH_STATUS_GOOD:     status_color = 0xFF34C759; break;  // Green
        case HEALTH_STATUS_WARNING:  status_color = 0xFFFF9500; break;  // Orange
        case HEALTH_STATUS_CRITICAL: status_color = 0xFFFF3B30; break;  // Red
        default:                     status_color = 0xFF8E8E93; break;  // Grey
    }
    
    // Health score circle
    int score = dh->health_score;
    int radius = 30;
    int cx = x + radius;
    int cy = y + radius;
    
    // Background circle
    gfx_fill_rounded_rect(cx - radius, cy - radius, radius * 2, radius * 2, 
                          0xFFE5E5EA, radius);
    
    // Score arc (simplified as filled portion)
    int fill_h = (score * radius * 2) / 100;
    gfx_fill_rounded_rect(cx - radius, cy + radius - fill_h, radius * 2, fill_h, 
                          status_color, radius);
    
    // Score text
    char score_str[8];
    int_to_str(score, score_str);
    gfx_draw_string(cx - strlen(score_str) * 4, cy - 8, score_str, 0xFF1C1C1E);
    
    // Status label
    gfx_draw_string(x + 70, y + 10, "Disk Health", 0xFF1C1C1E);
    gfx_draw_string(x + 70, y + 30, disk_health_status_string(status), status_color);
    
    // Temperature
    char temp_str[16];
    strcpy(temp_str, "Temp: ");
    int_to_str(dh->temperature, temp_str + 6);
    strcat(temp_str, "C");
    gfx_draw_string(x + 70, y + 50, temp_str, 0xFF8E8E93);
    
    // Power-on hours
    char hours_str[24];
    strcpy(hours_str, "Hours: ");
    int_to_str(dh->power_on_hours, hours_str + 7);
    gfx_draw_string(x + 70, y + 70, hours_str, 0xFF8E8E93);
    
    // Reallocated sectors (if any)
    if (dh->reallocated_sectors > 0) {
        char realloc_str[32];
        strcpy(realloc_str, "Bad Sectors: ");
        int_to_str(dh->reallocated_sectors, realloc_str + 13);
        gfx_draw_string(x + 70, y + 90, realloc_str, 0xFFFF3B30);
    }
}

// --- Detailed Rendering for Disk Utility ---

void disk_health_render_details(int x, int y, int drive_index) {
    DiskHealth* dh = disk_health_get(drive_index);
    if (!dh) return;
    
    // Background
    gfx_fill_rounded_rect(x, y, 600, 200, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 600, 200, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "S.M.A.R.T. Attributes", 0xFF1C1C1E);
    
    // Attributes table header
    int header_y = y + 40;
    gfx_draw_string(x + 16, header_y, "Attribute", 0xFF8E8E93);
    gfx_draw_string(x + 200, header_y, "Value", 0xFF8E8E93);
    gfx_draw_string(x + 280, header_y, "Worst", 0xFF8E8E93);
    gfx_draw_string(x + 360, header_y, "Thresh", 0xFF8E8E93);
    gfx_draw_string(x + 450, header_y, "Status", 0xFF8E8E93);
    
    gfx_draw_rect(x + 10, header_y + 18, 580, 1, 0xFFE5E5EA);
    
    // Attributes
    int attr_y = header_y + 24;
    for (int i = 0; i < dh->attr_count && i < 6; i++) {
        SmartAttribute* attr = &dh->attributes[i];
        
        gfx_draw_string(x + 16, attr_y, attr->name, 0xFF1C1C1E);
        
        char val_str[8];
        int_to_str(attr->value, val_str);
        gfx_draw_string(x + 200, attr_y, val_str, 0xFF1C1C1E);
        
        int_to_str(attr->worst, val_str);
        gfx_draw_string(x + 280, attr_y, val_str, 0xFF1C1C1E);
        
        int_to_str(attr->threshold, val_str);
        gfx_draw_string(x + 360, attr_y, val_str, 0xFF1C1C1E);
        
        // Status indicator
        const char* status_text;
        uint32_t status_color;
        switch (attr->status) {
            case 0: status_text = "OK"; status_color = 0xFF34C759; break;
            case 1: status_text = "Warning"; status_color = 0xFFFF9500; break;
            case 2: status_text = "Critical"; status_color = 0xFFFF3B30; break;
            default: status_text = "-"; status_color = 0xFF8E8E93; break;
        }
        gfx_draw_string(x + 450, attr_y, status_text, status_color);
        
        attr_y += 22;
    }
}
