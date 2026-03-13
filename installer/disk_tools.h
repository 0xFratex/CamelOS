// installer/disk_tools.h - Extended Disk Utility Tools Header
// Version 2.0 - Comprehensive disk management utilities
#ifndef DISK_TOOLS_H
#define DISK_TOOLS_H

#include "../include/types.h"

// ============================================================================
// DISK BENCHMARK
// ============================================================================

typedef struct {
    uint32_t read_speed_kb;      // KB/s
    uint32_t write_speed_kb;     // KB/s
    uint32_t random_read_speed;  // KB/s
    uint32_t random_write_speed; // KB/s
    uint32_t access_time_ms;     // Average access time in ms
    uint32_t burst_speed_kb;     // Burst speed in KB/s
    uint32_t test_sectors;       // Number of sectors tested
    int test_duration_ms;        // Duration of test in ms
    int is_ssd;                  // SSD detection
    int test_complete;
    int test_progress;           // 0-100
    char status_message[64];
} DiskBenchmark;

// Benchmark test types
typedef enum {
    BENCHMARK_SEQUENTIAL_READ,
    BENCHMARK_SEQUENTIAL_WRITE,
    BENCHMARK_RANDOM_READ,
    BENCHMARK_RANDOM_WRITE,
    BENCHMARK_MIXED,
    BENCHMARK_FULL
} BenchmarkType;

// ============================================================================
// BAD SECTOR SCAN
// ============================================================================

typedef struct {
    uint32_t start_sector;
    uint32_t end_sector;
    uint32_t sectors_scanned;
    uint32_t bad_sectors_found;
    uint32_t suspicious_sectors;  // Slow read sectors
    uint32_t recovered_sectors;   // Sectors recovered by re-read
    uint32_t bad_sector_list[256]; // List of bad sector LBA addresses
    int bad_sector_count;
    int scan_progress;            // 0-100
    int scan_active;
    int scan_paused;
    int scan_complete;
    uint32_t current_sector;
    uint32_t sectors_per_second;
    char status_message[64];
} BadSectorScan;

// Scan modes
typedef enum {
    SCAN_MODE_QUICK,        // Quick scan (read test only)
    SCAN_MODE_STANDARD,     // Standard scan (read + verify)
    SCAN_MODE_THOROUGH,     // Thorough scan (multiple passes)
    SCAN_MODE_DESTRUCTIVE   // Destructive write test (destroys data!)
} ScanMode;

// ============================================================================
// DISK WIPE
// ============================================================================

typedef enum {
    WIPE_MODE_ZEROS,        // Fill with zeros (quick)
    WIPE_MODE_ONES,         // Fill with ones
    WIPE_MODE_RANDOM,       // Fill with random data (1 pass)
    WIPE_MODE_DOD_SHORT,    // DoD 5220.22-M short (3 passes)
    WIPE_MODE_DOD_STANDARD, // DoD 5220.22-M standard (7 passes)
    WIPE_MODE_GUTMANN,      // Gutmann method (35 passes)
    WIPE_MODE_CUSTOM        // Custom pattern
} WipeMode;

typedef struct {
    uint32_t start_sector;
    uint32_t end_sector;
    WipeMode mode;
    int current_pass;
    int total_passes;
    uint32_t sectors_wiped;
    uint32_t sectors_total;
    int wipe_progress;          // 0-100
    int wipe_active;
    int wipe_complete;
    int verify_wipe;            // Verify after wipe
    int verification_passed;
    uint32_t verification_errors;
    char status_message[64];
} DiskWipe;

// ============================================================================
// DISK CLONE
// ============================================================================

typedef struct {
    int source_drive;
    int target_drive;
    uint32_t source_sectors;
    uint32_t target_sectors;
    uint32_t sectors_copied;
    uint32_t sectors_total;
    uint32_t sectors_failed;
    uint32_t bytes_per_second;
    int clone_progress;         // 0-100
    int clone_active;
    int clone_complete;
    int clone_mode;             // 0 = sector-by-sector, 1 = smart clone
    int verify_after_clone;
    int verification_passed;
    int skip_errors;            // Skip read errors
    char source_model[41];
    char target_model[41];
    char status_message[64];
} DiskClone;

// ============================================================================
// SURFACE SCAN
// ============================================================================

typedef struct {
    uint32_t start_sector;
    uint32_t end_sector;
    uint32_t sectors_scanned;
    uint32_t sectors_total;
    uint32_t damaged_sectors;
    uint32_t slow_sectors;      // Sectors with high latency
    uint32_t damaged_list[128];
    int damaged_count;
    uint32_t latency_map[1024]; // Latency per sector range (for visualization)
    int scan_progress;
    int scan_active;
    int scan_complete;
    int threshold_ms;           // Latency threshold for slow sector
    char status_message[64];
} SurfaceScan;

// ============================================================================
// FILESYSTEM CHECK
// ============================================================================

typedef enum {
    FS_CHECK_UNKNOWN,
    FS_CHECK_PFS32,
    FS_CHECK_FAT32,
    FS_CHECK_NTFS,
    FS_CHECK_EXT4
} FilesystemType;

typedef struct {
    FilesystemType fs_type;
    int partition_index;
    uint32_t partition_start;
    uint32_t partition_size;
    
    // Check results
    uint32_t total_files;
    uint32_t total_directories;
    uint32_t total_clusters;
    uint32_t used_clusters;
    uint32_t free_clusters;
    uint32_t bad_clusters;
    uint32_t lost_clusters;
    uint32_t cross_linked;
    uint32_t orphaned_files;
    uint32_t invalid_entries;
    uint32_t errors_found;
    uint32_t errors_fixed;
    
    // Check options
    int check_only;             // Don't fix errors
    int fix_errors;             // Automatically fix errors
    int recover_data;           // Try to recover lost data
    int verbose;                // Detailed output
    
    // Progress
    int check_progress;
    int check_active;
    int check_complete;
    char current_phase[32];
    char status_message[64];
} FilesystemCheck;

