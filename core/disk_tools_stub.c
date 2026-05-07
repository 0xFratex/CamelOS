// core/disk_tools_stub.c - Kernel-side stubs for disk utility functions
// These functions are fully implemented in the installer build (installer/disk_tools.c,
// installer/disk_health.c). For the kernel build, we provide stubs so the disk
// utility app can compile and link. When the user triggers these operations from
// the GUI, the stubs return safe defaults indicating the feature is not yet
// available in the running OS (only in the installer environment).

#include "../installer/disk_tools.h"
#include "../installer/disk_health.h"
#include "../hal/drivers/serial.h"
#include "string.h"

// ============================================================================
// Disk Benchmark Stubs
// ============================================================================

void disk_benchmark_init(DiskBenchmark* bench) {
    if (bench) {
        memset(bench, 0, sizeof(DiskBenchmark));
    }
}

int disk_benchmark_run(int drive_index, DiskBenchmark* bench, BenchmarkType type) {
    (void)drive_index; (void)bench; (void)type;
    s_printf("[DiskUtil] Benchmark not available in kernel mode\n");
    return -1;
}

int disk_benchmark_run_async(int drive_index, DiskBenchmark* bench, BenchmarkType type) {
    (void)drive_index; (void)bench; (void)type;
    return -1;
}

void disk_benchmark_stop(DiskBenchmark* bench) {
    (void)bench;
}

void disk_benchmark_render(int x, int y, DiskBenchmark* bench) {
    (void)x; (void)y; (void)bench;
}

// ============================================================================
// Bad Sector Scan Stubs
// ============================================================================

void bad_sector_scan_init(BadSectorScan* scan) {
    if (scan) memset(scan, 0, sizeof(BadSectorScan));
}

int bad_sector_scan_start(int drive_index, BadSectorScan* scan, ScanMode mode) {
    (void)drive_index; (void)scan; (void)mode;
    return -1;
}

void bad_sector_scan_pause(BadSectorScan* scan) { (void)scan; }
void bad_sector_scan_resume(BadSectorScan* scan) { (void)scan; }
void bad_sector_scan_stop(BadSectorScan* scan) { (void)scan; }
void bad_sector_scan_render(int x, int y, BadSectorScan* scan) { (void)x; (void)y; (void)scan; }

// ============================================================================
// Disk Wipe Stubs
// ============================================================================

void disk_wipe_init(DiskWipe* wipe) {
    if (wipe) memset(wipe, 0, sizeof(DiskWipe));
}

int disk_wipe_start(int drive_index, DiskWipe* wipe, WipeMode mode, uint32_t start, uint32_t end) {
    (void)drive_index; (void)wipe; (void)mode; (void)start; (void)end;
    return -1;
}

void disk_wipe_pause(DiskWipe* wipe) { (void)wipe; }
void disk_wipe_resume(DiskWipe* wipe) { (void)wipe; }
void disk_wipe_stop(DiskWipe* wipe) { (void)wipe; }
void disk_wipe_render(int x, int y, DiskWipe* wipe) { (void)x; (void)y; (void)wipe; }

// ============================================================================
// Disk Clone Stubs
// ============================================================================

void disk_clone_init(DiskClone* clone) {
    if (clone) memset(clone, 0, sizeof(DiskClone));
}

int disk_clone_start(int source, int target, DiskClone* clone, int sector_mode) {
    (void)source; (void)target; (void)clone; (void)sector_mode;
    return -1;
}

void disk_clone_pause(DiskClone* clone) { (void)clone; }
void disk_clone_resume(DiskClone* clone) { (void)clone; }
void disk_clone_stop(DiskClone* clone) { (void)clone; }
int disk_clone_verify(DiskClone* clone) { (void)clone; return -1; }
void disk_clone_render(int x, int y, DiskClone* clone) { (void)x; (void)y; (void)clone; }

// ============================================================================
// Surface Scan Stubs
// ============================================================================

void surface_scan_init(SurfaceScan* scan) {
    if (scan) memset(scan, 0, sizeof(SurfaceScan));
}

int surface_scan_start(int drive_index, SurfaceScan* scan, uint32_t start, uint32_t end) {
    (void)drive_index; (void)scan; (void)start; (void)end;
    s_printf("[DiskUtil] Surface scan not available in kernel mode\n");
    return -1;
}

void surface_scan_stop(SurfaceScan* scan) { (void)scan; }
void surface_scan_render(int x, int y, SurfaceScan* scan) { (void)x; (void)y; (void)scan; }

// ============================================================================
// Filesystem Check Stubs
// ============================================================================

void fs_check_init(FilesystemCheck* check) {
    if (check) memset(check, 0, sizeof(FilesystemCheck));
}

int fs_check_start(int drive_index, int partition_index, FilesystemCheck* check) {
    (void)drive_index; (void)partition_index; (void)check;
    return -1;
}

void fs_check_stop(FilesystemCheck* check) { (void)check; }
void fs_check_render(int x, int y, FilesystemCheck* check) { (void)x; (void)y; (void)check; }

// ============================================================================
// Disk Info Stubs
// ============================================================================

int disk_get_info(int drive_index, DiskInfo* info) {
    (void)drive_index; (void)info;
    return -1;
}

void disk_info_render(int x, int y, DiskInfo* info) { (void)x; (void)y; (void)info; }

// ============================================================================
// Operations Queue Stubs
// ============================================================================

void disk_queue_init(DiskOperationsQueue* queue) {
    if (queue) memset(queue, 0, sizeof(DiskOperationsQueue));
}

int disk_queue_add(DiskOperationsQueue* queue, DiskOperationType type, int drive_index, void* data) {
    (void)queue; (void)type; (void)drive_index; (void)data;
    return -1;
}

void disk_queue_start(DiskOperationsQueue* queue) { (void)queue; }
void disk_queue_stop(DiskOperationsQueue* queue) { (void)queue; }
void disk_queue_render(int x, int y, DiskOperationsQueue* queue) { (void)x; (void)y; (void)queue; }

// ============================================================================
// Utility Stubs
// ============================================================================

const char* wipe_mode_name(WipeMode mode) { (void)mode; return "Unknown"; }
const char* scan_mode_name(ScanMode mode) { (void)mode; return "Unknown"; }
const char* fs_type_name(FilesystemType type) { (void)type; return "Unknown"; }

void format_speed(uint32_t kb_per_sec, char* out) {
    (void)kb_per_sec;
    if (out) { out[0] = '0'; out[1] = '\0'; }
}

void format_size(uint64_t bytes, char* out) {
    (void)bytes;
    if (out) { out[0] = '0'; out[1] = '\0'; }
}

void format_time(int seconds, char* out) {
    (void)seconds;
    if (out) { out[0] = '0'; out[1] = '\0'; }
}

// ============================================================================
// Disk Health Stubs
// ============================================================================

void disk_health_init(void) {}

void disk_health_scan(int drive_index) { (void)drive_index; }

DiskHealth* disk_health_get(int drive_index) { (void)drive_index; return NULL; }

int disk_health_get_score(int drive_index) { (void)drive_index; return -1; }

HealthStatus disk_health_get_status(int drive_index) {
    (void)drive_index;
    return HEALTH_STATUS_UNKNOWN;
}

void disk_health_read_smart(int drive_index) { (void)drive_index; }

int disk_health_run_test(int drive_index, int test_type) {
    (void)drive_index; (void)test_type;
    return -1;
}

const char* disk_health_status_string(HealthStatus status) {
    (void)status;
    return "Unknown";
}

void disk_health_render_summary(int x, int y, int drive_index) {
    (void)x; (void)y; (void)drive_index;
}
