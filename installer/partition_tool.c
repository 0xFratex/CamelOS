// installer/partition_tool.c - Partition Management Tool Implementation

#include "partition_tool.h"
#include "../hal/drivers/ata.h"
#include "../core/string.h"
#include "../fs/pfs32.h"
#include "../hal/video/gfx_hal.h"

// Global state
static PartitionTool g_ptool;

// --- Utilities ---

const char* partition_type_name(uint8_t type) {
    switch (type) {
        case 0x00: return "Free Space";
        case 0x01: return "FAT12";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0F: return "Extended";
        case 0x83: return "EXT4";
        case 0x7F: return "PFS32 (Camel OS)";
        case 0x82: return "Linux Swap";
        case 0xA5: return "FreeBSD";
        case 0xA6: return "OpenBSD";
        default:   return "Unknown";
    }
}

void partition_format_size(uint32_t sectors, char* out) {
    if (sectors == 0) {
        strcpy(out, "0 MB");
        return;
    }
    
    uint32_t mb = sectors / 2048;  // 512 bytes per sector
    
    if (mb >= 1024) {
        uint32_t gb = mb / 1024;
        uint32_t gb_dec = (mb % 1024) * 10 / 1024;
        int_to_str(gb, out);
        strcat(out, ".");
        char dec[4];
        int_to_str(gb_dec, dec);
        strcat(out, dec);
        strcat(out, " GB");
    } else {
        int_to_str(mb, out);
        strcat(out, " MB");
    }
}

// --- Initialization ---

void partition_tool_init(int drive_index) {
    memset(&g_ptool, 0, sizeof(g_ptool));
    g_ptool.drive_index = drive_index;
    g_ptool.selected_partition = -1;
    g_ptool.hover_partition = -1;
    
    partition_tool_read_mbr(drive_index);
}

void partition_tool_refresh(void) {
    partition_tool_read_mbr(g_ptool.drive_index);
}

// --- MBR Operations ---

int partition_tool_read_mbr(int drive_index) {
    if (!ide_devices[drive_index].present) {
        return 0;
    }
    
    g_ptool.drive_index = drive_index;
    g_ptool.total_sectors = ide_devices[drive_index].sectors;
    
    // Read MBR
    uint8_t buffer[512];
    ata_read_sector(drive_index, 0, buffer);
    
    // Check signature
    if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
        g_ptool.has_mbr = 1;
        memcpy(&g_ptool.mbr, buffer, sizeof(MBR));
    } else {
        g_ptool.has_mbr = 0;
        memset(&g_ptool.mbr, 0, sizeof(MBR));
    }
    
    // Parse partitions
    g_ptool.partition_count = 0;
    g_ptool.used_sectors = 0;
    g_ptool.free_sectors = g_ptool.total_sectors;
    
    for (int i = 0; i < 4; i++) {
        PartitionEntry* entry = &g_ptool.mbr.partitions[i];
        PartitionInfo* info = &g_ptool.partitions[i];
        
        info->index = i;
        info->type = entry->type;
        info->lba_start = entry->lba_start;
        info->lba_length = entry->lba_length;
        info->is_bootable = (entry->status & 0x80) != 0;
        info->is_camel_os = (entry->type == 0x7F);
        info->is_empty = (entry->type == 0);
        
        strcpy(info->type_name, partition_type_name(entry->type));
        
        // Calculate size
        info->size_mb = entry->lba_length / 2048;
        info->size_gb = info->size_mb / 1024;
        
        if (entry->type != 0) {
            g_ptool.partition_count++;
            g_ptool.used_sectors += entry->lba_length;
        }
    }
    
    g_ptool.free_sectors = g_ptool.total_sectors - g_ptool.used_sectors;
    
    return g_ptool.has_mbr;
}

int partition_tool_write_mbr(int drive_index) {
    // Ensure signature
    g_ptool.mbr.signature = 0xAA55;
    
    // Write MBR
    ata_write_sector(drive_index, 0, (uint8_t*)&g_ptool.mbr);
    
    // Refresh
    return partition_tool_read_mbr(drive_index);
}