// ============================================================================
// DISK INFO
// ============================================================================

typedef struct {
    int drive_index;
    char model[41];
    char serial[21];
    char firmware[9];
    uint32_t total_sectors;
    uint64_t total_bytes;
    uint32_t sector_size;
    uint32_t cache_size;        // KB
    int is_ata;
    int is_atapi;
    int is_ssd;
    int has_smart;
    int smart_enabled;
    int is_removable;
    
    // Transfer modes
    int udma_mode;
    int pio_mode;
    int has_lba48;
    int has_ncq;
    
    // Geometry (for CHS)
    uint16_t cylinders;
    uint8_t heads;
    uint8_t sectors_per_track;
    
    // Health
    int health_score;
    int temperature;
    
} DiskInfo;

// ============================================================================
// OPERATIONS QUEUE
// ============================================================================

typedef enum {
    OP_NONE,
    OP_BENCHMARK,
    OP_SCAN,
    OP_WIPE,
    OP_CLONE,
    OP_SURFACE_SCAN,
    OP_FS_CHECK
} DiskOperationType;

typedef struct {
    DiskOperationType type;
    int drive_index;
    int priority;
    int status;             // 0=pending, 1=running, 2=complete, 3=failed
    void* operation_data;
    char description[64];
} DiskOperation;

typedef struct {
    DiskOperation operations[16];
    int operation_count;
    int current_operation;
    int queue_active;
} DiskOperationsQueue;

// ============================================================================
// PUBLIC API - BENCHMARK
// ============================================================================

void disk_benchmark_init(DiskBenchmark* bench);
int disk_benchmark_run(int drive_index, DiskBenchmark* bench, BenchmarkType type);
int disk_benchmark_run_async(int drive_index, DiskBenchmark* bench, BenchmarkType type);
void disk_benchmark_stop(DiskBenchmark* bench);
void disk_benchmark_render(int x, int y, DiskBenchmark* bench);

// ============================================================================
// PUBLIC API - BAD SECTOR SCAN
// ============================================================================

void bad_sector_scan_init(BadSectorScan* scan);
int bad_sector_scan_start(int drive_index, BadSectorScan* scan, ScanMode mode);
void bad_sector_scan_pause(BadSectorScan* scan);
void bad_sector_scan_resume(BadSectorScan* scan);
void bad_sector_scan_stop(BadSectorScan* scan);
void bad_sector_scan_render(int x, int y, BadSectorScan* scan);

// ============================================================================
// PUBLIC API - DISK WIPE
// ============================================================================

void disk_wipe_init(DiskWipe* wipe);
int disk_wipe_start(int drive_index, DiskWipe* wipe, WipeMode mode, uint32_t start, uint32_t end);
void disk_wipe_pause(DiskWipe* wipe);
void disk_wipe_resume(DiskWipe* wipe);
void disk_wipe_stop(DiskWipe* wipe);
void disk_wipe_render(int x, int y, DiskWipe* wipe);

// ============================================================================
// PUBLIC API - DISK CLONE
// ============================================================================

void disk_clone_init(DiskClone* clone);
int disk_clone_start(int source_drive, int target_drive, DiskClone* clone, int sector_mode);
void disk_clone_pause(DiskClone* clone);
void disk_clone_resume(DiskClone* clone);
void disk_clone_stop(DiskClone* clone);
int disk_clone_verify(DiskClone* clone);
void disk_clone_render(int x, int y, DiskClone* clone);

// ============================================================================
// PUBLIC API - SURFACE SCAN
// ============================================================================

void surface_scan_init(SurfaceScan* scan);
int surface_scan_start(int drive_index, SurfaceScan* scan, uint32_t start, uint32_t end);
void surface_scan_stop(SurfaceScan* scan);
void surface_scan_render(int x, int y, SurfaceScan* scan);

// ============================================================================
// PUBLIC API - FILESYSTEM CHECK
// ============================================================================

void fs_check_init(FilesystemCheck* check);
int fs_check_start(int drive_index, int partition_index, FilesystemCheck* check);
void fs_check_stop(FilesystemCheck* check);
void fs_check_render(int x, int y, FilesystemCheck* check);

// ============================================================================
// PUBLIC API - DISK INFO
// ============================================================================

int disk_get_info(int drive_index, DiskInfo* info);
void disk_info_render(int x, int y, DiskInfo* info);

// ============================================================================
// PUBLIC API - OPERATIONS QUEUE
// ============================================================================

void disk_queue_init(DiskOperationsQueue* queue);
int disk_queue_add(DiskOperationsQueue* queue, DiskOperationType type, int drive_index, void* data);
void disk_queue_start(DiskOperationsQueue* queue);
void disk_queue_stop(DiskOperationsQueue* queue);
void disk_queue_render(int x, int y, DiskOperationsQueue* queue);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

const char* wipe_mode_name(WipeMode mode);
const char* scan_mode_name(ScanMode mode);
const char* fs_type_name(FilesystemType type);
void format_speed(uint32_t kb_per_sec, char* out);
void format_size(uint64_t bytes, char* out);
void format_time(int seconds, char* out);

#endif // DISK_TOOLS_H
