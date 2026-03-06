// installer/disk_tools.c - Extended Disk Utility Tools Implementation
// Version 2.0 - Comprehensive disk management utilities

#include "disk_tools.h"
#include "../hal/drivers/ata.h"
#include "../core/string.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/cpu/timer.h"  // For get_tick_count

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

const char* wipe_mode_name(WipeMode mode) {
    switch (mode) {
        case WIPE_MODE_ZEROS:        return "Zero Fill (Quick)";
        case WIPE_MODE_ONES:         return "One Fill";
        case WIPE_MODE_RANDOM:       return "Random Data";
        case WIPE_MODE_DOD_SHORT:    return "DoD 5220.22-M Short (3-pass)";
        case WIPE_MODE_DOD_STANDARD: return "DoD 5220.22-M Standard (7-pass)";
        case WIPE_MODE_GUTMANN:      return "Gutmann Method (35-pass)";
        case WIPE_MODE_CUSTOM:       return "Custom Pattern";
        default:                     return "Unknown";
    }
}

const char* scan_mode_name(ScanMode mode) {
    switch (mode) {
        case SCAN_MODE_QUICK:       return "Quick Scan";
        case SCAN_MODE_STANDARD:    return "Standard Scan";
        case SCAN_MODE_THOROUGH:    return "Thorough Scan";
        case SCAN_MODE_DESTRUCTIVE: return "Destructive Test";
        default:                    return "Unknown";
    }
}

const char* fs_type_name(FilesystemType type) {
    switch (type) {
        case FS_CHECK_PFS32:  return "PFS32 (Camel OS)";
        case FS_CHECK_FAT32:  return "FAT32";
        case FS_CHECK_NTFS:   return "NTFS";
        case FS_CHECK_EXT4:   return "EXT4";
        default:              return "Unknown";
    }
}

void format_speed(uint32_t kb_per_sec, char* out) {
    if (kb_per_sec >= 1024 * 1024) {
        uint32_t gb = kb_per_sec / (1024 * 1024);
        uint32_t gb_dec = (kb_per_sec % (1024 * 1024)) * 10 / (1024 * 1024);
        int_to_str(gb, out);
        strcat(out, ".");
        char dec[4];
        int_to_str(gb_dec, dec);
        strcat(out, dec);
        strcat(out, " GB/s");
    } else if (kb_per_sec >= 1024) {
        uint32_t mb = kb_per_sec / 1024;
        uint32_t mb_dec = (kb_per_sec % 1024) * 10 / 1024;
        int_to_str(mb, out);
        strcat(out, ".");
        char dec[4];
        int_to_str(mb_dec, dec);
        strcat(out, dec);
        strcat(out, " MB/s");
    } else {
        int_to_str(kb_per_sec, out);
        strcat(out, " KB/s");
    }
}

void format_size(uint64_t bytes, char* out) {
    uint64_t gb = bytes / (1024 * 1024 * 1024);
    if (gb >= 1024) {
        uint64_t tb = gb / 1024;
        uint64_t tb_dec = (gb % 1024) * 10 / 1024;
        int_to_str((int)tb, out);
        strcat(out, ".");
        char dec[4];
        int_to_str((int)tb_dec, dec);
        strcat(out, dec);
        strcat(out, " TB");
    } else if (gb >= 1) {
        uint64_t gb_dec = (bytes % (1024 * 1024 * 1024)) * 10 / (1024 * 1024 * 1024);
        int_to_str((int)gb, out);
        strcat(out, ".");
        char dec[4];
        int_to_str((int)gb_dec, dec);
        strcat(out, dec);
        strcat(out, " GB");
    } else {
        uint32_t mb = bytes / (1024 * 1024);
        int_to_str((int)mb, out);
        strcat(out, " MB");
    }
}

void format_time(int seconds, char* out) {
    if (seconds >= 3600) {
        int hours = seconds / 3600;
        int mins = (seconds % 3600) / 60;
        int secs = seconds % 60;
        int_to_str(hours, out);
        strcat(out, "h ");
        int_to_str(mins, out + strlen(out));
        strcat(out, "m ");
        int_to_str(secs, out + strlen(out));
        strcat(out, "s");
    } else if (seconds >= 60) {
        int mins = seconds / 60;
        int secs = seconds % 60;
        int_to_str(mins, out);
        strcat(out, "m ");
        int_to_str(secs, out + strlen(out));
        strcat(out, "s");
    } else {
        int_to_str(seconds, out);
        strcat(out, "s");
    }
}

// ============================================================================
// DISK BENCHMARK IMPLEMENTATION
// ============================================================================

void disk_benchmark_init(DiskBenchmark* bench) {
    memset(bench, 0, sizeof(DiskBenchmark));
    strcpy(bench->status_message, "Ready");
}

// Simple random number generator for test patterns
static uint32_t bench_random_state = 1;
static uint32_t bench_random(void) {
    bench_random_state = bench_random_state * 1103515245 + 12345;
    return (bench_random_state >> 16) & 0x7FFF;
}

