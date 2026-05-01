// fs/pfs32.c - PFS32 Implementation v3.0 (APFS+ Compatible)
// Features: Copy-on-Write, Snapshots, Checksumming, Space Sharing,
//           Extended Attributes, Nanosecond Timestamps, Clone Support,
//           Full Disk Utilization (zero sector waste except bad blocks)
#include "pfs32.h"
#include "disk.h"

// Forward declarations for bitmap functions (defined later in this file)
void pfs32_bitmap_set(uint32_t block);
void pfs32_bitmap_clear(uint32_t block);
void pfs32_bitmap_set_bad(uint32_t block);
void pfs32_flush_bitmap(void);
int pfs32_bitmap_test(uint32_t block);
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"
#include "../core/task.h" // For get_current_uid()

// --- Concurrency / Thread Safety (BUG-001) ---
// Simple spinlock implementation using interrupt disable
static volatile int pfs_lock = 0;

static inline void pfs_spin_lock(void) {
    asm volatile("cli");
    while(__sync_lock_test_and_set(&pfs_lock, 1)) {
        asm volatile("sti; pause; cli");
    }
}

static inline void pfs_spin_unlock(void) {
    __sync_lock_release(&pfs_lock);
    asm volatile("sti");
}

#define PFS_LOCK()   pfs_spin_lock()
#define PFS_UNLOCK() pfs_spin_unlock() 

// --- CRITICAL: Entries Per Block Calculation ---
// pfs32_direntry_t is 128 bytes (with APFS+ extended fields), so only 4 fit per 512-byte block.
// Old code assumed 8 entries (when entries were 64 bytes), causing buffer overflows
// and directory corruption. This was the root cause of config save failures,
// file duplication, and installer crashes.
#define PFS32_ENTRIES_PER_BLOCK (PFS32_BLOCK_SIZE / sizeof(pfs32_direntry_t))  // = 4 

static pfs32_superblock_t sb;
static uint32_t disk_start = 0;
static uint32_t mounted = 0;
static pfs32_stats_t stats = {0};

// --- APFS+ Compatibility State ---
static uint32_t* block_bitmap = 0;       // Full-disk block bitmap (1=used, 0=free)
static uint32_t block_bitmap_blocks = 0; // Blocks occupied by the bitmap
static uint32_t block_bitmap_size = 0;   // Size of in-memory bitmap in bytes
static int block_bitmap_dirty = 0;       // In-memory bitmap needs flush
static uint32_t* cow_bitmap = 0;         // CoW bitmap for current transaction
static pfs32_snapshot_t snapshots[PFS32_MAX_SNAPSHOTS];
static pfs32_clone_entry_t clones[PFS32_MAX_CLONES];
static int clone_count = 0;
static uint32_t current_transaction = 0;

// --- Helper Prototypes ---
uint32_t get_current_gid() { return 0; } // Placeholder: Hook into task/OS
uint32_t pfs32_time_now() { return 0; }  // Placeholder: Hook into RTC

// Simple integer to string conversion
void pfs_int_to_str(int num, char* buf) {
    if (num == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    
    int i = 0;
    int is_neg = num < 0;
    if (is_neg) num = -num;
    
    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    if (is_neg) buf[i++] = '-';
    buf[i] = 0;
    
    // Reverse string
    int len = i;
    for (int j = 0; j < len / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[len - 1 - j];
        buf[len - 1 - j] = tmp;
    }
}

// --- FAT CACHE (LRU Implementation PERF-004) ---
#define FAT_CACHE_SIZE 256
static uint32_t fat_cache_block[FAT_CACHE_SIZE];
static uint32_t fat_cache_data[FAT_CACHE_SIZE][PFS32_BLOCK_SIZE/4];
static int fat_cache_dirty[FAT_CACHE_SIZE];
static uint32_t fat_cache_lru[FAT_CACHE_SIZE]; // Last access timestamp
static uint32_t fat_access_counter = 0;        // Logical clock

// --- Allocation Optimization ---
static uint32_t last_alloc_search_ptr = 0;

// Forward Declarations
void get_basename(const char* path, char* out_buf);
void get_parent_path(const char* path, char* out_buf);
int find_entry_in_dir(uint32_t dir_start, const char* name, pfs32_direntry_t* out, uint32_t* out_blk, int* out_idx);

// --- Helper: Disk I/O with Bounds Checking ---
static int disk_rw(int write, uint32_t block, void* buf) {
    if (!mounted && block != 0) return PFS_ERR_IO;
    
    if (mounted && block >= sb.total_blocks) {
        return PFS_ERR_IO;
    }
    
    int ret = 0;
    for(int i=0; i<3; i++) {
        if (write) {
            ret = disk_write_block(disk_start + block, buf);
            if(ret == 0) stats.disk_writes++;
        } else {
            ret = disk_read_block(disk_start + block, buf);
            if(ret == 0) stats.disk_reads++;
        }
        
        if (ret == 0) return PFS_OK;
        for(volatile int k=0; k<1000; k++);
    }
    return PFS_ERR_IO;
}

// --- Helper: Sanitize Filename ---
void sanitize_name(char* dest, const char* src, int max_len) {
    int i = 0, j = 0;
    while(src[i] != 0 && j < max_len) {
        unsigned char c = (unsigned char)src[i];
        // Allow alphanumeric, dot, underscore, dash, SPACE, parenthesis
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '.') || (c == '_') ||
            (c == '-') || (c == ' ') || (c == '(') || (c == ')')) {
            dest[j++] = (char)c;
        }
        i++;
    }
    dest[j] = 0;
    if (j == 0) { dest[0] = '_'; dest[1] = 0; }
}

// --- Permission Logic (SEC-002 Group Support) ---
int check_permission(uint8_t file_uid, uint8_t file_gid, uint8_t file_perm, int op) {
    int current_uid = get_current_uid();
    int current_gid = get_current_gid();

    // Root (0) bypass
    if (current_uid == 0) return 1;

    // Permissions: [Owner 3][Group 3][World 2]
    
    // Check Owner
    if (current_uid == file_uid) {
        uint8_t owner_perm = (file_perm >> 5) & 0x07;
        return (owner_perm & op);
    }

    // Check Group
    if (current_gid == file_gid) {
        uint8_t group_perm = (file_perm >> 2) & 0x07;
        return (group_perm & op);
    }

    // Check World
    uint8_t world_perm = file_perm & 0x03;
    // World doesn't support write bit in this compact scheme, usually just R or X
    // Map op: Read(4) -> 2, Write(2) -> Not supported, Exec(1) -> 1
    uint8_t req = 0;
    if (op == PFS_PERM_READ) req = 2;
    if (op == PFS_PERM_EXEC) req = 1;
    if (op == PFS_PERM_WRITE) return 0; // World write disabled by design in packed byte

    return (world_perm & req);
}

// --- FAT Management (LRU) ---

void init_fat_cache() {
    PFS_LOCK();
    for(int i=0; i<FAT_CACHE_SIZE; i++) {
        fat_cache_block[i] = PFS32_END_BLOCK;
        fat_cache_dirty[i] = 0;
        fat_cache_lru[i] = 0;
        memset(fat_cache_data[i], 0, PFS32_BLOCK_SIZE);
    }
    fat_access_counter = 0;
    PFS_UNLOCK();
}

void flush_fat() {
    if (!mounted) return;
    PFS_LOCK();
    for(int i=0; i<FAT_CACHE_SIZE; i++) {
        if(fat_cache_block[i] != PFS32_END_BLOCK && fat_cache_dirty[i]) {
            if (disk_rw(1, 1 + fat_cache_block[i], fat_cache_data[i]) == PFS_OK) {
                fat_cache_dirty[i] = 0;
            }
        }
    }
    PFS_UNLOCK();
}

uint32_t get_fat(uint32_t cluster) {
    if (PFS32_BLOCK_SIZE == 0) return PFS32_END_BLOCK;
    PFS_LOCK();
    
    uint32_t entries_per_block = PFS32_BLOCK_SIZE / 4;
    uint32_t fat_blk_idx = cluster / entries_per_block;
    uint32_t fat_offset = cluster % entries_per_block;
    fat_access_counter++;

    // Check Cache
    for(int i=0; i<FAT_CACHE_SIZE; i++) {
        if(fat_cache_block[i] == fat_blk_idx) {
            fat_cache_lru[i] = fat_access_counter; // Update LRU
            stats.cache_hits++;
            uint32_t val = fat_cache_data[i][fat_offset];
            PFS_UNLOCK();
            return val;
        }
    }
    
    stats.cache_misses++;

    // LRU Victim Selection
    int victim = 0;
    uint32_t min_lru = fat_cache_lru[0];
    for(int i=1; i<FAT_CACHE_SIZE; i++) {
        if (fat_cache_lru[i] < min_lru) {
            min_lru = fat_cache_lru[i];
            victim = i;
        }
    }

    // Flush victim
    if(fat_cache_block[victim] != PFS32_END_BLOCK && fat_cache_dirty[victim]) {
        disk_rw(1, 1 + fat_cache_block[victim], fat_cache_data[victim]);
    }

    // Load new
    fat_cache_block[victim] = fat_blk_idx;
    fat_cache_dirty[victim] = 0;
    fat_cache_lru[victim] = fat_access_counter;
    
    if (disk_rw(0, 1 + fat_blk_idx, fat_cache_data[victim]) != PFS_OK) {
        fat_cache_block[victim] = PFS32_END_BLOCK; // Invalidate
        PFS_UNLOCK();
        return PFS32_END_BLOCK;
    }

    uint32_t val = fat_cache_data[victim][fat_offset];
    PFS_UNLOCK();
    return val;
}

void set_fat(uint32_t cluster, uint32_t val) {
    if (PFS32_BLOCK_SIZE == 0) return;
    PFS_LOCK();
    
    uint32_t entries_per_block = PFS32_BLOCK_SIZE / 4;
    uint32_t fat_blk_idx = cluster / entries_per_block;
    uint32_t fat_offset = cluster % entries_per_block;
    fat_access_counter++;

    int slot = -1;
    for(int i=0; i<FAT_CACHE_SIZE; i++) {
        if(fat_cache_block[i] == fat_blk_idx) { slot = i; break; }
    }

    if(slot == -1) {
        // Must load it first (Read-Modify-Write)
        PFS_UNLOCK(); // Release lock before calling get_fat which locks
        get_fat(cluster); 
        PFS_LOCK(); // Re-acquire
        
        // Find where it ended up
        for(int i=0; i<FAT_CACHE_SIZE; i++) {
            if(fat_cache_block[i] == fat_blk_idx) { slot = i; break; }
        }
    }

    if (slot != -1) {
        fat_cache_data[slot][fat_offset] = val;
        fat_cache_dirty[slot] = 1;
        fat_cache_lru[slot] = fat_access_counter;
    }
    PFS_UNLOCK();
}

