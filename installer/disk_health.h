// installer/disk_health.h - Disk Health Monitoring Header
#ifndef DISK_HEALTH_H
#define DISK_HEALTH_H

#include "../include/types.h"

// SMART-like attributes
typedef struct {
    uint16_t id;
    const char* name;
    uint8_t value;      // Current value (100 = good, lower = worse)
    uint8_t worst;      // Worst value ever recorded
    uint8_t threshold;  // Failure threshold
    uint8_t status;     // 0=OK, 1=Warning, 2=Critical
} SmartAttribute;

// Disk health summary
typedef struct {
    int drive_index;
    char model[41];
    char serial[21];
    uint32_t total_sectors;
    uint32_t power_on_hours;
    uint32_t spin_up_count;
    uint32_t reallocated_sectors;
    uint32_t pending_sectors;
    uint32_t crc_errors;
    int temperature;
    int health_score;      // 0-100
    int smart_supported;
    int smart_enabled;
    
    SmartAttribute attributes[16];
    int attr_count;
} DiskHealth;

// Health status codes
typedef enum {
    HEALTH_STATUS_GOOD,
    HEALTH_STATUS_WARNING,
    HEALTH_STATUS_CRITICAL,
    HEALTH_STATUS_UNKNOWN
} HealthStatus;

// Public API
void disk_health_init(void);
void disk_health_scan(int drive_index);
DiskHealth* disk_health_get(int drive_index);
int disk_health_get_score(int drive_index);
HealthStatus disk_health_get_status(int drive_index);

// SMART simulation
void disk_health_read_smart(int drive_index);
int disk_health_run_test(int drive_index, int test_type);

// Display helpers
const char* disk_health_status_string(HealthStatus status);
void disk_health_render_summary(int x, int y, int drive_index);

#endif // DISK_HEALTH_H