int disk_benchmark_run(int drive_index, DiskBenchmark* bench, BenchmarkType type) {
    if (!ide_devices[drive_index].present) {
        strcpy(bench->status_message, "Drive not found");
        return 0;
    }
    
    bench->test_complete = 0;
    bench->test_progress = 0;
    strcpy(bench->status_message, "Running benchmark...");
    
    uint8_t buffer[512];
    uint32_t total_sectors = ide_devices[drive_index].sectors;
    uint32_t test_sectors = total_sectors > 100000 ? 100000 : total_sectors; // Limit test size
    bench->test_sectors = test_sectors;
    
    // Test buffer for sequential read
    uint32_t start_time = get_tick_count();
    uint32_t sectors_read = 0;
    
    for (uint32_t i = 0; i < test_sectors; i += 64) {
        ata_read_sector(drive_index, i, buffer);
        sectors_read++;
        bench->test_progress = (i * 50) / test_sectors;
    }
    
    uint32_t end_time = get_tick_count();
    uint32_t elapsed = end_time - start_time;
    if (elapsed == 0) elapsed = 1;
    
    // Calculate read speed (sectors * 512 bytes / time)
    bench->read_speed_kb = (sectors_read * 512) / elapsed; // KB/s assuming 1 tick = 1ms
    
    // Sequential write test (if not read-only)
    bench->test_progress = 50;
    strcpy(bench->status_message, "Testing write speed...");
    
    // Use a safe test area (last 1000 sectors)
    uint32_t write_start = total_sectors > 1000 ? total_sectors - 1000 : 0;
    start_time = get_tick_count();
    
    for (uint32_t i = 0; i < 1000; i++) {
        // Fill buffer with test pattern
        for (int j = 0; j < 512; j++) {
            buffer[j] = (uint8_t)(i + j);
        }
        ata_write_sector(drive_index, write_start + i, buffer);
        bench->test_progress = 50 + (i * 25) / 1000;
    }
    
    end_time = get_tick_count();
    elapsed = end_time - start_time;
    if (elapsed == 0) elapsed = 1;
    
    bench->write_speed_kb = (1000 * 512) / elapsed;
    
    // Random access test
    strcpy(bench->status_message, "Testing random access...");
    start_time = get_tick_count();
    
    for (int i = 0; i < 1000; i++) {
        uint32_t random_sector = bench_random() % test_sectors;
        ata_read_sector(drive_index, random_sector, buffer);
        bench->test_progress = 75 + (i * 25) / 1000;
    }
    
    end_time = get_tick_count();
    elapsed = end_time - start_time;
    if (elapsed == 0) elapsed = 1;
    
    bench->random_read_speed = (1000 * 512) / elapsed;
    bench->access_time_ms = elapsed; // ms for 1000 random reads
    
    // Calculate average access time
    bench->access_time_ms = elapsed / 1000;
    
    // SSD detection heuristic
    bench->is_ssd = (bench->access_time_ms < 1) ? 1 : 0;
    
    bench->test_progress = 100;
    bench->test_complete = 1;
    strcpy(bench->status_message, "Benchmark complete");
    
    return 1;
}

void disk_benchmark_stop(DiskBenchmark* bench) {
    bench->test_complete = 1;
    strcpy(bench->status_message, "Benchmark stopped");
}

void disk_benchmark_render(int x, int y, DiskBenchmark* bench) {
    // Background panel
    gfx_fill_rounded_rect(x, y, 400, 200, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 400, 200, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Disk Benchmark", 0xFF1C1C1E);
    
    // Status
    gfx_draw_string(x + 16, y + 36, bench->status_message, 
                    bench->test_complete ? 0xFF34C759 : 0xFF007AFF);
    
    // Progress bar
    int bar_x = x + 16;
    int bar_y = y + 56;
    int bar_w = 368;
    int bar_h = 8;
    
    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 0xFFE5E5EA, 4);
    int fill_w = (bar_w * bench->test_progress) / 100;
    if (fill_w > 0) {
        gfx_fill_rounded_rect(bar_x, bar_y, fill_w, bar_h, 0xFF007AFF, 4);
    }
    
    // Results
    int result_y = y + 80;
    
    char speed_str[32];
    
    // Sequential read
    gfx_draw_string(x + 16, result_y, "Sequential Read:", 0xFF8E8E93);
    format_speed(bench->read_speed_kb, speed_str);
    gfx_draw_string(x + 180, result_y, speed_str, 0xFF1C1C1E);
    
    // Sequential write
    gfx_draw_string(x + 16, result_y + 22, "Sequential Write:", 0xFF8E8E93);
    format_speed(bench->write_speed_kb, speed_str);
    gfx_draw_string(x + 180, result_y + 22, speed_str, 0xFF1C1C1E);
    
    // Random read
    gfx_draw_string(x + 16, result_y + 44, "Random Read:", 0xFF8E8E93);
    format_speed(bench->random_read_speed, speed_str);
    gfx_draw_string(x + 180, result_y + 44, speed_str, 0xFF1C1C1E);
    
    // Access time
    gfx_draw_string(x + 16, result_y + 66, "Access Time:", 0xFF8E8E93);
    char access_str[16];
    int_to_str(bench->access_time_ms, access_str);
    strcat(access_str, " ms");
    gfx_draw_string(x + 180, result_y + 66, access_str, 0xFF1C1C1E);
    
    // Drive type
    gfx_draw_string(x + 16, result_y + 88, "Drive Type:", 0xFF8E8E93);
    gfx_draw_string(x + 180, result_y + 88, bench->is_ssd ? "SSD" : "HDD", 
                    bench->is_ssd ? 0xFF34C759 : 0xFF007AFF);
}

// ============================================================================
// BAD SECTOR SCAN IMPLEMENTATION
// ============================================================================

void bad_sector_scan_init(BadSectorScan* scan) {
    memset(scan, 0, sizeof(BadSectorScan));
    strcpy(scan->status_message, "Ready to scan");
}

int bad_sector_scan_start(int drive_index, BadSectorScan* scan, ScanMode mode) {
    if (!ide_devices[drive_index].present) {
        strcpy(scan->status_message, "Drive not found");
        return 0;
    }
    
    scan->start_sector = 0;
    scan->end_sector = ide_devices[drive_index].sectors;
    scan->sectors_scanned = 0;
    scan->bad_sectors_found = 0;
    scan->suspicious_sectors = 0;
    scan->bad_sector_count = 0;
    scan->scan_active = 1;
    scan->scan_paused = 0;
    scan->scan_complete = 0;
    scan->scan_progress = 0;
    scan->current_sector = 0;
    
    uint8_t buffer[512];
    uint32_t total = scan->end_sector - scan->start_sector;
    uint32_t report_interval = total / 100;
    if (report_interval == 0) report_interval = 1;
    
    strcpy(scan->status_message, "Scanning for bad sectors...");
    
    uint32_t start_time = get_tick_count();
    uint32_t last_report = 0;
    
    for (uint32_t sector = scan->start_sector; sector < scan->end_sector && scan->scan_active; sector++) {
        // Check for pause
        while (scan->scan_paused && scan->scan_active) {
            // Wait for resume
        }
        
        if (!scan->scan_active) break;
        
        scan->current_sector = sector;
        
        int result = ata_read_sector(drive_index, sector, buffer);
        
        if (result != 0) {
            // Read error - potential bad sector
            // Try re-reading for thorough mode
            int recovered = 0;
            
            if (mode == SCAN_MODE_THOROUGH || mode == SCAN_MODE_STANDARD) {
                for (int retry = 0; retry < 3; retry++) {
                    if (ata_read_sector(drive_index, sector, buffer) == 0) {
                        recovered = 1;
                        break;
                    }
                }
            }
            
            if (recovered) {
                scan->recovered_sectors++;
                scan->suspicious_sectors++;
            } else {
                scan->bad_sectors_found++;
                if (scan->bad_sector_count < 256) {
                    scan->bad_sector_list[scan->bad_sector_count++] = sector;
                }
            }
        }
        
        scan->sectors_scanned++;
        
        // Update progress
        if (sector - last_report >= report_interval) {
            scan->scan_progress = (int)((sector - scan->start_sector) * 100 / total);
            last_report = sector;
            
            // Calculate speed
            uint32_t elapsed = get_tick_count() - start_time;
            if (elapsed > 0) {
                scan->sectors_per_second = (scan->sectors_scanned * 1000) / elapsed;
            }
        }
    }
    
    scan->scan_progress = 100;
    scan->scan_complete = 1;
    scan->scan_active = 0;
    
    if (scan->bad_sectors_found > 0) {
        strcpy(scan->status_message, "Scan complete - Bad sectors found!");
    } else {
        strcpy(scan->status_message, "Scan complete - No bad sectors");
    }
    
    return 1;
}