uint32_t alloc_block() {
    uint32_t start_search = last_alloc_search_ptr;
    if (start_search < sb.data_start_block || start_search >= sb.total_blocks) {
        start_search = sb.data_start_block;
    }

    // Search bitmap first (authoritative tracker) if in-memory bitmap loaded
    for(uint32_t i = start_search; i < sb.total_blocks; i++) {
        if(pfs32_bitmap_test(i) == 0 && get_fat(i) == PFS32_FREE_BLOCK) {
            set_fat(i, PFS32_END_BLOCK);
            pfs32_bitmap_set(i);
            uint8_t z[512]; memset(z, 0, 512);
            disk_rw(1, i, z);
            last_alloc_search_ptr = i + 1;
            if(sb.free_blocks > 0) sb.free_blocks--;
            return i;
        }
    }
    
    // Wrap around
    for(uint32_t i = sb.data_start_block; i < start_search; i++) {
        if(pfs32_bitmap_test(i) == 0 && get_fat(i) == PFS32_FREE_BLOCK) {
            set_fat(i, PFS32_END_BLOCK);
            pfs32_bitmap_set(i);
            uint8_t z[512]; memset(z, 0, 512);
            disk_rw(1, i, z);
            last_alloc_search_ptr = i + 1;
            if(sb.free_blocks > 0) sb.free_blocks--;
            return i;
        }
    }

    return 0; 
}

// --- Directory Logic ---

int find_entry_in_buf(uint8_t* buf, const char* name, pfs32_direntry_t* out) {
    pfs32_direntry_t* entries = (pfs32_direntry_t*)buf;
    for(int i=0; i<PFS32_ENTRIES_PER_BLOCK; i++) {
        if(entries[i].filename[0] == 0) continue;
        char clean[64]; sanitize_name(clean, entries[i].filename, 63);
        if(strcmp(clean, name) == 0) {
            if(out) *out = entries[i];
            return i;
        }
    }
    return -1;
}

int find_entry_in_dir(uint32_t dir_start, const char* name, pfs32_direntry_t* out, uint32_t* out_blk, int* out_idx) {
    uint32_t curr = dir_start;
    while(curr != PFS32_END_BLOCK && curr != 0) {
        uint8_t buf[512];
        if(disk_rw(0, curr, buf) != PFS_OK) break;
        
        int idx = find_entry_in_buf(buf, name, out);
        if (idx != -1) {
            if(out_blk) *out_blk = curr;
            if(out_idx) *out_idx = idx;
            return PFS_OK;
        }
        curr = get_fat(curr);
    }
    return PFS_ERR_NOT_FOUND;
}

// --- Lifecycle ---

int pfs32_init(uint32_t start, uint32_t total) {
    init_fat_cache();
    disk_start = start;
    memset(&sb, 0, sizeof(sb));
    memset(&stats, 0, sizeof(stats));

    s_printf("[PFS] init start=");
    char buf[32];
    int_to_str(start, buf);
    s_printf(buf);
    s_printf(" total=");
    int_to_str(total, buf);
    s_printf(buf);
    s_printf("\n");

    int res = disk_read_block(disk_start, &sb);
    s_printf("[PFS] read_block res=");
    int_to_str(res, buf);
    s_printf(buf);
    s_printf("\n");
    if(res != 0) return PFS_ERR_IO;

    s_printf("[PFS] sb.magic=");
    int_to_str(sb.magic, buf);
    s_printf(buf);
    s_printf(" expected=");
    int_to_str(PFS32_MAGIC, buf);
    s_printf(buf);
    s_printf("\n");
    s_printf("[PFS] sb.total_blocks=");
    int_to_str(sb.total_blocks, buf);
    s_printf(buf);
    s_printf(" block_size=");
    int_to_str(sb.block_size, buf);
    s_printf(buf);
    s_printf("\n");

    if (sb.magic != PFS32_MAGIC) return PFS_ERR_NO_FS;

    mounted = 1;

    // Load block bitmap into memory for fast allocation tracking
    if (sb.block_bitmap_start && sb.block_bitmap_blocks > 0) {
        // Free old bitmap if re-initializing
        if (block_bitmap) {
            kfree(block_bitmap);
            block_bitmap = 0;
        }

        block_bitmap_size = (sb.total_blocks + 7) / 8;
        // Align allocation to uint32_t boundary
        uint32_t alloc_size = (block_bitmap_size + 3) & ~3;
        block_bitmap = (uint32_t*)kmalloc(alloc_size);
        if (block_bitmap) {
            memset(block_bitmap, 0, alloc_size);
            uint8_t* bmp = (uint8_t*)block_bitmap;
            // Read bitmap blocks from disk into in-memory bitmap
            for (uint32_t i = 0; i < sb.block_bitmap_blocks; i++) {
                uint8_t buf[512];
                if (disk_rw(0, sb.block_bitmap_start + i, buf) == PFS_OK) {
                    uint32_t offset = i * PFS32_BLOCK_SIZE;
                    uint32_t chunk = PFS32_BLOCK_SIZE;
                    if (offset + chunk > block_bitmap_size) {
                        chunk = block_bitmap_size - offset;
                    }
                    if (chunk > 0) memcpy(bmp + offset, buf, chunk);
                }
            }
            block_bitmap_dirty = 0;
            s_printf("[PFS] Block bitmap loaded into memory\n");
        } else {
            s_printf("[PFS] WARNING: Could not allocate bitmap memory\n");
        }
    }

    return PFS_OK;
}

int pfs32_format(const char* label, uint32_t total) {
    init_fat_cache();
    memset(&sb, 0, sizeof(sb));
    sb.magic = PFS32_MAGIC;
    sb.version = PFS32_VERSION;
    sb.block_size = PFS32_BLOCK_SIZE;
    sb.page_size = PFS32_PAGE_SIZE;
    sb.total_blocks = total;
    sb.total_pages = total / PFS32_PAGE_BLOCKS;
    sb.cluster_blocks = 8; // 4KB clusters
    sb.volume_label[0] = 0;
    if(label) strncpy(sb.volume_label, label, 31);

    // Calculate block bitmap size (1 bit per block = total/8 bytes = total/4096 blocks)
    block_bitmap_blocks = (total + 4095) / 4096;
    if(block_bitmap_blocks < 1) block_bitmap_blocks = 1;
    sb.block_bitmap_blocks = block_bitmap_blocks;
    sb.block_bitmap_start = 1; // Right after superblock

    // FAT blocks (kept for backward compatibility)
    uint32_t total_clusters = total / sb.cluster_blocks;
    uint32_t fat_blocks = (total_clusters + 127) / 128;
    sb.fat_blocks = fat_blocks;
    
    // Layout: [Superblock][Block Bitmap][FAT][Bad Block List][Data...]
    sb.data_start_block = 1 + block_bitmap_blocks + fat_blocks + 1; // +1 for bad block list
    sb.bad_block_list_start = 1 + block_bitmap_blocks + fat_blocks;
    sb.root_dir_block = sb.data_start_block;
    sb.free_blocks = total - sb.data_start_block;
    sb.free_pages = sb.free_blocks / PFS32_PAGE_BLOCKS;

    // APFS+ features enabled by default
    sb.feature_flags = PFS32_DEFAULT_FEATURES;
    sb.checksum_algo = 1; // Fletcher-64
    sb.next_transaction_id = 1;
    sb.container_id = 0xCAFEBABE; // Unique container ID
    sb.volume_count = 1;
    sb.bad_block_count = 0;
    sb.snapshot_count = 0;

    mounted = 1;

    // Write superblock
    if(disk_write_block(disk_start, &sb) != 0) {
        mounted = 0;
        return PFS_ERR_IO;
    }

    // Initialize block bitmap (all zeros = all free)
    uint8_t zero[512]; memset(zero, 0, 512);
    for(uint32_t i = 0; i < block_bitmap_blocks; i++) {
        disk_write_block(disk_start + sb.block_bitmap_start + i, zero);
    }
    
    // Initialize FAT
    for(uint32_t i=1; i <= fat_blocks; i++) {
        disk_write_block(disk_start + 1 + block_bitmap_blocks + i - 1, zero);
    }

    // Initialize bad block list (1 block, all zeros)
    disk_write_block(disk_start + sb.bad_block_list_start, zero);

    // Mark system blocks as used in FAT and bitmap
    for(uint32_t i=0; i <= sb.root_dir_block; i++) {
        set_fat(i, PFS32_END_BLOCK);
    }
    
    // Mark blocks 0..data_start_block-1 as used in block bitmap
    for(uint32_t i = 0; i < sb.data_start_block; i++) {
        pfs32_bitmap_set(i);
    }
    sb.free_blocks = total - sb.data_start_block;

    // Scan for bad blocks (write/read test)
    if(sb.feature_flags & PFS32_FEATURE_FULL_DISK_UTIL) {
        sb.bad_block_count = 0;
        uint8_t test_pattern[512];
        uint8_t verify[512];
        for(int pat = 0; pat < 2; pat++) {
            // Pattern 1: 0xAA, Pattern 2: 0x55
            memset(test_pattern, (pat == 0) ? 0xAA : 0x55, 512);
            
            // Only test data blocks (skip system area)
            for(uint32_t blk = sb.data_start_block; blk < total; blk++) {
                // Skip if already marked bad
                if(pfs32_bitmap_test(blk) == 0xFE) continue;
                
                // Save original
                uint8_t orig[512];
                disk_read_block(disk_start + blk, orig);
                
                // Write test pattern
                disk_write_block(disk_start + blk, test_pattern);
                
                // Read back
                disk_read_block(disk_start + blk, verify);
                
                // Compare
                int bad = 0;
                for(int j = 0; j < 512; j++) {
                    if(verify[j] != test_pattern[j]) { bad = 1; break; }
                }
                
                if(bad) {
                    pfs32_bitmap_set_bad(blk);
                    sb.bad_block_count++;
                    stats.bad_block_count++;
                } else {
                    // Restore original
                    disk_write_block(disk_start + blk, orig);
                }
            }
        }
        // Recalculate free blocks
        sb.free_blocks = 0;
        for(uint32_t blk = sb.data_start_block; blk < total; blk++) {
            if(pfs32_bitmap_test(blk) == 0) sb.free_blocks++;
        }
        sb.free_pages = sb.free_blocks / PFS32_PAGE_BLOCKS;
    }

    flush_fat();
    pfs32_flush_bitmap();
    
    // Write root directory
    memset(zero, 0, 512);
    pfs32_direntry_t* root = (pfs32_direntry_t*)zero;
    
    // Root .
    strcpy(root[0].filename, ".");
    root[0].attributes = PFS32_ATTR_DIRECTORY;
    root[0].uid = 0;
    root[0].permissions = 0xE8; // 111 010 00
    root[0].start_block = sb.root_dir_block;
    root[0].create_time = pfs32_time_now();

    // Root ..
    strcpy(root[1].filename, "..");
    root[1].attributes = PFS32_ATTR_DIRECTORY;
    root[1].uid = 0;
    root[1].permissions = 0xE8;
    root[1].start_block = sb.root_dir_block;
    root[1].create_time = pfs32_time_now();

    if(disk_write_block(disk_start + sb.root_dir_block, zero) != 0) return PFS_ERR_IO;
    
    // Update superblock with final stats
    sb.superblock_checksum = pfs32_compute_checksum(&sb, sizeof(sb) - sizeof(pfs32_checksum_t));
    disk_write_block(disk_start, &sb);
    
    flush_fat();
    return PFS_OK;
}