// --- Partition Operations ---

// Helper: LBA to CHS conversion (for legacy compatibility)
void lba_to_chs(uint32_t lba, uint8_t* chs) {
    // Simplified CHS calculation for modern drives
    // We use a safe default that works with LBA
    uint16_t cylinder = lba / (63 * 255);
    uint8_t head = (lba / 63) % 255;
    uint8_t sector = (lba % 63) + 1;
    
    chs[0] = head;
    chs[1] = ((cylinder >> 2) & 0xC0) | (sector & 0x3F);
    chs[2] = cylinder & 0xFF;
}

int partition_tool_create(int index, uint8_t type, uint32_t start, uint32_t size) {
    if (index < 0 || index >= 4) return 0;
    
    // Check if partition slot is free
    if (g_ptool.mbr.partitions[index].type != 0) {
        return 0;  // Slot already in use
    }
    
    // Create partition entry
    PartitionEntry* entry = &g_ptool.mbr.partitions[index];
    
    entry->status = (index == 0) ? 0x80 : 0x00;  // Make first partition bootable
    entry->type = type;
    entry->lba_start = start;
    entry->lba_length = size;
    
    // Set CHS values (for compatibility)
    lba_to_chs(start, entry->chs_start);
    lba_to_chs(start + size - 1, entry->chs_end);
    
    return partition_tool_write_mbr(g_ptool.drive_index);
}

int partition_tool_delete(int index) {
    if (index < 0 || index >= 4) return 0;
    
    // Clear partition entry
    memset(&g_ptool.mbr.partitions[index], 0, sizeof(PartitionEntry));
    
    return partition_tool_write_mbr(g_ptool.drive_index);
}

int partition_tool_format(int index, uint8_t fs_type) {
    if (index < 0 || index >= 4) return 0;
    
    PartitionEntry* entry = &g_ptool.mbr.partitions[index];
    if (entry->type == 0) return 0;  // No partition
    
    int drive = g_ptool.drive_index;
    
    switch (fs_type) {
        case PART_TYPE_PFS32:
            // Format as Camel OS native filesystem
            pfs32_init(entry->lba_start, entry->lba_length);
            pfs32_format("Camel OS", entry->lba_length);
            entry->type = PART_TYPE_PFS32;
            break;
            
        case PART_TYPE_FAT32:
            // Create FAT32 boot sector (simplified)
            {
                uint8_t fat_boot[512];
                memset(fat_boot, 0, 512);
                
                // Jump instruction
                fat_boot[0] = 0xEB;
                fat_boot[1] = 0x58;
                fat_boot[2] = 0x90;
                
                // OEM name
                memcpy(fat_boot + 3, "CAMELOS ", 8);
                
                // Bytes per sector
                *(uint16_t*)(fat_boot + 11) = 512;
                
                // Sectors per cluster
                fat_boot[13] = 8;
                
                // Reserved sectors
                *(uint16_t*)(fat_boot + 14) = 32;
                
                // Number of FATs
                fat_boot[16] = 2;
                
                // Root entries (0 for FAT32)
                *(uint16_t*)(fat_boot + 17) = 0;
                
                // Total sectors (small)
                *(uint16_t*)(fat_boot + 19) = 0;
                
                // Media descriptor
                fat_boot[21] = 0xF8;
                
                // Sectors per FAT (FAT32)
                *(uint32_t*)(fat_boot + 36) = entry->lba_length / 256;
                
                // Root cluster
                *(uint32_t*)(fat_boot + 44) = 2;
                
                // Volume label
                memcpy(fat_boot + 71, "CAMEL OS   ", 11);
                
                // Filesystem type
                memcpy(fat_boot + 82, "FAT32   ", 8);
                
                // Boot signature
                fat_boot[510] = 0x55;
                fat_boot[511] = 0xAA;
                
                ata_write_sector(drive, entry->lba_start, fat_boot);
                entry->type = PART_TYPE_FAT32;
            }
            break;
            
        case PART_TYPE_NTFS:
            // Create NTFS boot sector (simplified)
            {
                uint8_t ntfs_boot[512];
                memset(ntfs_boot, 0, 512);
                
                ntfs_boot[0] = 0xEB;
                ntfs_boot[1] = 0x52;
                ntfs_boot[2] = 0x90;
                
                memcpy(ntfs_boot + 3, "NTFS    ", 8);
                
                // Bytes per sector
                *(uint16_t*)(ntfs_boot + 11) = 512;
                
                // Sectors per cluster
                ntfs_boot[13] = 8;
                
                // Total sectors
                *(uint64_t*)(ntfs_boot + 40) = entry->lba_length;
                
                // MFT location (simplified)
                *(uint64_t*)(ntfs_boot + 48) = 4;
                
                // Boot signature
                ntfs_boot[510] = 0x55;
                ntfs_boot[511] = 0xAA;
                
                ata_write_sector(drive, entry->lba_start, ntfs_boot);
                entry->type = PART_TYPE_NTFS;
            }
            break;
            
        default:
            return 0;  // Unsupported filesystem
    }
    
    // Update partition type and write MBR
    return partition_tool_write_mbr(drive);
}