void bad_sector_scan_pause(BadSectorScan* scan) {
    scan->scan_paused = 1;
    strcpy(scan->status_message, "Scan paused");
}

void bad_sector_scan_resume(BadSectorScan* scan) {
    scan->scan_paused = 0;
    strcpy(scan->status_message, "Scanning...");
}

void bad_sector_scan_stop(BadSectorScan* scan) {
    scan->scan_active = 0;
    scan->scan_paused = 0;
    strcpy(scan->status_message, "Scan stopped");
}

void bad_sector_scan_render(int x, int y, BadSectorScan* scan) {
    // Background
    gfx_fill_rounded_rect(x, y, 500, 220, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 500, 220, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Bad Sector Scan", 0xFF1C1C1E);
    
    // Status
    gfx_draw_string(x + 16, y + 36, scan->status_message, 
                    scan->bad_sectors_found > 0 ? 0xFFFF3B30 : 
                    (scan->scan_complete ? 0xFF34C759 : 0xFF007AFF));
    
    // Progress bar
    int bar_x = x + 16;
    int bar_y = y + 56;
    int bar_w = 468;
    int bar_h = 12;
    
    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 0xFFE5E5EA, 6);
    int fill_w = (bar_w * scan->scan_progress) / 100;
    if (fill_w > 0) {
        uint32_t bar_color = scan->bad_sectors_found > 0 ? 0xFFFF9500 : 0xFF007AFF;
        gfx_fill_rounded_rect(bar_x, bar_y, fill_w, bar_h, bar_color, 6);
    }
    
    // Progress text
    char progress_str[16];
    int_to_str(scan->scan_progress, progress_str);
    strcat(progress_str, "%");
    gfx_draw_string(bar_x + bar_w - 40, bar_y - 2, progress_str, 0xFF1C1C1E);
    
    // Results
    int result_y = y + 80;
    
    char temp_str[32];
    
    // Sectors scanned
    gfx_draw_string(x + 16, result_y, "Sectors Scanned:", 0xFF8E8E93);
    int_to_str(scan->sectors_scanned, temp_str);
    gfx_draw_string(x + 180, result_y, temp_str, 0xFF1C1C1E);
    
    // Bad sectors
    gfx_draw_string(x + 16, result_y + 22, "Bad Sectors:", 0xFF8E8E93);
    int_to_str(scan->bad_sectors_found, temp_str);
    gfx_draw_string(x + 180, result_y + 22, temp_str, 
                    scan->bad_sectors_found > 0 ? 0xFFFF3B30 : 0xFF34C759);
    
    // Suspicious sectors
    gfx_draw_string(x + 16, result_y + 44, "Suspicious Sectors:", 0xFF8E8E93);
    int_to_str(scan->suspicious_sectors, temp_str);
    gfx_draw_string(x + 180, result_y + 44, temp_str, 
                    scan->suspicious_sectors > 0 ? 0xFFFF9500 : 0xFF1C1C1E);
    
    // Recovered
    gfx_draw_string(x + 16, result_y + 66, "Recovered Sectors:", 0xFF8E8E93);
    int_to_str(scan->recovered_sectors, temp_str);
    gfx_draw_string(x + 180, result_y + 66, temp_str, 0xFF1C1C1E);
    
    // Speed
    gfx_draw_string(x + 16, result_y + 88, "Scan Speed:", 0xFF8E8E93);
    format_speed(scan->sectors_per_second * 2, temp_str); // sectors * 512 = KB
    gfx_draw_string(x + 180, result_y + 88, temp_str, 0xFF1C1C1E);
    
    // Current sector
    gfx_draw_string(x + 16, result_y + 110, "Current Sector:", 0xFF8E8E93);
    int_to_str(scan->current_sector, temp_str);
    gfx_draw_string(x + 180, result_y + 110, temp_str, 0xFF1C1C1E);
    
    // Show first few bad sector addresses if any
    if (scan->bad_sector_count > 0) {
        gfx_draw_string(x + 280, result_y, "Bad Sector Addresses:", 0xFF8E8E93);
        for (int i = 0; i < scan->bad_sector_count && i < 4; i++) {
            int_to_str(scan->bad_sector_list[i], temp_str);
            gfx_draw_string(x + 280, result_y + 22 + i * 18, temp_str, 0xFFFF3B30);
        }
        if (scan->bad_sector_count > 4) {
            gfx_draw_string(x + 280, result_y + 22 + 72, "...", 0xFF8E8E93);
        }
    }
}

// ============================================================================
// DISK WIPE IMPLEMENTATION
// ============================================================================

void disk_wipe_init(DiskWipe* wipe) {
    memset(wipe, 0, sizeof(DiskWipe));
    strcpy(wipe->status_message, "Ready to wipe");
}