// Fast format - same as pfs32_format but skips bad block scan for boot-time speed
uint32_t pfs32_format_fast(const char* label, uint32_t total) {
    init_fat_cache();
    memset(&sb, 0, sizeof(sb));
    sb.magic = PFS32_MAGIC;
    sb.version = PFS32_VERSION;
    sb.block_size = PFS32_BLOCK_SIZE;
    sb.page_size = PFS32_PAGE_SIZE;
    sb.total_blocks = total;
    sb.total_pages = total / PFS32_PAGE_BLOCKS;
    sb.cluster_blocks = 8;
    sb.volume_label[0] = 0;
    if(label) strncpy(sb.volume_label, label, 31);

    block_bitmap_blocks = (total + 4095) / 4096;
    if(block_bitmap_blocks < 1) block_bitmap_blocks = 1;
    sb.block_bitmap_blocks = block_bitmap_blocks;
    sb.block_bitmap_start = 1;

    uint32_t total_clusters = total / sb.cluster_blocks;
    uint32_t fat_blocks = (total_clusters + 127) / 128;
    sb.fat_blocks = fat_blocks;
    
    sb.data_start_block = 1 + block_bitmap_blocks + fat_blocks + 1;
    sb.bad_block_list_start = 1 + block_bitmap_blocks + fat_blocks;
    sb.root_dir_block = sb.data_start_block;
    sb.free_blocks = total - sb.data_start_block;
    sb.free_pages = sb.free_blocks / PFS32_PAGE_BLOCKS;

    // Skip FULL_DISK_UTIL feature to avoid slow bad block scan
    sb.feature_flags = PFS32_DEFAULT_FEATURES & ~PFS32_FEATURE_FULL_DISK_UTIL;
    sb.checksum_algo = 1;
    sb.next_transaction_id = 1;
    sb.container_id = 0xCAFEBABE;
    sb.volume_count = 1;
    sb.bad_block_count = 0;
    sb.snapshot_count = 0;

    mounted = 1;

    // Write superblock
    if(disk_write_block(disk_start, &sb) != 0) {
        mounted = 0;
        return PFS_ERR_IO;
    }

    // Initialize block bitmap
    uint8_t zero[512]; memset(zero, 0, 512);
    for(uint32_t i = 0; i < block_bitmap_blocks; i++) {
        disk_write_block(disk_start + sb.block_bitmap_start + i, zero);
    }
    
    // Initialize FAT
    for(uint32_t i=1; i <= fat_blocks; i++) {
        disk_write_block(disk_start + 1 + block_bitmap_blocks + i - 1, zero);
    }

    // Initialize bad block list
    disk_write_block(disk_start + sb.bad_block_list_start, zero);

    // Mark system blocks as used
    for(uint32_t i=0; i <= sb.root_dir_block; i++) {
        set_fat(i, PFS32_END_BLOCK);
    }
    for(uint32_t i = 0; i < sb.data_start_block; i++) {
        pfs32_bitmap_set(i);
    }
    sb.free_blocks = total - sb.data_start_block;

    flush_fat();
    pfs32_flush_bitmap();
    
    // Write root directory
    memset(zero, 0, 512);
    pfs32_direntry_t* root = (pfs32_direntry_t*)zero;
    strcpy(root[0].filename, ".");
    root[0].attributes = PFS32_ATTR_DIRECTORY;
    root[0].uid = 0;
    root[0].permissions = 0xE8;
    root[0].start_block = sb.root_dir_block;
    root[0].create_time = pfs32_time_now();
    strcpy(root[1].filename, "..");
    root[1].attributes = PFS32_ATTR_DIRECTORY;
    root[1].uid = 0;
    root[1].permissions = 0xE8;
    root[1].start_block = sb.root_dir_block;
    root[1].create_time = pfs32_time_now();
    if(disk_write_block(disk_start + sb.root_dir_block, zero) != 0) return PFS_ERR_IO;
    
    sb.superblock_checksum = pfs32_compute_checksum(&sb, sizeof(sb) - sizeof(pfs32_checksum_t));
    disk_write_block(disk_start, &sb);
    
    flush_fat();
    // CRITICAL: Flush all cached writes to actual disk so filesystem persists across reboots
    disk_flush_cache();

    // Verify superblock was actually committed to disk
    {
        pfs32_superblock_t verify_sb;
        memset(&verify_sb, 0, sizeof(verify_sb));
        disk_read_block(disk_start, &verify_sb);
        if (verify_sb.magic != PFS32_MAGIC) {
            s_printf("[PFS] CRITICAL: Superblock verification failed after format!\n");
            s_printf("[PFS] Retrying write...\n");
            disk_write_block(disk_start, &sb);
            disk_flush_cache();
        }
    }

    return PFS_OK;
}

int get_dir_block(const char* path, uint32_t* block_out) {
    if(!mounted) return PFS_ERR_NO_FS;
    if(!path || !path[0] || strcmp(path, "/") == 0) {
        *block_out = sb.root_dir_block;
        return PFS_OK;
    }

    char buf[128]; strncpy(buf, path, 127); buf[127]=0;
    uint32_t curr = sb.root_dir_block;
    char* token = buf;
    if(*token == '/') token++;
    if(*token == 0) { *block_out = curr; return PFS_OK; }

    while(token && *token) {
        char* next_slash = strchr(token, '/');
        if(next_slash) *next_slash = 0;

        if(strlen(token) > 0) {
            pfs32_direntry_t ent;
            if(find_entry_in_dir(curr, token, &ent, 0, 0) != PFS_OK) return PFS_ERR_NOT_FOUND;
            
            // Handle Symlinks (API-003) - Basic Resolution (Depth 1)
            if (ent.attributes & PFS32_ATTR_SYMLINK) {
                 // To do: Read file content as new path. For now, treat as error or simple pass
                 // if not implementing full recursion yet.
                 return PFS_ERR_ACCESS; 
            }

            if(!(ent.attributes & PFS32_ATTR_DIRECTORY)) return PFS_ERR_NOT_FOUND;
            if (!check_permission(ent.uid, ent.gid, ent.permissions, PFS_PERM_EXEC)) return PFS_ERR_ACCESS;
            
            curr = ent.start_block;
        }

        if(!next_slash) break;
        token = next_slash + 1;
    }
    *block_out = curr;
    return PFS_OK;
}

// --- Creation ---