int partition_tool_set_active(int index) {
    if (index < 0 || index >= 4) return 0;
    
    // Clear active flag on all partitions
    for (int i = 0; i < 4; i++) {
        g_ptool.mbr.partitions[i].status &= ~0x80;
    }
    
    // Set active on selected partition
    g_ptool.mbr.partitions[index].status |= 0x80;
    
    return partition_tool_write_mbr(g_ptool.drive_index);
}

// --- Queries ---

PartitionInfo* partition_tool_get_info(int index) {
    if (index < 0 || index >= 4) return NULL;
    return &g_ptool.partitions[index];
}

int partition_tool_get_free_space(uint32_t* start, uint32_t* length) {
    // Find largest contiguous free space
    uint32_t free_start = 0;
    uint32_t free_len = 0;
    
    // Start after first track (standard alignment)
    uint32_t current = 2048;
    
    // Sort partitions by start position and find gaps
    // Simplified: just check for gaps
    for (int i = 0; i < 4; i++) {
        PartitionInfo* info = &g_ptool.partitions[i];
        if (info->type != 0) {
            if (info->lba_start > current) {
                uint32_t gap = info->lba_start - current;
                if (gap > free_len) {
                    free_start = current;
                    free_len = gap;
                }
            }
            current = info->lba_start + info->lba_length;
        }
    }
    
    // Check space after last partition
    if (g_ptool.total_sectors - current > free_len) {
        free_start = current;
        free_len = g_ptool.total_sectors - current;
    }
    
    if (start) *start = free_start;
    if (length) *length = free_len;
    
    return free_len > 0;
}

int partition_tool_find_camel_partition(void) {
    for (int i = 0; i < 4; i++) {
        if (g_ptool.partitions[i].type == PART_TYPE_CAMEL) {
            return i;
        }
    }
    return -1;
}

// --- Rendering ---

void partition_tool_render_bar(int x, int y, int w, int h, int selected) {
    // Background
    gfx_fill_rounded_rect(x, y, w, h, 0xFFE5E5EA, 8);
    gfx_draw_rect(x, y, w, h, 0xFFC6C6C8);
    
    if (!g_ptool.has_mbr) {
        // Uninitialized disk
        gfx_draw_string(x + w/2 - 50, y + h/2 - 8, "Uninitialized", 0xFF8E8E93);
        return;
    }
    
    // Draw partitions
    int px = x + 4;
    int pw = w - 8;
    
    for (int i = 0; i < 4; i++) {
        PartitionInfo* info = &g_ptool.partitions[i];
        if (info->type == 0) continue;
        
        // Calculate partition width
        int part_w = (int)((uint64_t)info->lba_length * pw / g_ptool.total_sectors);
        if (part_w < 4) part_w = 4;
        
        // Partition color based on type
        uint32_t color;
        switch (info->type) {
            case PART_TYPE_PFS32:
                color = 0xFF007AFF;  // Blue for Camel OS
                break;
            case PART_TYPE_NTFS:
                color = 0xFF5856D6;  // Purple for NTFS
                break;
            case PART_TYPE_FAT32:
                color = 0xFF34C759;  // Green for FAT32
                break;
            case PART_TYPE_EXT4:
                color = 0xFFFF9500;  // Orange for EXT4
                break;
            default:
                color = 0xFF8E8E93;  // Grey for others
        }
        
        // Highlight selected
        if (i == selected) {
            gfx_fill_rounded_rect(px - 2, y - 2, part_w + 4, h + 4, 0xFF3D89D6, 8);
        }
        
        // Draw partition
        gfx_fill_rounded_rect(px, y + 2, part_w, h - 4, color, 6);
        
        // Active indicator
        if (info->is_bootable) {
            gfx_fill_rect(px + 2, y + 2, 4, 4, 0xFFFFFFFF);
        }
        
        px += part_w;
    }
}