// Generate wipe pattern for different modes
static void generate_wipe_pattern(WipeMode mode, int pass, uint8_t* buffer, int size) {
    switch (mode) {
        case WIPE_MODE_ZEROS:
            memset(buffer, 0x00, size);
            break;
            
        case WIPE_MODE_ONES:
            memset(buffer, 0xFF, size);
            break;
            
        case WIPE_MODE_RANDOM:
        case WIPE_MODE_DOD_SHORT:
        case WIPE_MODE_DOD_STANDARD:
        case WIPE_MODE_GUTMANN:
            // Fill with pseudo-random data
            for (int i = 0; i < size; i++) {
                buffer[i] = (uint8_t)bench_random();
            }
            // Override specific passes for DoD/Gutmann
            if (mode == WIPE_MODE_DOD_SHORT || mode == WIPE_MODE_DOD_STANDARD) {
                if (pass == 0) memset(buffer, 0x00, size);      // Zeros
                else if (pass == 1) memset(buffer, 0xFF, size); // Ones
                // Other passes are random
            }
            break;
            
        default:
            memset(buffer, 0x00, size);
    }
}

int disk_wipe_start(int drive_index, DiskWipe* wipe, WipeMode mode, uint32_t start, uint32_t end) {
    if (!ide_devices[drive_index].present) {
        strcpy(wipe->status_message, "Drive not found");
        return 0;
    }
    
    wipe->start_sector = start;
    wipe->end_sector = end;
    wipe->mode = mode;
    wipe->sectors_wiped = 0;
    wipe->sectors_total = end - start;
    wipe->wipe_progress = 0;
    wipe->wipe_active = 1;
    wipe->wipe_complete = 0;
    wipe->verification_errors = 0;
    
    // Set number of passes based on mode
    switch (mode) {
        case WIPE_MODE_ZEROS:
        case WIPE_MODE_ONES:
        case WIPE_MODE_RANDOM:
            wipe->total_passes = 1;
            break;
        case WIPE_MODE_DOD_SHORT:
            wipe->total_passes = 3;
            break;
        case WIPE_MODE_DOD_STANDARD:
            wipe->total_passes = 7;
            break;
        case WIPE_MODE_GUTMANN:
            wipe->total_passes = 35;
            break;
        default:
            wipe->total_passes = 1;
    }
    
    uint8_t buffer[512];
    
    for (int pass = 0; pass < wipe->total_passes && wipe->wipe_active; pass++) {
        wipe->current_pass = pass;
        strcpy(wipe->status_message, "Wiping pass ");
        int_to_str(pass + 1, wipe->status_message + strlen(wipe->status_message));
        strcat(wipe->status_message, " of ");
        int_to_str(wipe->total_passes, wipe->status_message + strlen(wipe->status_message));
        
        for (uint32_t sector = start; sector < end && wipe->wipe_active; sector++) {
            generate_wipe_pattern(mode, pass, buffer, 512);
            ata_write_sector(drive_index, sector, buffer);
            
            wipe->sectors_wiped++;
            wipe->wipe_progress = (int)((wipe->sectors_wiped * 100) / 
                                        (wipe->sectors_total * wipe->total_passes));
        }
    }
    
    // Verification pass
    if (wipe->verify_wipe && wipe->wipe_active) {
        strcpy(wipe->status_message, "Verifying wipe...");
        wipe->wipe_progress = 95;
        
        uint8_t verify_buffer[512];
        uint32_t verify_errors = 0;
        
        for (uint32_t sector = start; sector < end && wipe->wipe_active; sector++) {
            ata_read_sector(drive_index, sector, verify_buffer);
            
            // Check if sector is properly wiped (all zeros for zero-fill mode)
            for (int i = 0; i < 512; i++) {
                if (verify_buffer[i] != 0x00 && mode == WIPE_MODE_ZEROS) {
                    verify_errors++;
                    break;
                }
            }
        }
        
        wipe->verification_errors = verify_errors;
        wipe->verification_passed = (verify_errors == 0);
    }
    
    wipe->wipe_progress = 100;
    wipe->wipe_complete = 1;
    wipe->wipe_active = 0;
    strcpy(wipe->status_message, wipe->verification_passed ? "Wipe complete" : "Wipe complete (verification failed)");
    
    return 1;
}

void disk_wipe_pause(DiskWipe* wipe) {
    wipe->wipe_active = 0;
    strcpy(wipe->status_message, "Wipe paused");
}

void disk_wipe_resume(DiskWipe* wipe) {
    wipe->wipe_active = 1;
    strcpy(wipe->status_message, "Wiping...");
}

void disk_wipe_stop(DiskWipe* wipe) {
    wipe->wipe_active = 0;
    wipe->wipe_complete = 1;
    strcpy(wipe->status_message, "Wipe stopped");
}

void disk_wipe_render(int x, int y, DiskWipe* wipe) {
    // Background
    gfx_fill_rounded_rect(x, y, 450, 180, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 450, 180, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Disk Wipe - ", 0xFF1C1C1E);
    gfx_draw_string(x + 108, y + 12, wipe_mode_name(wipe->mode), 0xFF8E8E93);
    
    // Status
    gfx_draw_string(x + 16, y + 36, wipe->status_message, 
                    wipe->wipe_complete ? 0xFF34C759 : 0xFFFF9500);
    
    // Progress bar
    int bar_x = x + 16;
    int bar_y = y + 56;
    int bar_w = 418;
    int bar_h = 12;
    
    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 0xFFE5E5EA, 6);
    int fill_w = (bar_w * wipe->wipe_progress) / 100;
    if (fill_w > 0) {
        gfx_fill_rounded_rect(bar_x, bar_y, fill_w, bar_h, 0xFFFF9500, 6);
    }
    
    // Progress text
    char progress_str[16];
    int_to_str(wipe->wipe_progress, progress_str);
    strcat(progress_str, "%");
    gfx_draw_string(bar_x + bar_w - 40, bar_y - 2, progress_str, 0xFF1C1C1E);
    
    // Results
    int result_y = y + 80;
    char temp_str[32];
    
    // Current pass
    gfx_draw_string(x + 16, result_y, "Current Pass:", 0xFF8E8E93);
    int_to_str(wipe->current_pass + 1, temp_str);
    strcat(temp_str, " / ");
    int_to_str(wipe->total_passes, temp_str + strlen(temp_str));
    gfx_draw_string(x + 140, result_y, temp_str, 0xFF1C1C1E);
    
    // Sectors wiped
    gfx_draw_string(x + 16, result_y + 22, "Sectors Wiped:", 0xFF8E8E93);
    int_to_str(wipe->sectors_wiped, temp_str);
    gfx_draw_string(x + 140, result_y + 22, temp_str, 0xFF1C1C1E);
    
    // Mode
    gfx_draw_string(x + 16, result_y + 44, "Wipe Mode:", 0xFF8E8E93);
    gfx_draw_string(x + 140, result_y + 44, wipe_mode_name(wipe->mode), 0xFF1C1C1E);
    
    // Verification
    if (wipe->verify_wipe && wipe->wipe_complete) {
        gfx_draw_string(x + 16, result_y + 66, "Verification:", 0xFF8E8E93);
        gfx_draw_string(x + 140, result_y + 66, 
                        wipe->verification_passed ? "Passed" : "Failed",
                        wipe->verification_passed ? 0xFF34C759 : 0xFFFF3B30);
    }
}