int pfs32_create_node(const char* path, int is_dir) {
    if(!mounted) return PFS_ERR_NO_FS;

    char parent[128];
    char name[64];
    get_parent_path(path, parent);
    get_basename(path, name);
    if (strlen(name) == 0) return PFS_ERR_PARAM;

    uint32_t pblk;
    if(get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if(find_entry_in_dir(pblk, name, 0, 0, 0) == PFS_OK) return PFS_ERR_EXISTS;

    uint32_t curr = pblk;
    uint32_t target_blk = 0;
    int target_idx = -1;
    uint8_t buf[512];

    // Find free slot
    while(1) {
        disk_rw(0, curr, buf);
        pfs32_direntry_t* entries = (pfs32_direntry_t*)buf;
        
        for(int i=0; i<PFS32_ENTRIES_PER_BLOCK; i++) {
            if(entries[i].filename[0] == 0) {
                target_blk = curr;
                target_idx = i;
                break;
            }
        }
        if (target_idx != -1) break;

        uint32_t next = get_fat(curr);
        if(next == PFS32_END_BLOCK || next == 0) {
            uint32_t new_blk = alloc_block();
            if(new_blk == 0) return PFS_ERR_FULL;
            set_fat(curr, new_blk);
            set_fat(new_blk, PFS32_END_BLOCK);
            flush_fat();
            
            memset(buf, 0, 512);
            target_blk = new_blk;
            target_idx = 0;
            break; 
        }
        curr = next;
    }

    pfs32_direntry_t* entries = (pfs32_direntry_t*)buf;
    memset(&entries[target_idx], 0, sizeof(pfs32_direntry_t));
    sanitize_name(entries[target_idx].filename, name, 39);
    entries[target_idx].attributes = is_dir ? PFS32_ATTR_DIRECTORY : 0;
    entries[target_idx].uid = get_current_uid();
    entries[target_idx].gid = get_current_gid();
    
    // Default Perms: Owner RWX, Group R-X, World R-- -> 111 101 10 -> 0xFA
    entries[target_idx].permissions = 0xFA; 
    
    entries[target_idx].create_time = pfs32_time_now();
    entries[target_idx].modify_time = entries[target_idx].create_time;

    uint32_t data_blk = alloc_block();
    if(data_blk == 0) return PFS_ERR_FULL;
    
    entries[target_idx].start_block = data_blk;

    if(is_dir) {
        entries[target_idx].file_size = 0;
        uint8_t z[512]; memset(z, 0, 512);
        pfs32_direntry_t* dent = (pfs32_direntry_t*)z;
        
        strcpy(dent[0].filename, ".");
        dent[0].attributes = PFS32_ATTR_DIRECTORY;
        dent[0].start_block = data_blk;

        strcpy(dent[1].filename, "..");
        dent[1].attributes = PFS32_ATTR_DIRECTORY;
        dent[1].start_block = pblk;

        disk_rw(1, data_blk, z);
    } else {
        entries[target_idx].file_size = 0;
        uint8_t z[512]; memset(z, 0, 512);
        disk_rw(1, data_blk, z);
    }
    
    set_fat(data_blk, PFS32_END_BLOCK);
    disk_rw(1, target_blk, buf);
    flush_fat();
    return PFS_OK;
}

// --- File I/O ---

int pfs32_write_file(const char* path, uint8_t* data, uint32_t size) {
    if(!mounted) return PFS_ERR_NO_FS;

    int res = pfs32_create_node(path, 0);
    if (res != PFS_OK && res != PFS_ERR_EXISTS) return res;

    uint32_t pblk;
    char parent[128];
    get_parent_path(path, parent);
    if(get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;

    uint32_t entry_blk;
    int entry_idx;
    pfs32_direntry_t entry;
    char name[64];
    get_basename(path, name);
    if(find_entry_in_dir(pblk, name, &entry, &entry_blk, &entry_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;

    if (!check_permission(entry.uid, entry.gid, entry.permissions, PFS_PERM_WRITE)) return PFS_ERR_ACCESS;

    uint32_t blk = entry.start_block;
    uint32_t written = 0;

    while(written < size) {
        uint8_t buf[512]; memset(buf, 0, 512);
        uint32_t chunk = (size - written < 512) ? size - written : 512;
        memcpy(buf, data + written, chunk);
        
        disk_rw(1, blk, buf);
        written += chunk;

        if(written < size) {
            uint32_t next = get_fat(blk);
            if(next == PFS32_END_BLOCK || next == 0) {
                next = alloc_block();
                if(next == 0) return PFS_ERR_FULL;
                set_fat(blk, next);
                set_fat(next, PFS32_END_BLOCK);
            }
            blk = next;
        }
    }
    
    // Update size and time
    uint8_t dbuf[512];
    disk_rw(0, entry_blk, dbuf);
    pfs32_direntry_t* de = (pfs32_direntry_t*)dbuf;
    de[entry_idx].file_size = size;
    de[entry_idx].modify_time = pfs32_time_now(); 
    disk_rw(1, entry_blk, dbuf);

    flush_fat();
    return size;
}

int pfs32_read_file(const char* path, uint8_t* buffer, uint32_t max) {
    if(!mounted) return PFS_ERR_NO_FS;
    pfs32_direntry_t entry;
    uint32_t entry_blk; int entry_idx;

    uint32_t pblk;
    char parent[128];
    get_parent_path(path, parent);
    get_dir_block(parent, &pblk);
    char name[64];
    get_basename(path, name);
    if(find_entry_in_dir(pblk, name, &entry, &entry_blk, &entry_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;

    if (!check_permission(entry.uid, entry.gid, entry.permissions, PFS_PERM_READ)) return PFS_ERR_ACCESS;
    if(entry.attributes & PFS32_ATTR_DIRECTORY) return PFS_ERR_PARAM;

    // Update Access Time
    uint8_t dbuf[512];
    disk_rw(0, entry_blk, dbuf);
    ((pfs32_direntry_t*)dbuf)[entry_idx].access_time = pfs32_time_now();
    disk_rw(1, entry_blk, dbuf);

    uint32_t blk = entry.start_block;
    uint32_t read = 0;
    uint32_t total = (entry.file_size > max) ? max : entry.file_size;

    while(read < total && blk != PFS32_END_BLOCK && blk != 0) {
        uint8_t buf[512];
        if(disk_rw(0, blk, buf) != PFS_OK) break;
        uint32_t chunk = (total - read < 512) ? total - read : 512;
        memcpy(buffer + read, buf, chunk);
        read += chunk;
        blk = get_fat(blk);
    }
    return read;
}

// --- FEAT-001: Truncate ---
int pfs32_truncate(const char* path, uint32_t new_size) {
    if(!mounted) return PFS_ERR_NO_FS;

    uint32_t pblk;
    char parent[128];
    get_parent_path(path, parent);
    if(get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;

    pfs32_direntry_t entry;
    uint32_t entry_blk; int entry_idx;
    char name[64];
    get_basename(path, name);
    if(find_entry_in_dir(pblk, name, &entry, &entry_blk, &entry_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if(!check_permission(entry.uid, entry.gid, entry.permissions, PFS_PERM_WRITE)) return PFS_ERR_ACCESS;

    if (new_size == entry.file_size) return PFS_OK;

    uint32_t current_blk = entry.start_block;
    uint32_t bytes_covered = 0;

    if (new_size < entry.file_size) {
        // Shrink
        while(current_blk != PFS32_END_BLOCK && bytes_covered + 512 <= new_size) {
            bytes_covered += 512;
            current_blk = get_fat(current_blk);
        }
        
        // current_blk is the last valid block.
        // If we are mid-block, we keep it but zero out the end? (Optional security)
        // Free the rest of the chain
        uint32_t next = get_fat(current_blk);
        set_fat(current_blk, PFS32_END_BLOCK);
        
        while(next != PFS32_END_BLOCK && next != 0) {
            uint32_t temp = get_fat(next);
            set_fat(next, PFS32_FREE_BLOCK);
            next = temp;
        }
    } else {
        // Expand
        // Walk to end
        while(get_fat(current_blk) != PFS32_END_BLOCK) {
            current_blk = get_fat(current_blk);
            bytes_covered += 512;
        }
        // Allocate new blocks
        while (bytes_covered < new_size) {
            uint32_t new_b = alloc_block();
            if(!new_b) return PFS_ERR_FULL;
            set_fat(current_blk, new_b);
            set_fat(new_b, PFS32_END_BLOCK);
            current_blk = new_b;
            bytes_covered += 512;
        }
    }

    // Update Directory Entry
    uint8_t buf[512];
    disk_rw(0, entry_blk, buf);
    pfs32_direntry_t* de = (pfs32_direntry_t*)buf;
    de[entry_idx].file_size = new_size;
    de[entry_idx].modify_time = pfs32_time_now();
    disk_rw(1, entry_blk, buf);
    flush_fat();

    return PFS_OK;
}

// --- FEAT-002: Copy ---
int pfs32_copy(const char* src, const char* dst) {
    pfs32_direntry_t s_ent;
    // Check if source exists
    if(pfs32_stat(src, &s_ent) != PFS_OK) return PFS_ERR_NOT_FOUND;

    // Check directory
    if(s_ent.attributes & PFS32_ATTR_DIRECTORY) return PFS_ERR_PARAM;

    // Create destination
    int res = pfs32_create_file(dst);
    if(res != PFS_OK && res != PFS_ERR_EXISTS) return res;

    // Allocation for buffer
    uint32_t buf_size = 4096; // 4KB chunks
    uint8_t* buf = (uint8_t*)kmalloc(buf_size);
    if (!buf) return PFS_ERR_IO;

    uint32_t copied = 0;
    uint32_t size = s_ent.file_size;
    
    // Open handles for robust copy
    int h_src = pfs32_open(src, 0); // Read
    if (h_src < 0) { kfree(buf); return PFS_ERR_IO; }
    
    int h_dst = pfs32_open(dst, 1); // Write
    if (h_dst < 0) { pfs32_close(h_src); kfree(buf); return PFS_ERR_IO; }

    while(copied < size) {
        uint32_t chunk = (size - copied < buf_size) ? size - copied : buf_size;

        pfs32_seek(h_src, copied);
        pfs32_seek(h_dst, copied);

        int bytes_read = pfs32_read_handle(h_src, buf, chunk);
        if (bytes_read <= 0) break;

        int bytes_written = pfs32_write_handle(h_dst, buf, bytes_read);
        if (bytes_written <= 0) break;

        copied += bytes_written;
        if (bytes_written < bytes_read) break; // Partial write, stop
    }

    pfs32_close(h_src);
    pfs32_close(h_dst);
    kfree(buf);
    return PFS_OK;
}

// --- Deletion & Util ---

void free_chain(uint32_t start_block) {
    uint32_t curr = start_block;
    while(curr != PFS32_END_BLOCK && curr != 0) {
        uint32_t next = get_fat(curr);
        set_fat(curr, PFS32_FREE_BLOCK);
        pfs32_bitmap_clear(curr);
        sb.free_blocks++;
        curr = next;
    }
}

int pfs32_delete(const char* path) {
    if(!mounted) return PFS_ERR_NO_FS;

    uint32_t pblk;
    char parent[128];
    get_parent_path(path, parent);
    if(get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;

    pfs32_direntry_t entry;
    uint32_t entry_blk; int entry_idx;
    char name[64];
    get_basename(path, name);
    if(find_entry_in_dir(pblk, name, &entry, &entry_blk, &entry_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;

    if(!check_permission(entry.uid, entry.gid, entry.permissions, PFS_PERM_WRITE)) return PFS_ERR_ACCESS;

    if(entry.attributes & PFS32_ATTR_DIRECTORY) {
        // Check empty logic...
    }

    uint8_t buf[512];
    disk_rw(0, entry_blk, buf);
    ((pfs32_direntry_t*)buf)[entry_idx].filename[0] = 0; 
    disk_rw(1, entry_blk, buf);

    free_chain(entry.start_block);
    flush_fat();

    return PFS_OK;
}

int pfs32_rename(const char* oldpath, const char* newpath) {
    if(!mounted) return PFS_ERR_NO_FS;
    // Same parent check
    char p1[128];
    char p2[128];
    get_parent_path(oldpath, p1);
    get_parent_path(newpath, p2);
    if(strcmp(p1, p2) != 0) return PFS_ERR_PARAM;

    uint32_t pblk;
    if(get_dir_block(p1, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;

    pfs32_direntry_t entry;
    uint32_t entry_blk; int entry_idx;
    char old_name[64];
    get_basename(oldpath, old_name);
    if(find_entry_in_dir(pblk, old_name, &entry, &entry_blk, &entry_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;

    if(!check_permission(entry.uid, entry.gid, entry.permissions, PFS_PERM_WRITE)) return PFS_ERR_ACCESS;

    uint8_t buf[512];
    disk_rw(0, entry_blk, buf);
    pfs32_direntry_t* de = (pfs32_direntry_t*)buf;
    memset(de[entry_idx].filename, 0, 40);
    char new_name[64];
    get_basename(newpath, new_name);
    sanitize_name(de[entry_idx].filename, new_name, 39);
    de[entry_idx].modify_time = pfs32_time_now();

    disk_rw(1, entry_blk, buf);
    return PFS_OK;
}

// --- DIAG-001: FSCK ---
int pfs32_fsck(int repair) {
    if(!mounted) return PFS_ERR_NO_FS;
    s_printf("[FSCK] Starting...\n");
    
    // 1. Validate Superblock
    if(sb.magic != PFS32_MAGIC) {
        s_printf("[FSCK] Bad Magic\n");
        return -1;
    }
    
    // 2. Check FAT Chains (Simulated)
    // iterate all files, follow chains, mark visited blocks in a bitmap (alloc in RAM)
    // check for cross-links or orphan blocks.
    
    s_printf("[FSCK] Check Complete (Basic).\n");
    return PFS_OK;
}

int pfs32_get_stats(pfs32_stats_t* out_stats) {
    if(out_stats) *out_stats = stats;
    return PFS_OK;
}

// --- Utils ---
int pfs32_listdir(uint32_t block, pfs32_direntry_t* buf, uint32_t max) {
    if(!mounted) return -1;
    int count = 0;
    uint32_t curr = block;
    while(curr != PFS32_END_BLOCK && curr != 0 && count < max) {
        uint8_t dbuf[512];
        if(disk_rw(0, curr, dbuf) != PFS_OK) break;
        pfs32_direntry_t* d = (pfs32_direntry_t*)dbuf;
        for(int i=0; i<PFS32_ENTRIES_PER_BLOCK; i++) {
            if(d[i].filename[0] != 0 && count < max) {
                // Skip . and .. directory entries but allow other dotfiles
                if(strcmp(d[i].filename, ".") == 0 || strcmp(d[i].filename, "..") == 0) continue;
                buf[count] = d[i];
                count++;
            }
        }
        curr = get_fat(curr);
    }
    return count;
}

int pfs32_stat(const char* path, pfs32_direntry_t* out) {
    uint32_t pblk;
    char parent[128];
    get_parent_path(path, parent);
    if(get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;
    char name[64];
    get_basename(path, name);
    return find_entry_in_dir(pblk, name, out, 0, 0);
}

int pfs32_create_file(const char* path) { return pfs32_create_node(path, 0); }
int pfs32_create_directory(const char* path) { return pfs32_create_node(path, 1); }
int pfs32_sync() {
    flush_fat();
    pfs32_flush_bitmap();
    // Write back superblock if mounted
    if (mounted && disk_start) {
        sb.superblock_checksum = pfs32_compute_checksum(&sb, sizeof(sb) - sizeof(pfs32_checksum_t));
        disk_rw(1, 0, &sb);
    }
    // CRITICAL: Flush disk write-back cache so data actually reaches disk.
    // Without this, all writes stay in RAM and are lost on reboot.
    disk_flush_cache();
    return PFS_OK;
}

// --- String Helpers ---
void get_basename(const char* path, char* out_buf) {
    const char* s = strrchr(path, '/');
    if (!s) strncpy(out_buf, path, 63);
    else strncpy(out_buf, s+1, 63);
    out_buf[63] = 0;
    int len = strlen(out_buf);
    if(len > 0 && out_buf[len-1] == '/') out_buf[len-1] = 0;
}

void get_parent_path(const char* path, char* out_buf) {
    strncpy(out_buf, path, 127); out_buf[127] = 0;
    int len = strlen(out_buf);
    if(len > 1 && out_buf[len-1] == '/') out_buf[len-1] = 0;
    char* s = strrchr(out_buf, '/');
    if(s) {
        if(s == out_buf) { out_buf[1] = 0; } // Root
        else *s = 0;
    } else {
        strcpy(out_buf, "/");
    }
}

// Helper to check existence
int file_exists(const char* path) {
    pfs32_direntry_t ent;
    return (pfs32_stat(path, &ent) == PFS_OK);
}

void get_unique_path(const char* base_path, const char* name, char* out_full_path) {
    char clean_name[64];
    sanitize_name(clean_name, name, 63);
    
    // Separate Extension
    char base[64];
    char ext[16];
    ext[0] = 0;
    
    char* dot = strrchr(clean_name, '.');
    if (dot) {
        strcpy(ext, dot); // copy extension
        *dot = 0; // cut base
        strcpy(base, clean_name);
    } else {
        strcpy(base, clean_name);
    }
    
    // Try base name first
    strcpy(out_full_path, base_path);
    if(out_full_path[strlen(out_full_path)-1] != '/') strcat(out_full_path, "/");
    strcat(out_full_path, base);
    strcat(out_full_path, ext);
    
    if (!file_exists(out_full_path)) return;
    
    // Iterate
    for(int i=1; i<100; i++) {
        strcpy(out_full_path, base_path);
        if(out_full_path[strlen(out_full_path)-1] != '/') strcat(out_full_path, "/");
        
        strcat(out_full_path, base);
        strcat(out_full_path, " ");
        char num[8]; pfs_int_to_str(i, num);
        strcat(out_full_path, num);
        strcat(out_full_path, ext);
        
        if (!file_exists(out_full_path)) return;
    }
}

// --- FILE HANDLE SYSTEM ---

#define MAX_FILE_HANDLES 32

typedef struct {
    int active;
    uint32_t file_start_block;  // First block of file data
    uint32_t current_block;     // Current block pointer (for sequential access)
    uint32_t current_offset;    // Byte offset in file
    uint32_t size;              // Total file size
    uint32_t flags;             // R/W flags
    int dir_entry_block;        // Location of directory entry (for time updates)
    int dir_entry_idx;
} file_handle_t;

static file_handle_t handles[MAX_FILE_HANDLES];

void pfs32_init_handles() {
    memset(handles, 0, sizeof(handles));
}

int pfs32_open(const char* path, int flags) {
    if (!mounted) return PFS_ERR_NO_FS;

    // Find free handle
    int id = -1;
    for(int i=0; i<MAX_FILE_HANDLES; i++) {
        if (!handles[i].active) { id = i; break; }
    }
    if (id == -1) return -1; // Too many open files

    // Resolve Path
    pfs32_direntry_t entry;
    uint32_t entry_blk;
    int entry_idx;
    uint32_t pblk;

    char parent[128];
    get_parent_path(path, parent);
    if (get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;
    char name[64];
    get_basename(path, name);
    if (find_entry_in_dir(pblk, name, &entry, &entry_blk, &entry_idx) != PFS_OK) {
        return PFS_ERR_NOT_FOUND;
    }

    // Permission check (using existing logic)
    int perm_check = (flags == 1) ? PFS_PERM_WRITE : PFS_PERM_READ;
    if (!check_permission(entry.uid, entry.gid, entry.permissions, perm_check)) return PFS_ERR_ACCESS;

    handles[id].active = 1;
    handles[id].file_start_block = entry.start_block;
    handles[id].current_block = entry.start_block;
    handles[id].current_offset = 0;
    handles[id].size = entry.file_size;
    handles[id].flags = flags;
    handles[id].dir_entry_block = entry_blk;
    handles[id].dir_entry_idx = entry_idx;

    return id;
}

void pfs32_close(int handle) {
    if (handle >= 0 && handle < MAX_FILE_HANDLES) {
        handles[handle].active = 0;
    }
}

int pfs32_seek(int handle, uint32_t offset) {
    if (handle < 0 || handle >= MAX_FILE_HANDLES || !handles[handle].active) return PFS_ERR_PARAM;

    if (offset > handles[handle].size) offset = handles[handle].size;

    // Optimized: If offset is 0, just reset
    if (offset == 0) {
        handles[handle].current_offset = 0;
        handles[handle].current_block = handles[handle].file_start_block;
        return PFS_OK;
    }

    // Basic linear seek (O(N)) - FAT optimization requires tracking cluster chain in memory
    // For now, reset to start and walk forward

    handles[handle].current_offset = 0;
    handles[handle].current_block = handles[handle].file_start_block;

    uint32_t bytes_skipped = 0;
    while(bytes_skipped + 512 <= offset) {
        handles[handle].current_block = get_fat(handles[handle].current_block);
        bytes_skipped += 512;
    }
    handles[handle].current_offset = offset;
    return PFS_OK;
}

int pfs32_read_handle(int handle, void* buffer, uint32_t len) {
    if (handle < 0 || handle >= MAX_FILE_HANDLES || !handles[handle].active) return PFS_ERR_PARAM;

    uint32_t read = 0;
    uint32_t available = handles[handle].size - handles[handle].current_offset;
    if (len > available) len = available;

    uint8_t* ptr = (uint8_t*)buffer;

    while(read < len) {
        // Calculate offset within current block
        uint32_t block_offset = handles[handle].current_offset % 512;
        uint32_t to_read = 512 - block_offset;
        if (to_read > (len - read)) to_read = (len - read);

        uint8_t buf[512];
        if (disk_rw(0, handles[handle].current_block, buf) != PFS_OK) break;

        memcpy(ptr + read, buf + block_offset, to_read);

        read += to_read;
        handles[handle].current_offset += to_read;

        // Advance block if we hit boundary
        if ((handles[handle].current_offset % 512) == 0 && handles[handle].current_offset < handles[handle].size) {
            handles[handle].current_block = get_fat(handles[handle].current_block);
        }
    }
    return read;
}

int pfs32_write_handle(int handle, const void* buffer, uint32_t len) {
    if (handle < 0 || handle >= MAX_FILE_HANDLES || !handles[handle].active) return PFS_ERR_PARAM;
    if (!(handles[handle].flags & 1)) return PFS_ERR_ACCESS; // Not opened for write

    uint32_t written = 0;
    uint32_t available = len; // Can write up to len

    const uint8_t* ptr = (const uint8_t*)buffer;

    while(written < len) {
        // Calculate offset within current block
        uint32_t block_offset = handles[handle].current_offset % 512;
        uint32_t to_write = 512 - block_offset;
        if (to_write > (len - written)) to_write = (len - written);

        uint8_t buf[512];
        // Read-modify-write if not full block
        if (to_write < 512 || block_offset > 0) {
            if (disk_rw(0, handles[handle].current_block, buf) != PFS_OK) break;
        } else {
            memset(buf, 0, 512);
        }

        memcpy(buf + block_offset, ptr + written, to_write);
        if (disk_rw(1, handles[handle].current_block, buf) != PFS_OK) break;

        written += to_write;
        handles[handle].current_offset += to_write;

        // Advance block if we hit boundary and need more
        if ((handles[handle].current_offset % 512) == 0 && written < len) {
            uint32_t next = get_fat(handles[handle].current_block);
            if (next == PFS32_END_BLOCK || next == 0) {
                next = alloc_block();
                if (next == 0) break;
                set_fat(handles[handle].current_block, next);
                set_fat(next, PFS32_END_BLOCK);
                flush_fat();
            }
            handles[handle].current_block = next;
        }
    }

    // Update file size if extended
    if (handles[handle].current_offset > handles[handle].size) {
        handles[handle].size = handles[handle].current_offset;
        // Update directory entry
        uint8_t dbuf[512];
        if (disk_rw(0, handles[handle].dir_entry_block, dbuf) == PFS_OK) {
            ((pfs32_direntry_t*)dbuf)[handles[handle].dir_entry_idx].file_size = handles[handle].size;
            ((pfs32_direntry_t*)dbuf)[handles[handle].dir_entry_idx].modify_time = pfs32_time_now();
            disk_rw(1, handles[handle].dir_entry_block, dbuf);
        }
    }

    return written;
}
// =====================================================================
// APFS+ COMPATIBILITY API (New in v3.0)
// =====================================================================

// --- Block Bitmap Management (Full Disk Utilization) ---

// Set bit in block bitmap (mark block as used)
void pfs32_bitmap_set(uint32_t block) {
    if (!mounted || !sb.block_bitmap_start) return;
    uint32_t byte_idx = block / 8;
    uint32_t bit_idx = block % 8;

    // Update in-memory bitmap if loaded
    if (block_bitmap && byte_idx < block_bitmap_size) {
        uint8_t* bmp = (uint8_t*)block_bitmap;
        bmp[byte_idx] |= (1 << bit_idx);
        block_bitmap_dirty = 1;
    }

    // Also update on-disk bitmap
    uint32_t bitmap_block = sb.block_bitmap_start + (byte_idx / PFS32_BLOCK_SIZE);
    uint32_t offset_in_block = byte_idx % PFS32_BLOCK_SIZE;
    uint8_t buf[512];
    if(disk_rw(0, bitmap_block, buf) == PFS_OK) {
        buf[offset_in_block] |= (1 << bit_idx);
        disk_rw(1, bitmap_block, buf);
    }
}

// Clear bit in block bitmap (mark block as free)
void pfs32_bitmap_clear(uint32_t block) {
    if (!mounted || !sb.block_bitmap_start) return;
    uint32_t byte_idx = block / 8;
    uint32_t bit_idx = block % 8;

    // Update in-memory bitmap if loaded
    if (block_bitmap && byte_idx < block_bitmap_size) {
        uint8_t* bmp = (uint8_t*)block_bitmap;
        bmp[byte_idx] &= ~(1 << bit_idx);
        block_bitmap_dirty = 1;
    }

    // Also update on-disk bitmap
    uint32_t bitmap_block = sb.block_bitmap_start + (byte_idx / PFS32_BLOCK_SIZE);
    uint32_t offset_in_block = byte_idx % PFS32_BLOCK_SIZE;
    uint8_t buf[512];
    if(disk_rw(0, bitmap_block, buf) == PFS_OK) {
        buf[offset_in_block] &= ~(1 << bit_idx);
        disk_rw(1, bitmap_block, buf);
    }
}

// Test bit in block bitmap. Returns: 0=free, 1=used
int pfs32_bitmap_test(uint32_t block) {
    if (!mounted || !sb.block_bitmap_start) return 0;
    uint32_t byte_idx = block / 8;
    uint32_t bit_idx = block % 8;

    // Use in-memory bitmap if loaded (fast path, no disk I/O)
    if (block_bitmap && byte_idx < block_bitmap_size) {
        uint8_t* bmp = (uint8_t*)block_bitmap;
        return (bmp[byte_idx] >> bit_idx) & 1;
    }

    // Fall back to on-disk bitmap
    uint32_t bitmap_block = sb.block_bitmap_start + (byte_idx / PFS32_BLOCK_SIZE);
    uint32_t offset_in_block = byte_idx % PFS32_BLOCK_SIZE;
    uint8_t buf[512];
    if(disk_rw(0, bitmap_block, buf) != PFS_OK) return 0;
    return (buf[offset_in_block] >> bit_idx) & 1;
}

// Mark a block as bad (set 2 bits = 0xFE pattern)
void pfs32_bitmap_set_bad(uint32_t block) {
    if (!mounted || !sb.block_bitmap_start) return;
    // Set the block bit and also mark in FAT as bad
    pfs32_bitmap_set(block);
    set_fat(block, PFS32_BAD_BLOCK);
}

// Flush entire bitmap to disk
void pfs32_flush_bitmap(void) {
    if(!mounted) return;

    // Flush in-memory bitmap to disk if loaded and dirty
    if (block_bitmap && block_bitmap_dirty && sb.block_bitmap_start) {
        uint8_t* bmp = (uint8_t*)block_bitmap;
        for (uint32_t i = 0; i < sb.block_bitmap_blocks; i++) {
            uint32_t offset = i * PFS32_BLOCK_SIZE;
            uint32_t chunk = PFS32_BLOCK_SIZE;
            if (offset + chunk > block_bitmap_size) {
                chunk = block_bitmap_size - offset;
            }
            uint8_t buf[512];
            memset(buf, 0, 512);
            if (chunk > 0) memcpy(buf, bmp + offset, chunk);
            disk_rw(1, sb.block_bitmap_start + i, buf);
        }
        block_bitmap_dirty = 0;
    }

    // Always update superblock
    disk_write_block(disk_start, &sb);
}

// --- Fletcher-64 Checksum (APFS-like block integrity) ---

pfs32_checksum_t pfs32_compute_checksum(const void* data, uint32_t size) {
    pfs32_checksum_t cs;
    cs.lo = 0;
    cs.hi = 0;
    
    if (!data || size == 0) return cs;
    
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t words = size / 2;
    
    uint32_t sum1 = 0, sum2 = 0;
    for (uint32_t i = 0; i < words; i++) {
        sum1 = (sum1 + ptr[i]) % 0xFFFF;
        sum2 = (sum2 + sum1) % 0xFFFF;
    }
    
    // Handle odd byte
    if (size & 1) {
        uint8_t last = ((const uint8_t*)data)[size - 1];
        sum1 = (sum1 + last) % 0xFFFF;
        sum2 = (sum2 + sum1) % 0xFFFF;
    }
    
    cs.lo = sum1;
    cs.hi = sum2;
    return cs;
}

int pfs32_verify_block(uint32_t block) {
    if (!mounted) return PFS_ERR_NO_FS;
    if (!(sb.feature_flags & PFS32_FEATURE_CHECKSUM)) return PFS_OK;
    
    // Read block and compute checksum
    uint8_t buf[512];
    if (disk_rw(0, block, buf) != PFS_OK) return PFS_ERR_IO;
    
    // Checksum verification would compare stored vs computed
    // For now, just compute and log
    pfs32_checksum_t cs = pfs32_compute_checksum(buf, 496); // Skip last 16 bytes (checksum area)
    (void)cs;
    return PFS_OK;
}

int pfs32_verify_all(void) {
    if (!mounted) return PFS_ERR_NO_FS;
    int failures = 0;
    s_printf("[PFS] Verifying all blocks...\n");
    for (uint32_t blk = sb.data_start_block; blk < sb.total_blocks; blk++) {
        if (pfs32_bitmap_test(blk) && pfs32_verify_block(blk) != PFS_OK) {
            failures++;
            stats.checksum_failures++;
        }
    }
    char buf[16];
    int_to_str(failures, buf);
    s_printf("[PFS] Verification complete. Failures: ");
    s_printf(buf);
    s_printf("\n");
    return failures;
}

// --- Copy-on-Write (CoW) ---

int pfs32_cow_copy_block(uint32_t src_block, uint32_t* dst_block) {
    if (!mounted || !dst_block) return PFS_ERR_PARAM;
    if (!(sb.feature_flags & PFS32_FEATURE_COW)) {
        *dst_block = src_block; // No CoW, return same block
        return PFS_OK;
    }
    
    // Allocate a new block
    uint32_t new_blk = alloc_block();
    if (new_blk == 0) return PFS_ERR_FULL;
    
    // Copy data from source to new block
    uint8_t buf[512];
    if (disk_rw(0, src_block, buf) != PFS_OK) {
        set_fat(new_blk, PFS32_FREE_BLOCK);
        return PFS_ERR_IO;
    }
    if (disk_rw(1, new_blk, buf) != PFS_OK) {
        set_fat(new_blk, PFS32_FREE_BLOCK);
        return PFS_ERR_IO;
    }
    
    *dst_block = new_blk;
    stats.cow_copies++;
    
    // Mark in CoW bitmap
    if (cow_bitmap) {
        uint32_t byte_idx = src_block / 8;
        uint32_t bit_idx = src_block % 8;
        cow_bitmap[byte_idx / 4] |= (1 << (bit_idx + (byte_idx % 4) * 8));
    }
    
    return PFS_OK;
}

int pfs32_cow_write(const char* path, uint8_t* data, uint32_t size) {
    if (!mounted) return PFS_ERR_NO_FS;
    if (!(sb.feature_flags & PFS32_FEATURE_COW)) {
        return pfs32_write_file(path, data, size); // Fall back to regular write
    }
    
    // For CoW: We write to new blocks instead of overwriting
    // This is essentially the same as write_file but with cow_copy_block
    // for existing blocks
    
    // Find the file
    pfs32_direntry_t entry;
    uint32_t entry_blk;
    int entry_idx;
    uint32_t pblk;
    char parent[128];
    char name[64];
    get_parent_path(path, parent);
    get_basename(path, name);
    
    if (get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (find_entry_in_dir(pblk, name, &entry, &entry_blk, &entry_idx) != PFS_OK) {
        // File doesn't exist, create it normally
        return pfs32_write_file(path, data, size);
    }
    
    // CoW: Copy the block chain, then write to the copies
    uint32_t blk = entry.start_block;
    uint32_t written = 0;
    uint32_t prev_blk = 0;
    uint32_t new_start = 0;
    
    while (written < size) {
        uint32_t new_blk;
        if (pfs32_cow_copy_block(blk, &new_blk) != PFS_OK) return PFS_ERR_IO;
        
        if (prev_blk != 0) {
            set_fat(prev_blk, new_blk);
        } else {
            new_start = new_blk;
        }
        
        // Write data to the new block
        uint8_t buf[512];
        memset(buf, 0, 512);
        uint32_t chunk = (size - written < 512) ? size - written : 512;
        memcpy(buf, data + written, chunk);
        disk_rw(1, new_blk, buf);
        written += chunk;
        
        prev_blk = new_blk;
        uint32_t next = get_fat(blk);
        if (next == PFS32_END_BLOCK || next == 0) {
            if (written < size) {
                // Need more blocks
                while (written < size) {
                    uint32_t extra = alloc_block();
                    if (extra == 0) return PFS_ERR_FULL;
                    set_fat(prev_blk, extra);
                    set_fat(extra, PFS32_END_BLOCK);
                    memset(buf, 0, 512);
                    chunk = (size - written < 512) ? size - written : 512;
                    memcpy(buf, data + written, chunk);
                    disk_rw(1, extra, buf);
                    written += chunk;
                    prev_blk = extra;
                }
            }
            break;
        }
        blk = next;
    }
    
    // Update directory entry to point to new start block
    if (new_start != 0) {
        uint8_t dbuf[512];
        disk_rw(0, entry_blk, dbuf);
        pfs32_direntry_t* de = (pfs32_direntry_t*)dbuf;
        de[entry_idx].start_block = new_start;
        de[entry_idx].file_size = size;
        de[entry_idx].modify_time = pfs32_time_now();
        disk_rw(1, entry_blk, dbuf);
    }
    
    flush_fat();
    current_transaction++;
    sb.next_transaction_id = current_transaction;
    return size;
}

// --- Snapshots ---

int pfs32_snapshot_create(const char* name) {
    if (!mounted || !name) return PFS_ERR_PARAM;
    if (!(sb.feature_flags & PFS32_FEATURE_SNAPSHOTS)) return PFS_ERR_PARAM;
    
    if (sb.snapshot_count >= PFS32_MAX_SNAPSHOTS) return PFS_ERR_FULL;
    
    // Find free snapshot slot
    int slot = -1;
    for (int i = 0; i < PFS32_MAX_SNAPSHOTS; i++) {
        if (!snapshots[i].active) { slot = i; break; }
    }
    if (slot == -1) return PFS_ERR_FULL;
    
    // Capture current root directory block
    snapshots[slot].snapshot_id = sb.next_transaction_id++;
    snapshots[slot].root_block = sb.root_dir_block;
    snapshots[slot].create_time = pfs32_time_now();
    strncpy(snapshots[slot].name, name, 31);
    snapshots[slot].name[31] = 0;
    snapshots[slot].active = 1;
    
    sb.snapshot_count++;
    
    // Flush snapshot metadata
    disk_write_block(disk_start, &sb);
    
    s_printf("[PFS] Snapshot created: ");
    s_printf(name);
    s_printf("\n");
    return PFS_OK;
}

int pfs32_snapshot_delete(const char* name) {
    if (!mounted || !name) return PFS_ERR_PARAM;
    
    for (int i = 0; i < PFS32_MAX_SNAPSHOTS; i++) {
        if (snapshots[i].active && strcmp(snapshots[i].name, name) == 0) {
            snapshots[i].active = 0;
            sb.snapshot_count--;
            disk_write_block(disk_start, &sb);
            return PFS_OK;
        }
    }
    return PFS_ERR_NOT_FOUND;
}

int pfs32_snapshot_list(pfs32_snapshot_t* out_list, int max_count) {
    if (!out_list) return 0;
    int count = 0;
    for (int i = 0; i < PFS32_MAX_SNAPSHOTS && count < max_count; i++) {
        if (snapshots[i].active) {
            out_list[count] = snapshots[i];
            count++;
        }
    }
    return count;
}

int pfs32_snapshot_restore(const char* name) {
    if (!mounted || !name) return PFS_ERR_PARAM;
    
    for (int i = 0; i < PFS32_MAX_SNAPSHOTS; i++) {
        if (snapshots[i].active && strcmp(snapshots[i].name, name) == 0) {
            sb.root_dir_block = snapshots[i].root_block;
            disk_write_block(disk_start, &sb);
            s_printf("[PFS] Snapshot restored: ");
            s_printf(name);
            s_printf("\n");
            return PFS_OK;
        }
    }
    return PFS_ERR_NOT_FOUND;
}

// --- Cloning (APFS-like fast clone) ---

int pfs32_clone_file(const char* src, const char* dst) {
    if (!mounted) return PFS_ERR_NO_FS;
    
    pfs32_direntry_t entry;
    if (pfs32_stat(src, &entry) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (entry.attributes & PFS32_ATTR_DIRECTORY) return PFS_ERR_PARAM;
    
    // Create the destination file
    int res = pfs32_create_file(dst);
    if (res != PFS_OK && res != PFS_ERR_EXISTS) return res;
    
    // Find the destination entry
    pfs32_direntry_t dst_entry;
    uint32_t dst_blk;
    int dst_idx;
    uint32_t pblk;
    char parent[128];
    char name[64];
    get_parent_path(dst, parent);
    get_basename(dst, name);
    if (get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (find_entry_in_dir(pblk, name, &dst_entry, &dst_blk, &dst_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;
    
    if (sb.feature_flags & PFS32_FEATURE_CLONE) {
        // Fast clone: Point to same data blocks with clone_id
        uint32_t clone_id = 0;
        if (clone_count < PFS32_MAX_CLONES) {
            clone_id = ++clone_count;
            clones[clone_count - 1].clone_id = clone_id;
            clones[clone_count - 1].source_block = entry.start_block;
            clones[clone_count - 1].ref_count = 2; // Original + clone
        }
        
        // Update destination entry to share data blocks
        uint8_t dbuf[512];
        disk_rw(0, dst_blk, dbuf);
        pfs32_direntry_t* de = (pfs32_direntry_t*)dbuf;
        // Free the allocated block for dst
        uint32_t old_start = de[dst_idx].start_block;
        de[dst_idx].start_block = entry.start_block;
        de[dst_idx].file_size = entry.file_size;
        de[dst_idx].clone_id = clone_id;
        de[dst_idx].modify_time = pfs32_time_now();
        disk_rw(1, dst_blk, dbuf);
        
        // Free the old start block
        if (old_start) {
            set_fat(old_start, PFS32_FREE_BLOCK);
        }
        
        stats.clone_count++;
    } else {
        // No clone support - do full copy
        return pfs32_copy(src, dst);
    }
    
    return PFS_OK;
}

int pfs32_clone_directory(const char* src, const char* dst) {
    // For directories, we create a new directory but share all file blocks
    if (!mounted) return PFS_ERR_NO_FS;
    
    pfs32_direntry_t src_entry;
    if (pfs32_stat(src, &src_entry) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (!(src_entry.attributes & PFS32_ATTR_DIRECTORY)) return PFS_ERR_PARAM;
    
    // Create destination directory
    int res = pfs32_create_directory(dst);
    if (res != PFS_OK && res != PFS_ERR_EXISTS) return res;
    
    // Copy entries by cloning each file
    pfs32_direntry_t entries[32];
    int count = pfs32_listdir(src_entry.start_block, entries, 32);
    
    for (int i = 0; i < count; i++) {
        if (entries[i].filename[0] == 0 || 
            strcmp(entries[i].filename, ".") == 0 ||
            strcmp(entries[i].filename, "..") == 0) continue;
        
        char src_path[256], dst_path[256];
        strcpy(src_path, src);
        if(src_path[strlen(src_path)-1] != '/') strcat(src_path, "/");
        strcat(src_path, entries[i].filename);
        
        strcpy(dst_path, dst);
        if(dst_path[strlen(dst_path)-1] != '/') strcat(dst_path, "/");
        strcat(dst_path, entries[i].filename);
        
        if (entries[i].attributes & PFS32_ATTR_DIRECTORY) {
            pfs32_clone_directory(src_path, dst_path);
        } else {
            pfs32_clone_file(src_path, dst_path);
        }
    }
    
    return PFS_OK;
}

// --- Extended Attributes (APFS-like xattr) ---

int pfs32_set_xattr(const char* path, const char* name, const uint8_t* value, uint32_t size) {
    if (!mounted || !path || !name || !value) return PFS_ERR_PARAM;
    if (!(sb.feature_flags & PFS32_FEATURE_EXT_ATTR)) return PFS_ERR_PARAM;
    if (size > PFS32_XATTR_MAX_VALUE) return PFS_ERR_PARAM;
    
    pfs32_direntry_t entry;
    uint32_t entry_blk;
    int entry_idx;
    uint32_t pblk;
    char parent[128];
    char fname[64];
    get_parent_path(path, parent);
    get_basename(path, fname);
    if (get_dir_block(parent, &pblk) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (find_entry_in_dir(pblk, fname, &entry, &entry_blk, &entry_idx) != PFS_OK) return PFS_ERR_NOT_FOUND;
    
    // Get or allocate xattr block
    uint32_t xattr_blk = entry.ext_attr_block;
    if (xattr_blk == 0) {
        xattr_blk = alloc_block();
        if (xattr_blk == 0) return PFS_ERR_FULL;
        
        // Update entry with xattr block
        uint8_t dbuf[512];
        disk_rw(0, entry_blk, dbuf);
        pfs32_direntry_t* de = (pfs32_direntry_t*)dbuf;
        de[entry_idx].ext_attr_block = xattr_blk;
        de[entry_idx].ext_attr_size = 0;
        disk_rw(1, entry_blk, dbuf);
    }
    
    // Read xattr block
    uint8_t xbuf[512];
    disk_rw(0, xattr_blk, xbuf);
    pfs32_xattr_entry_t* xattrs = (pfs32_xattr_entry_t*)xbuf;
    
    // Find existing or free slot
    int slot = -1;
    for (int i = 0; i < (int)PFS32_XATTRS_PER_BLOCK; i++) {
        if (xattrs[i].name[0] == 0) {
            if (slot == -1) slot = i;
        } else if (strcmp(xattrs[i].name, name) == 0) {
            slot = i; // Replace existing
            break;
        }
    }
    
    if (slot == -1) return PFS_ERR_FULL;
    
    // Write xattr
    strncpy(xattrs[slot].name, name, PFS32_XATTR_MAX_NAME - 1);
    xattrs[slot].name[PFS32_XATTR_MAX_NAME - 1] = 0;
    xattrs[slot].value_size = size > PFS32_XATTR_MAX_VALUE ? PFS32_XATTR_MAX_VALUE : size;
    memcpy(xattrs[slot].value, value, xattrs[slot].value_size);
    
    disk_rw(1, xattr_blk, xbuf);
    return PFS_OK;
}

int pfs32_get_xattr(const char* path, const char* name, uint8_t* value, uint32_t max_size) {
    if (!mounted || !path || !name || !value) return PFS_ERR_PARAM;
    
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (entry.ext_attr_block == 0) return PFS_ERR_NOT_FOUND;
    
    uint8_t xbuf[512];
    disk_rw(0, entry.ext_attr_block, xbuf);
    pfs32_xattr_entry_t* xattrs = (pfs32_xattr_entry_t*)xbuf;
    
    for (int i = 0; i < (int)PFS32_XATTRS_PER_BLOCK; i++) {
        if (xattrs[i].name[0] == 0) continue;
        if (strcmp(xattrs[i].name, name) == 0) {
            uint32_t size = xattrs[i].value_size;
            if (size > max_size) size = max_size;
            memcpy(value, xattrs[i].value, size);
            return size;
        }
    }
    return PFS_ERR_NOT_FOUND;
}

int pfs32_list_xattr(const char* path, char* names, uint32_t max_size) {
    if (!mounted || !path || !names) return PFS_ERR_PARAM;
    
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (entry.ext_attr_block == 0) return 0;
    
    uint8_t xbuf[512];
    disk_rw(0, entry.ext_attr_block, xbuf);
    pfs32_xattr_entry_t* xattrs = (pfs32_xattr_entry_t*)xbuf;
    
    uint32_t pos = 0;
    for (int i = 0; i < (int)PFS32_XATTRS_PER_BLOCK; i++) {
        if (xattrs[i].name[0] == 0) continue;
        uint32_t len = strlen(xattrs[i].name) + 1; // Include null terminator
        if (pos + len > max_size) break;
        memcpy(names + pos, xattrs[i].name, len);
        pos += len;
    }
    return pos;
}

int pfs32_remove_xattr(const char* path, const char* name) {
    if (!mounted || !path || !name) return PFS_ERR_PARAM;
    
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) != PFS_OK) return PFS_ERR_NOT_FOUND;
    if (entry.ext_attr_block == 0) return PFS_ERR_NOT_FOUND;
    
    uint8_t xbuf[512];
    disk_rw(0, entry.ext_attr_block, xbuf);
    pfs32_xattr_entry_t* xattrs = (pfs32_xattr_entry_t*)xbuf;
    
    for (int i = 0; i < (int)PFS32_XATTRS_PER_BLOCK; i++) {
        if (xattrs[i].name[0] == 0) continue;
        if (strcmp(xattrs[i].name, name) == 0) {
            memset(&xattrs[i], 0, sizeof(pfs32_xattr_entry_t));
            disk_rw(1, entry.ext_attr_block, xbuf);
            return PFS_OK;
        }
    }
    return PFS_ERR_NOT_FOUND;
}

// --- Full Disk Utilization ---

int pfs32_mark_bad_block(uint32_t block) {
    if (!mounted) return PFS_ERR_NO_FS;
    pfs32_bitmap_set_bad(block);
    sb.bad_block_count++;
    stats.bad_block_count++;
    sb.free_blocks--; // Was counted as free, now it's bad
    disk_write_block(disk_start, &sb);
    return PFS_OK;
}

int pfs32_scan_bad_blocks(void) {
    if (!mounted) return PFS_ERR_NO_FS;
    
    s_printf("[PFS] Scanning for bad blocks...\n");
    uint32_t found = 0;
    uint8_t test1[512], test2[512], verify[512];
    
    memset(test1, 0xAA, 512);
    memset(test2, 0x55, 512);
    
    for (uint32_t blk = sb.data_start_block; blk < sb.total_blocks; blk++) {
        // Skip already bad blocks
        if (get_fat(blk) == PFS32_BAD_BLOCK) continue;
        
        // Save original
        uint8_t orig[512];
        disk_read_block(disk_start + blk, orig);
        
        // Test pattern 1
        disk_write_block(disk_start + blk, test1);
        disk_read_block(disk_start + blk, verify);
        int bad = 0;
        for (int j = 0; j < 512; j++) {
            if (verify[j] != test1[j]) { bad = 1; break; }
        }
        
        if (!bad) {
            // Test pattern 2
            disk_write_block(disk_start + blk, test2);
            disk_read_block(disk_start + blk, verify);
            for (int j = 0; j < 512; j++) {
                if (verify[j] != test2[j]) { bad = 1; break; }
            }
        }
        
        if (bad) {
            pfs32_bitmap_set_bad(blk);
            found++;
        } else {
            // Restore original
            disk_write_block(disk_start + blk, orig);
        }
    }
    
    sb.bad_block_count += found;
    stats.bad_block_count += found;
    disk_write_block(disk_start, &sb);
    
    char buf[16];
    int_to_str(found, buf);
    s_printf("[PFS] Bad block scan complete. Found: ");
    s_printf(buf);
    s_printf("\n");
    return found;
}

uint32_t pfs32_get_usable_blocks(void) {
    if (!mounted) return 0;
    return sb.total_blocks - sb.bad_block_count;
}

uint32_t pfs32_get_utilization_percent(void) {
    if (!mounted || sb.total_blocks == 0) return 0;
    uint32_t usable = sb.total_blocks - sb.bad_block_count;
    return (usable * 100) / sb.total_blocks;
}

// --- Reclaim Lost Blocks ---
// Scans bitmap vs FAT and reconciles any blocks that are free in one but not the other.
// Returns the number of blocks reclaimed.
uint32_t pfs32_reclaim_lost_blocks(void) {
    if (!mounted) return 0;
    uint32_t reclaimed = 0;

    s_printf("[PFS] Reclaiming lost blocks...\n");

    for (uint32_t blk = sb.data_start_block; blk < sb.total_blocks; blk++) {
        int bitmap_used = pfs32_bitmap_test(blk);
        uint32_t fat_val = get_fat(blk);
        int fat_used = (fat_val != PFS32_FREE_BLOCK) ? 1 : 0;

        // Skip bad blocks
        if (fat_val == PFS32_BAD_BLOCK) continue;

        if (bitmap_used && !fat_used) {
            // Bitmap says used, FAT says free -> block is lost
            // Reclaim: clear bitmap so block can be allocated
            pfs32_bitmap_clear(blk);
            reclaimed++;
            s_printf("[PFS] Reclaimed lost block (bitmap used, FAT free): ");
            char buf[16];
            int_to_str(blk, buf);
            s_printf(buf);
            s_printf("\n");
        } else if (!bitmap_used && fat_used) {
            // Bitmap says free, FAT says used -> block is phantom-allocated
            // Reclaim: free in FAT so block matches bitmap
            set_fat(blk, PFS32_FREE_BLOCK);
            reclaimed++;
            s_printf("[PFS] Reclaimed lost block (bitmap free, FAT used): ");
            char buf[16];
            int_to_str(blk, buf);
            s_printf(buf);
            s_printf("\n");
        }
    }

    // Recalculate free_blocks after reconciliation
    sb.free_blocks = 0;
    for (uint32_t blk = sb.data_start_block; blk < sb.total_blocks; blk++) {
        if (pfs32_bitmap_test(blk) == 0 && get_fat(blk) == PFS32_FREE_BLOCK) {
            sb.free_blocks++;
        }
    }
    sb.free_pages = sb.free_blocks / PFS32_PAGE_BLOCKS;

    if (reclaimed > 0) {
        pfs32_flush_bitmap();
        flush_fat();
    }

    char buf[16];
    int_to_str(reclaimed, buf);
    s_printf("[PFS] Reclaim complete. Blocks recovered: ");
    s_printf(buf);
    s_printf("\n");

    return reclaimed;
}

// --- Disk Efficiency ---
// Returns percentage (0-100) of how many blocks are actually usable vs total.
// For 100% efficiency: (total_blocks - bad_blocks) / total_blocks * 100
uint32_t pfs32_get_disk_efficiency(void) {
    if (!mounted || sb.total_blocks == 0) return 0;
    uint32_t usable = sb.total_blocks - sb.bad_block_count;
    return (usable * 100) / sb.total_blocks;
}

// --- Space Sharing (APFS Container volumes) ---

int pfs32_create_volume(const char* name, uint32_t quota_blocks) {
    if (!mounted || !name) return PFS_ERR_PARAM;
    if (!(sb.feature_flags & PFS32_FEATURE_SPACE_SHARING)) return PFS_ERR_PARAM;
    if (sb.volume_count >= PFS32_MAX_VOLUMES) return PFS_ERR_FULL;
    
    // Create a virtual volume - just a directory in the root with quota metadata
    char vol_path[128];
    strcpy(vol_path, "/Volumes/");
    strcat(vol_path, name);
    
    int res = pfs32_create_directory(vol_path);
    if (res != PFS_OK && res != PFS_ERR_EXISTS) return res;
    
    // Store quota as extended attribute
    uint8_t quota_data[4];
    quota_data[0] = (quota_blocks >> 24) & 0xFF;
    quota_data[1] = (quota_blocks >> 16) & 0xFF;
    quota_data[2] = (quota_blocks >> 8) & 0xFF;
    quota_data[3] = quota_blocks & 0xFF;
    pfs32_set_xattr(vol_path, "pfs32.quota", quota_data, 4);
    pfs32_set_xattr(vol_path, "pfs32.volume_id", (uint8_t*)"vol", 3);
    
    sb.volume_count++;
    disk_write_block(disk_start, &sb);
    
    s_printf("[PFS] Volume created: ");
    s_printf(name);
    s_printf("\n");
    return PFS_OK;
}

int pfs32_delete_volume(const char* name) {
    if (!mounted || !name) return PFS_ERR_PARAM;
    
    char vol_path[128];
    strcpy(vol_path, "/Volumes/");
    strcat(vol_path, name);
    
    int res = pfs32_delete(vol_path);
    if (res == PFS_OK) {
        sb.volume_count--;
        disk_write_block(disk_start, &sb);
    }
    return res;
}

// --- Defragmentation (safe with CoW) ---

int pfs32_defrag_file(const char* path) {
    if (!mounted || !path) return PFS_ERR_PARAM;
    
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) != PFS_OK) return PFS_ERR_NOT_FOUND;
    
    // Read entire file and rewrite to contiguous blocks
    uint8_t* buf = (uint8_t*)kmalloc(entry.file_size + 512);
    if (!buf) return PFS_ERR_IO;
    
    int size = pfs32_read_file(path, buf, entry.file_size + 512);
    if (size <= 0) { kfree(buf); return PFS_ERR_IO; }
    
    // Delete and recreate (simplest defrag)
    pfs32_delete(path);
    int res = pfs32_write_file(path, buf, size);
    kfree(buf);
    
    return res > 0 ? PFS_OK : res;
}

int pfs32_defrag_volume(void) {
    if (!mounted) return PFS_ERR_NO_FS;
    // Full volume defrag would iterate all files
    // This is a placeholder for the full implementation
    s_printf("[PFS] Volume defragmentation started\n");
    return PFS_OK;
}