void partition_tool_render_details(int x, int y, int index) {
    if (index < 0 || index >= 4) return;
    
    PartitionInfo* info = &g_ptool.partitions[index];
    
    // Background
    gfx_fill_rounded_rect(x, y, 300, 120, 0xFFF2F2F7, 8);
    gfx_draw_rect(x, y, 300, 120, 0xFFC6C6C8);
    
    // Partition number
    char title[32];
    strcpy(title, "Partition ");
    char num[4];
    int_to_str(index + 1, num);
    strcat(title, num);
    gfx_draw_string(x + 16, y + 12, title, 0xFF1C1C1E);
    
    if (info->type == 0) {
        gfx_draw_string(x + 16, y + 40, "Empty", 0xFF8E8E93);
        gfx_draw_string(x + 16, y + 60, "Click to create new", 0xFF8E8E93);
        return;
    }
    
    // Type
    gfx_draw_string(x + 16, y + 40, "Type:", 0xFF8E8E93);
    gfx_draw_string(x + 70, y + 40, info->type_name, 0xFF1C1C1E);
    
    // Size
    char size_str[32];
    partition_format_size(info->lba_length, size_str);
    gfx_draw_string(x + 16, y + 60, "Size:", 0xFF8E8E93);
    gfx_draw_string(x + 70, y + 60, size_str, 0xFF1C1C1E);
    
    // Status
    gfx_draw_string(x + 16, y + 80, "Status:", 0xFF8E8E93);
    if (info->is_bootable) {
        gfx_draw_string(x + 70, y + 80, "Active (Bootable)", 0xFF007AFF);
    } else {
        gfx_draw_string(x + 70, y + 80, "Inactive", 0xFF8E8E93);
    }
    
    // Start sector
    char start_str[32];
    int_to_str(info->lba_start, start_str);
    gfx_draw_string(x + 16, y + 100, "Start:", 0xFF8E8E93);
    gfx_draw_string(x + 70, y + 100, start_str, 0xFF1C1C1E);
}

// --- Auto-partition for Camel OS ---

int partition_tool_auto_partition(int drive_index) {
    if (!ide_devices[drive_index].present) return 0;
    
    // Initialize partition tool
    partition_tool_init(drive_index);
    
    // Create fresh MBR with single Camel OS partition
    memset(&g_ptool.mbr, 0, sizeof(MBR));
    
    uint32_t total = ide_devices[drive_index].sectors;
    uint32_t start = 2048;  // 1MB alignment
    uint32_t size = total - start;
    
    // Create Camel OS partition
    PartitionEntry* entry = &g_ptool.mbr.partitions[0];
    entry->status = 0x80;  // Bootable
    entry->type = PART_TYPE_PFS32;
    entry->lba_start = start;
    entry->lba_length = size;
    lba_to_chs(start, entry->chs_start);
    lba_to_chs(start + size - 1, entry->chs_end);
    
    // Write MBR
    partition_tool_write_mbr(drive_index);
    
    // Format as PFS32
    pfs32_init(start, size);
    pfs32_format("Camel OS", size);
    
    return 1;
}