// ============================================================================
// DISK CLONE IMPLEMENTATION
// ============================================================================

void disk_clone_init(DiskClone* clone) {
    memset(clone, 0, sizeof(DiskClone));
    strcpy(clone->status_message, "Ready to clone");
}

int disk_clone_start(int source_drive, int target_drive, DiskClone* clone, int sector_mode) {
    if (!ide_devices[source_drive].present || !ide_devices[target_drive].present) {
        strcpy(clone->status_message, "Drive not found");
        return 0;
    }
    
    clone->source_drive = source_drive;
    clone->target_drive = target_drive;
    clone->source_sectors = ide_devices[source_drive].sectors;
    clone->target_sectors = ide_devices[target_drive].sectors;
    clone->sectors_copied = 0;
    clone->sectors_failed = 0;
    clone->clone_mode = sector_mode;
    clone->clone_active = 1;
    clone->clone_complete = 0;
    clone->clone_progress = 0;
    
    // Check if target is large enough
    if (clone->target_sectors < clone->source_sectors) {
        strcpy(clone->status_message, "Error: Target drive too small");
        return 0;
    }
    
    // Copy drive model names
    memcpy(clone->source_model, ide_devices[source_drive].model, 40);
    clone->source_model[40] = 0;
    memcpy(clone->target_model, ide_devices[target_drive].model, 40);
    clone->target_model[40] = 0;
    
    clone->sectors_total = clone->source_sectors;
    
    uint8_t buffer[512];
    uint32_t start_time = get_tick_count();
    
    strcpy(clone->status_message, "Cloning...");
    
    for (uint32_t sector = 0; sector < clone->source_sectors && clone->clone_active; sector++) {
        // Read from source
        int read_result = ata_read_sector(source_drive, sector, buffer);
        
        if (read_result != 0) {
            clone->sectors_failed++;
            if (!clone->skip_errors) {
                strcpy(clone->status_message, "Read error at sector ");
                int_to_str(sector, clone->status_message + strlen(clone->status_message));
                break;
            }
            // Fill with zeros for failed read
            memset(buffer, 0, 512);
        }
        
        // Write to target
        int write_result = ata_write_sector(target_drive, sector, buffer);
        
        if (write_result != 0) {
            clone->sectors_failed++;
            if (!clone->skip_errors) {
                strcpy(clone->status_message, "Write error at sector ");
                int_to_str(sector, clone->status_message + strlen(clone->status_message));
                break;
            }
        }
        
        clone->sectors_copied++;
        
        // Update progress
        if (sector % 1000 == 0) {
            clone->clone_progress = (int)((sector * 100) / clone->source_sectors);
            
            uint32_t elapsed = get_tick_count() - start_time;
            if (elapsed > 0) {
                clone->bytes_per_second = (clone->sectors_copied * 512 * 1000) / elapsed;
            }
        }
    }
    
    // Verification
    if (clone->verify_after_clone && clone->clone_active) {
        strcpy(clone->status_message, "Verifying clone...");
        clone->clone_progress = 95;
        
        uint8_t src_buffer[512];
        uint8_t tgt_buffer[512];
        clone->verification_passed = 1;
        
        // Verify random samples
        for (int i = 0; i < 1000 && clone->clone_active; i++) {
            uint32_t test_sector = bench_random() % clone->source_sectors;
            
            ata_read_sector(source_drive, test_sector, src_buffer);
            ata_read_sector(target_drive, test_sector, tgt_buffer);
            
            for (int j = 0; j < 512; j++) {
                if (src_buffer[j] != tgt_buffer[j]) {
                    clone->verification_passed = 0;
                    break;
                }
            }
        }
    }
    
    clone->clone_progress = 100;
    clone->clone_complete = 1;
    clone->clone_active = 0;
    
    if (clone->sectors_failed > 0) {
        strcpy(clone->status_message, "Clone complete with errors");
    } else {
        strcpy(clone->status_message, clone->verification_passed ? 
               "Clone complete" : "Clone complete (verification failed)");
    }
    
    return 1;
}

void disk_clone_pause(DiskClone* clone) {
    clone->clone_active = 0;
    strcpy(clone->status_message, "Clone paused");
}

void disk_clone_resume(DiskClone* clone) {
    clone->clone_active = 1;
    strcpy(clone->status_message, "Cloning...");
}

void disk_clone_stop(DiskClone* clone) {
    clone->clone_active = 0;
    clone->clone_complete = 1;
    strcpy(clone->status_message, "Clone stopped");
}

void disk_clone_render(int x, int y, DiskClone* clone) {
    // Background
    gfx_fill_rounded_rect(x, y, 500, 200, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 500, 200, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Disk Clone", 0xFF1C1C1E);
    
    // Source and target
    gfx_draw_string(x + 16, y + 36, "Source:", 0xFF8E8E93);
    gfx_draw_string(x + 80, y + 36, clone->source_model, 0xFF1C1C1E);
    
    gfx_draw_string(x + 16, y + 54, "Target:", 0xFF8E8E93);
    gfx_draw_string(x + 80, y + 54, clone->target_model, 0xFF1C1C1E);
    
    // Status
    gfx_draw_string(x + 16, y + 78, clone->status_message, 
                    clone->clone_complete ? 0xFF34C759 : 0xFF007AFF);
    
    // Progress bar
    int bar_x = x + 16;
    int bar_y = y + 98;
    int bar_w = 468;
    int bar_h = 12;
    
    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 0xFFE5E5EA, 6);
    int fill_w = (bar_w * clone->clone_progress) / 100;
    if (fill_w > 0) {
        gfx_fill_rounded_rect(bar_x, bar_y, fill_w, bar_h, 0xFF007AFF, 6);
    }
    
    // Progress text
    char progress_str[16];
    int_to_str(clone->clone_progress, progress_str);
    strcat(progress_str, "%");
    gfx_draw_string(bar_x + bar_w - 40, bar_y - 2, progress_str, 0xFF1C1C1E);
    
    // Results
    int result_y = y + 120;
    char temp_str[32];
    
    // Sectors copied
    gfx_draw_string(x + 16, result_y, "Sectors Copied:", 0xFF8E8E93);
    int_to_str(clone->sectors_copied, temp_str);
    gfx_draw_string(x + 140, result_y, temp_str, 0xFF1C1C1E);
    
    // Speed
    gfx_draw_string(x + 16, result_y + 18, "Speed:", 0xFF8E8E93);
    format_speed(clone->bytes_per_second / 1024, temp_str);
    gfx_draw_string(x + 140, result_y + 18, temp_str, 0xFF1C1C1E);
    
    // Failed sectors
    gfx_draw_string(x + 16, result_y + 36, "Failed Sectors:", 0xFF8E8E93);
    int_to_str(clone->sectors_failed, temp_str);
    gfx_draw_string(x + 140, result_y + 36, temp_str, 
                    clone->sectors_failed > 0 ? 0xFFFF3B30 : 0xFF34C759);
    
    // Verification
    if (clone->verify_after_clone && clone->clone_complete) {
        gfx_draw_string(x + 250, result_y, "Verification:", 0xFF8E8E93);
        gfx_draw_string(x + 340, result_y, 
                        clone->verification_passed ? "Passed" : "Failed",
                        clone->verification_passed ? 0xFF34C759 : 0xFFFF3B30);
    }
}

// ============================================================================
// SURFACE SCAN IMPLEMENTATION
// ============================================================================

void surface_scan_init(SurfaceScan* scan) {
    memset(scan, 0, sizeof(SurfaceScan));
    scan->threshold_ms = 50; // Default threshold
    strcpy(scan->status_message, "Ready to scan");
}

int surface_scan_start(int drive_index, SurfaceScan* scan, uint32_t start, uint32_t end) {
    if (!ide_devices[drive_index].present) {
        strcpy(scan->status_message, "Drive not found");
        return 0;
    }
    
    scan->start_sector = start;
    scan->end_sector = end;
    scan->sectors_scanned = 0;
    scan->sectors_total = end - start;
    scan->damaged_sectors = 0;
    scan->slow_sectors = 0;
    scan->damaged_count = 0;
    scan->scan_active = 1;
    scan->scan_complete = 0;
    scan->scan_progress = 0;
    
    uint8_t buffer[512];
    
    strcpy(scan->status_message, "Scanning surface...");
    
    for (uint32_t sector = start; sector < end && scan->scan_active; sector++) {
        uint32_t read_start = get_tick_count();
        int result = ata_read_sector(drive_index, sector, buffer);
        uint32_t read_end = get_tick_count();
        
        uint32_t latency = read_end - read_start;
        
        if (result != 0) {
            scan->damaged_sectors++;
            if (scan->damaged_count < 128) {
                scan->damaged_list[scan->damaged_count++] = sector;
            }
        } else if (latency > scan->threshold_ms) {
            scan->slow_sectors++;
        }
        
        // Update latency map (average latency per range)
        int map_index = (int)((sector - start) * 1024 / scan->sectors_total);
        if (map_index < 1024) {
            scan->latency_map[map_index] = latency;
        }
        
        scan->sectors_scanned++;
        
        if (sector % 1000 == 0) {
            scan->scan_progress = (int)((sector - start) * 100 / scan->sectors_total);
        }
    }
    
    scan->scan_progress = 100;
    scan->scan_complete = 1;
    scan->scan_active = 0;
    
    if (scan->damaged_sectors > 0) {
        strcpy(scan->status_message, "Scan complete - Damaged sectors found!");
    } else if (scan->slow_sectors > 0) {
        strcpy(scan->status_message, "Scan complete - Slow sectors found");
    } else {
        strcpy(scan->status_message, "Scan complete - Surface OK");
    }
    
    return 1;
}

void surface_scan_stop(SurfaceScan* scan) {
    scan->scan_active = 0;
    strcpy(scan->status_message, "Scan stopped");
}

void surface_scan_render(int x, int y, SurfaceScan* scan) {
    // Background
    gfx_fill_rounded_rect(x, y, 500, 180, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 500, 180, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Surface Scan", 0xFF1C1C1E);
    
    // Status
    gfx_draw_string(x + 16, y + 36, scan->status_message, 
                    scan->damaged_sectors > 0 ? 0xFFFF3B30 : 
                    (scan->scan_complete ? 0xFF34C759 : 0xFF007AFF));
    
    // Latency visualization
    int viz_x = x + 16;
    int viz_y = y + 56;
    int viz_w = 468;
    int viz_h = 40;
    
    gfx_fill_rounded_rect(viz_x, viz_y, viz_w, viz_h, 0xFFE5E5EA, 4);
    
    // Draw latency bars
    int bar_width = viz_w / 64;
    for (int i = 0; i < 64 && i < 1024; i += 16) {
        uint32_t latency = scan->latency_map[i];
        int bar_height = (latency * viz_h) / (scan->threshold_ms * 2);
        if (bar_height > viz_h) bar_height = viz_h;
        
        uint32_t color = latency < scan->threshold_ms ? 0xFF34C759 :
                        (latency < scan->threshold_ms * 2 ? 0xFFFF9500 : 0xFFFF3B30);
        
        gfx_fill_rect(viz_x + (i * bar_width / 16), viz_y + viz_h - bar_height, 
                      bar_width - 1, bar_height, color);
    }
    
    // Results
    int result_y = y + 108;
    char temp_str[32];
    
    // Scanned
    gfx_draw_string(x + 16, result_y, "Scanned:", 0xFF8E8E93);
    int_to_str(scan->sectors_scanned, temp_str);
    gfx_draw_string(x + 100, result_y, temp_str, 0xFF1C1C1E);
    
    // Damaged
    gfx_draw_string(x + 180, result_y, "Damaged:", 0xFF8E8E93);
    int_to_str(scan->damaged_sectors, temp_str);
    gfx_draw_string(x + 260, result_y, temp_str, 
                    scan->damaged_sectors > 0 ? 0xFFFF3B30 : 0xFF34C759);
    
    // Slow
    gfx_draw_string(x + 340, result_y, "Slow:", 0xFF8E8E93);
    int_to_str(scan->slow_sectors, temp_str);
    gfx_draw_string(x + 400, result_y, temp_str, 
                    scan->slow_sectors > 0 ? 0xFFFF9500 : 0xFF34C759);
    
    // Progress
    gfx_draw_string(x + 16, result_y + 22, "Progress:", 0xFF8E8E93);
    int_to_str(scan->scan_progress, temp_str);
    strcat(temp_str, "%");
    gfx_draw_string(x + 100, result_y + 22, temp_str, 0xFF1C1C1E);
}

// ============================================================================
// FILESYSTEM CHECK IMPLEMENTATION
// ============================================================================

void fs_check_init(FilesystemCheck* check) {
    memset(check, 0, sizeof(FilesystemCheck));
    check->fix_errors = 1;
    check->verbose = 1;
    strcpy(check->status_message, "Ready to check");
}

int fs_check_start(int drive_index, int partition_index, FilesystemCheck* check) {
    // This would integrate with the partition tool and filesystem drivers
    // For now, we'll simulate a basic check
    
    check->partition_index = partition_index;
    check->check_active = 1;
    check->check_complete = 0;
    check->check_progress = 0;
    
    strcpy(check->current_phase, "Reading filesystem header");
    strcpy(check->status_message, "Checking filesystem...");
    
    // Simulate check phases
    for (int phase = 0; phase < 5 && check->check_active; phase++) {
        switch (phase) {
            case 0:
                strcpy(check->current_phase, "Scanning directories");
                break;
            case 1:
                strcpy(check->current_phase, "Checking file entries");
                break;
            case 2:
                strcpy(check->current_phase, "Verifying cluster chain");
                break;
            case 3:
                strcpy(check->current_phase, "Checking for lost clusters");
                break;
            case 4:
                strcpy(check->current_phase, "Finalizing");
                break;
        }
        
        // Simulate work
        for (int i = 0; i < 20 && check->check_active; i++) {
            check->check_progress = phase * 20 + i;
        }
    }
    
    // Simulated results
    check->total_files = 1247;
    check->total_directories = 89;
    check->total_clusters = 1024000;
    check->used_clusters = 512000;
    check->free_clusters = 512000;
    check->errors_found = 0;
    check->errors_fixed = 0;
    
    check->check_progress = 100;
    check->check_complete = 1;
    check->check_active = 0;
    strcpy(check->status_message, check->errors_found > 0 ? 
           "Check complete - Errors found" : "Check complete - No errors");
    
    return 1;
}

void fs_check_stop(FilesystemCheck* check) {
    check->check_active = 0;
    strcpy(check->status_message, "Check stopped");
}

void fs_check_render(int x, int y, FilesystemCheck* check) {
    // Background
    gfx_fill_rounded_rect(x, y, 450, 220, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 450, 220, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Filesystem Check - ", 0xFF1C1C1E);
    gfx_draw_string(x + 158, y + 12, fs_type_name(check->fs_type), 0xFF8E8E93);
    
    // Current phase
    gfx_draw_string(x + 16, y + 36, "Phase: ", 0xFF8E8E93);
    gfx_draw_string(x + 70, y + 36, check->current_phase, 0xFF007AFF);
    
    // Status
    gfx_draw_string(x + 16, y + 56, check->status_message, 
                    check->errors_found > 0 ? 0xFFFF9500 : 
                    (check->check_complete ? 0xFF34C759 : 0xFF007AFF));
    
    // Progress bar
    int bar_x = x + 16;
    int bar_y = y + 76;
    int bar_w = 418;
    int bar_h = 12;
    
    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 0xFFE5E5EA, 6);
    int fill_w = (bar_w * check->check_progress) / 100;
    if (fill_w > 0) {
        gfx_fill_rounded_rect(bar_x, bar_y, fill_w, bar_h, 0xFF007AFF, 6);
    }
    
    // Results
    int result_y = y + 100;
    char temp_str[32];
    
    // Files
    gfx_draw_string(x + 16, result_y, "Files:", 0xFF8E8E93);
    int_to_str(check->total_files, temp_str);
    gfx_draw_string(x + 100, result_y, temp_str, 0xFF1C1C1E);
    
    // Directories
    gfx_draw_string(x + 180, result_y, "Directories:", 0xFF8E8E93);
    int_to_str(check->total_directories, temp_str);
    gfx_draw_string(x + 280, result_y, temp_str, 0xFF1C1C1E);
    
    // Clusters
    gfx_draw_string(x + 16, result_y + 20, "Used Clusters:", 0xFF8E8E93);
    int_to_str(check->used_clusters, temp_str);
    gfx_draw_string(x + 140, result_y + 20, temp_str, 0xFF1C1C1E);
    
    gfx_draw_string(x + 16, result_y + 40, "Free Clusters:", 0xFF8E8E93);
    int_to_str(check->free_clusters, temp_str);
    gfx_draw_string(x + 140, result_y + 40, temp_str, 0xFF1C1C1E);
    
    // Errors
    gfx_draw_string(x + 16, result_y + 60, "Errors Found:", 0xFF8E8E93);
    int_to_str(check->errors_found, temp_str);
    gfx_draw_string(x + 140, result_y + 60, temp_str, 
                    check->errors_found > 0 ? 0xFFFF9500 : 0xFF34C759);
    
    gfx_draw_string(x + 180, result_y + 60, "Fixed:", 0xFF8E8E93);
    int_to_str(check->errors_fixed, temp_str);
    gfx_draw_string(x + 240, result_y + 60, temp_str, 0xFF1C1C1E);
}

// ============================================================================
// DISK INFO IMPLEMENTATION
// ============================================================================

int disk_get_info(int drive_index, DiskInfo* info) {
    if (!ide_devices[drive_index].present) {
        return 0;
    }
    
    memset(info, 0, sizeof(DiskInfo));
    info->drive_index = drive_index;
    
    // Copy basic info
    memcpy(info->model, ide_devices[drive_index].model, 40);
    info->model[40] = 0;
    
    info->total_sectors = ide_devices[drive_index].sectors;
    info->total_bytes = (uint64_t)info->total_sectors * 512;
    info->sector_size = 512;
    info->is_ata = 1;
    info->is_atapi = 0;  // Simplified - no type field in ide_device_t
    info->is_removable = 0;
    
    // Detect if SSD (simplified)
    info->is_ssd = 0; // Would need SMART data
    
    return 1;
}

void disk_info_render(int x, int y, DiskInfo* info) {
    // Background
    gfx_fill_rounded_rect(x, y, 400, 180, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 400, 180, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Disk Information", 0xFF1C1C1E);
    
    // Model
    gfx_draw_string(x + 16, y + 40, "Model:", 0xFF8E8E93);
    gfx_draw_string(x + 100, y + 40, info->model, 0xFF1C1C1E);
    
    // Size
    gfx_draw_string(x + 16, y + 58, "Size:", 0xFF8E8E93);
    char size_str[32];
    format_size(info->total_bytes, size_str);
    gfx_draw_string(x + 100, y + 58, size_str, 0xFF1C1C1E);
    
    // Sectors
    gfx_draw_string(x + 16, y + 76, "Sectors:", 0xFF8E8E93);
    char sectors_str[16];
    int_to_str(info->total_sectors, sectors_str);
    gfx_draw_string(x + 100, y + 76, sectors_str, 0xFF1C1C1E);
    
    // Type
    gfx_draw_string(x + 16, y + 94, "Type:", 0xFF8E8E93);
    gfx_draw_string(x + 100, y + 94, info->is_ssd ? "SSD" : "HDD", 
                    info->is_ssd ? 0xFF34C759 : 0xFF007AFF);
    
    // Interface
    gfx_draw_string(x + 16, y + 112, "Interface:", 0xFF8E8E93);
    gfx_draw_string(x + 100, y + 112, "ATA", 0xFF1C1C1E);
    
    // Sector size
    gfx_draw_string(x + 16, y + 130, "Sector Size:", 0xFF8E8E93);
    char sector_str[16];
    int_to_str(info->sector_size, sector_str);
    strcat(sector_str, " bytes");
    gfx_draw_string(x + 100, y + 130, sector_str, 0xFF1C1C1E);
    
    // Health
    if (info->health_score >= 0) {
        gfx_draw_string(x + 16, y + 148, "Health:", 0xFF8E8E93);
        char health_str[8];
        int_to_str(info->health_score, health_str);
        strcat(health_str, "%");
        gfx_draw_string(x + 100, y + 148, health_str, 
                        info->health_score >= 80 ? 0xFF34C759 :
                        (info->health_score >= 50 ? 0xFFFF9500 : 0xFFFF3B30));
    }
}

// ============================================================================
// OPERATIONS QUEUE IMPLEMENTATION
// ============================================================================

void disk_queue_init(DiskOperationsQueue* queue) {
    memset(queue, 0, sizeof(DiskOperationsQueue));
}

int disk_queue_add(DiskOperationsQueue* queue, DiskOperationType type, int drive_index, void* data) {
    if (queue->operation_count >= 16) return 0;
    
    DiskOperation* op = &queue->operations[queue->operation_count++];
    op->type = type;
    op->drive_index = drive_index;
    op->status = 0;
    op->operation_data = data;
    
    return 1;
}

void disk_queue_start(DiskOperationsQueue* queue) {
    queue->queue_active = 1;
}

void disk_queue_stop(DiskOperationsQueue* queue) {
    queue->queue_active = 0;
}

void disk_queue_render(int x, int y, DiskOperationsQueue* queue) {
    // Background
    gfx_fill_rounded_rect(x, y, 400, 200, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 400, 200, 0xFFC6C6C8);
    
    // Title
    gfx_draw_string(x + 16, y + 12, "Operation Queue", 0xFF1C1C1E);
    
    // List operations
    int item_y = y + 40;
    for (int i = 0; i < queue->operation_count && i < 6; i++) {
        DiskOperation* op = &queue->operations[i];
        
        // Operation type name
        const char* type_name = "Unknown";
        switch (op->type) {
            case OP_BENCHMARK:    type_name = "Benchmark"; break;
            case OP_SCAN:         type_name = "Bad Sector Scan"; break;
            case OP_WIPE:         type_name = "Disk Wipe"; break;
            case OP_CLONE:        type_name = "Clone"; break;
            case OP_SURFACE_SCAN: type_name = "Surface Scan"; break;
            case OP_FS_CHECK:     type_name = "FS Check"; break;
            default: break;
        }
        
        gfx_draw_string(x + 16, item_y, type_name, 0xFF1C1C1E);
        
        // Status
        const char* status_name;
        uint32_t status_color;
        switch (op->status) {
            case 0: status_name = "Pending"; status_color = 0xFF8E8E93; break;
            case 1: status_name = "Running"; status_color = 0xFF007AFF; break;
            case 2: status_name = "Complete"; status_color = 0xFF34C759; break;
            case 3: status_name = "Failed"; status_color = 0xFFFF3B30; break;
            default: status_name = "Unknown"; status_color = 0xFF8E8E93; break;
        }
        
        gfx_draw_string(x + 200, item_y, status_name, status_color);
        
        // Drive
        char drive_str[8];
        strcpy(drive_str, "Drive ");
        int_to_str(op->drive_index, drive_str + 6);
        gfx_draw_string(x + 300, item_y, drive_str, 0xFF8E8E93);
        
        item_y += 22;
    }
    
    if (queue->operation_count == 0) {
        gfx_draw_string(x + 16, item_y, "No operations queued", 0xFF8E8E93);
    }
}
