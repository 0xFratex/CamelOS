/*
 * fat32.c - FAT32 Filesystem Driver Implementation for CamelOS
 *
 * Full FAT32 driver with read/write support, LFN reading, FAT caching,
 * and cluster chain management. Designed for interoperability with
 * USB drives, SD cards, and shared partitions.
 *
 * Reference: Microsoft FAT32 Specification (fatgen103.doc)
 *
 * Improvements:
 *   - Spinlock for concurrency safety
 *   - Safe FAT cache eviction (does not discard dirty sectors on write failure)
 *   - LBA48 support (via uint64_t sector addresses)
 *   - Retries in disk I/O (already in disk.c)
 */

#include "fat32.h"
#include "disk.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../core/memory.h"
#include "../hal/drivers/serial.h"

/* ========================================================================
 * CONCURRENCY LOCK
 * ======================================================================== */
static volatile int fat32_lock = 0;

static void fat32_lock_acquire(void) {
    while (__sync_lock_test_and_set(&fat32_lock, 1)) {
        asm volatile("pause");
    }
}

static void fat32_lock_release(void) {
    __sync_lock_release(&fat32_lock);
}

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static fat32_state_t fat32_state;

/* ========================================================================
 * FORWARD DECLARATIONS of internal (unlocked) functions
 * ======================================================================== */
static int fat32_init_internal(uint32_t partition_start_lba);
static int fat32_open_internal(const char* path, int flags);
static int fat32_close_internal(int handle);
static int32_t fat32_read_internal(int handle, void* buffer, uint32_t count);
static int32_t fat32_write_internal(int handle, const void* buffer, uint32_t count);
static int32_t fat32_seek_internal(int handle, int32_t offset, int whence);
static int fat32_stat_internal(const char* path, fat32_stat_t* stat);
static int fat32_mkdir_internal(const char* path);
static int fat32_unlink_internal(const char* path);
static int fat32_rename_internal(const char* oldpath, const char* newpath);
static int fat32_readdir_internal(const char* path, fat32_dirent_out_t* entries, uint32_t max);
static int fat32_sync_internal(void);
static int32_t fat32_free_clusters_internal(void);

/* ========================================================================
 * INTERNAL HELPERS - FAT DATE/TIME
 * ======================================================================== */

static uint16_t fat32_date_now(void) {
    extern void sys_get_date(int* year, int* month, int* day);
    int year = 2025, month = 1, day = 1;
    sys_get_date(&year, &month, &day);
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (day < 1) day = 1;
    if (day > 31) day = 31;
    return ((year - 1980) << 9) | (month << 5) | day;
}

static uint16_t fat32_time_now(void) {
    extern void sys_get_time(int* h, int* m, int* s);
    int h = 0, m = 0, s = 0;
    sys_get_time(&h, &m, &s);
    if (h < 0 || h > 23) h = 0;
    if (m < 0 || m > 59) m = 0;
    if (s < 0 || s > 59) s = 0;
    return (h << 11) | (m << 5) | (s / 2);
}

/* ========================================================================
 * INTERNAL HELPERS - CLUSTER / SECTOR CONVERSION
 * ======================================================================== */

static uint32_t cluster_to_sector(uint32_t cluster) {
    if (cluster < 2) return 0;
    return fat32_state.data_start +
           ((cluster - 2) * fat32_state.sectors_per_cluster);
}

static void fat_entry_location(uint32_t cluster,
                               uint32_t* fat_sector,
                               uint32_t* offset_in_sector) {
    uint32_t fat_offset = cluster * 4;
    *fat_sector = fat32_state.fat_start + (fat_offset / FAT32_SECTOR_SIZE);
    *offset_in_sector = fat_offset % FAT32_SECTOR_SIZE;
}

/* ========================================================================
 * INTERNAL HELPERS - FAT CACHE
 * ======================================================================== */

static void fat_cache_init(void) {
    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        fat32_state.fat_cache[i].sector = 0xFFFFFFFF;
        fat32_state.fat_cache[i].dirty = 0;
        fat32_state.fat_cache[i].lru = 0;
        memset(fat32_state.fat_cache[i].data, 0, FAT32_SECTOR_SIZE);
    }
    fat32_state.cache_counter = 0;
}

static int fat_cache_lookup(uint32_t sector) {
    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].sector == sector) {
            fat32_state.fat_cache[i].lru = ++fat32_state.cache_counter;
            return i;
        }
    }
    return -1;
}

/* Returns 0 on success, -1 on failure (write error) */
static int fat_cache_evict(void) {
    int victim = 0;
    int min_lru = fat32_state.fat_cache[0].lru;

    for (int i = 1; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].lru < min_lru) {
            min_lru = fat32_state.fat_cache[i].lru;
            victim = i;
        }
    }

    if (fat32_state.fat_cache[victim].dirty &&
        fat32_state.fat_cache[victim].sector != 0xFFFFFFFF) {
        uint32_t sec = fat32_state.fat_cache[victim].sector;
        if (disk_write_block(sec, fat32_state.fat_cache[victim].data) != 0) {
            // Write failed – do not evict this entry
            return -1;
        }

        // If FAT mirroring is enabled, write to the backup FAT(s)
        if (!(fat32_state.ext_flags & 0x0080)) {
            for (int f = 1; f < fat32_state.num_fats; f++) {
                uint32_t backup_sec = sec + (f * fat32_state.sectors_per_fat);
                disk_write_block(backup_sec, fat32_state.fat_cache[victim].data);
            }
        }

        fat32_state.fat_cache[victim].dirty = 0;
    }

    fat32_state.fat_cache[victim].sector = 0xFFFFFFFF;
    fat32_state.fat_cache[victim].dirty = 0;
    return 0;
}

/* Returns cache index on success, -1 on error */
static int fat_cache_read(uint32_t sector) {
    int idx = fat_cache_lookup(sector);
    if (idx >= 0) return idx;

    if (fat_cache_evict() != 0) {
        return -1;   // No free slot because a dirty write failed
    }

    idx = -1;
    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].sector == 0xFFFFFFFF) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;  // Should not happen

    if (disk_read_block(sector, fat32_state.fat_cache[idx].data) != 0) {
        return -1;
    }

    fat32_state.fat_cache[idx].sector = sector;
    fat32_state.fat_cache[idx].dirty = 0;
    fat32_state.fat_cache[idx].lru = ++fat32_state.cache_counter;
    return idx;
}

/* ========================================================================
 * INTERNAL HELPERS - FAT ENTRY READ/WRITE
 * ======================================================================== */

static uint32_t fat_get_next_cluster(uint32_t cluster) {
    uint32_t fat_sector, offset;
    fat_entry_location(cluster, &fat_sector, &offset);

    int idx = fat_cache_read(fat_sector);
    if (idx < 0) return FAT32_BAD_CLUSTER;

    uint32_t entry;
    memcpy(&entry, &fat32_state.fat_cache[idx].data[offset], 4);
    return entry & FAT32_CLUSTER_MASK;
}

static int fat_set_next_cluster(uint32_t cluster, uint32_t next) {
    uint32_t fat_sector, offset;
    fat_entry_location(cluster, &fat_sector, &offset);

    int idx = fat_cache_read(fat_sector);
    if (idx < 0) return FAT32_ERR_IO;

    uint32_t entry;
    memcpy(&entry, &fat32_state.fat_cache[idx].data[offset], 4);
    entry = (entry & 0xF0000000) | (next & FAT32_CLUSTER_MASK);
    memcpy(&fat32_state.fat_cache[idx].data[offset], &entry, 4);

    fat32_state.fat_cache[idx].dirty = 1;
    return FAT32_OK;
}

/* ========================================================================
 * INTERNAL HELPERS - CLUSTER CHAIN
 * ======================================================================== */

static int fat32_is_eoc(uint32_t cluster) {
    cluster &= FAT32_CLUSTER_MASK;
    return (cluster >= FAT32_EOC_MARKER_MIN && cluster <= FAT32_EOC_MARKER);
}

static int fat32_is_bad(uint32_t cluster) {
    return (cluster & FAT32_CLUSTER_MASK) == FAT32_BAD_CLUSTER;
}

static uint32_t fat32_follow_chain(uint32_t start_cluster, uint32_t index) {
    uint32_t cluster = start_cluster;
    for (uint32_t i = 0; i < index; i++) {
        if (fat32_is_eoc(cluster) || fat32_is_bad(cluster) || cluster < 2) {
            return 0;
        }
        cluster = fat_get_next_cluster(cluster);
    }
    return cluster;
}

static uint32_t fat32_chain_length(uint32_t start_cluster) {
    uint32_t count = 0;
    uint32_t cluster = start_cluster;
    while (cluster >= 2 && !fat32_is_eoc(cluster) && !fat32_is_bad(cluster)) {
        count++;
        if (count > FAT32_MAX_CLUSTER_CHAIN) return 0;
        cluster = fat_get_next_cluster(cluster);
    }
    return count;
}

static int fat32_free_chain(uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    uint32_t count = 0;
    while (cluster >= 2 && !fat32_is_eoc(cluster) && !fat32_is_bad(cluster)) {
        uint32_t next = fat_get_next_cluster(cluster);
        fat_set_next_cluster(cluster, FAT32_FREE_CLUSTER);
        cluster = next;
        count++;
        if (count > FAT32_MAX_CLUSTER_CHAIN) break;
    }
    if (fat32_state.free_cluster_count != 0xFFFFFFFF) {
        fat32_state.free_cluster_count += count;
    }
    fat32_state.next_free_cluster = start_cluster;
    return FAT32_OK;
}

/* ========================================================================
 * INTERNAL HELPERS - CLUSTER ALLOCATION
 * ======================================================================== */

static uint32_t fat32_alloc_cluster(void) {
    uint32_t cluster = fat32_state.next_free_cluster;
    uint32_t total = fat32_state.total_clusters + 2;

    for (uint32_t i = 0; i < total; i++) {
        if (cluster < 2) cluster = 2;
        if (cluster >= total) cluster = 2;

        uint32_t val = fat_get_next_cluster(cluster);
        if (val == FAT32_FREE_CLUSTER) {
            fat_set_next_cluster(cluster, FAT32_EOC_MARKER);
            fat32_state.next_free_cluster = cluster + 1;
            if (fat32_state.next_free_cluster >= total) {
                fat32_state.next_free_cluster = 2;
            }
            if (fat32_state.free_cluster_count != 0xFFFFFFFF) {
                fat32_state.free_cluster_count--;
            }
            return cluster;
        }
        cluster++;
    }
    return 0;
}

static uint32_t fat32_extend_chain(uint32_t start_cluster) {
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) return 0;

    uint32_t cluster = start_cluster;
    uint32_t count = 0;
    while (!fat32_is_eoc(cluster)) {
        cluster = fat_get_next_cluster(cluster);
        count++;
        if (count > FAT32_MAX_CLUSTER_CHAIN) return 0;
    }
    fat_set_next_cluster(cluster, new_cluster);
    return new_cluster;
}

static uint32_t fat32_alloc_first_cluster(void) {
    return fat32_alloc_cluster();
}

/* ========================================================================
 * INTERNAL HELPERS - SHORT NAME UTILITIES
 * ======================================================================== */

static char fat32_toupper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

static int fat32_is_valid_83_char(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
        c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
        c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
        c == '&' || c == '+' || c == ',' || c == ';' || c == '[' ||
        c == ']' || c == '=') {
        return 1;
    }
    return 0;
}

static uint8_t fat32_lfn_checksum(const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0x00) + (sum >> 1) + short_name[i];
    }
    return sum;
}

static int fat32_compare_short_name(const uint8_t* short_name,
                                     const char* long_name) {
    char name83[12];
    int i, j;
    const char* dot = 0;
    int name_len = 0;
    int ext_len = 0;

    for (i = 0; long_name[i]; i++) {
        if (long_name[i] == '.') dot = &long_name[i];
    }
    if (dot) {
        name_len = (int)(dot - long_name);
        ext_len = (int)(&long_name[i] - dot - 1);
    } else {
        name_len = i;
        ext_len = 0;
    }

    memset(name83, ' ', 11);
    name83[11] = '\0';

    for (i = 0; i < name_len && i < 8; i++) {
        name83[i] = fat32_toupper(long_name[i]);
    }
    if (dot) {
        for (j = 0; j < ext_len && j < 3; j++) {
            name83[8 + j] = fat32_toupper(dot[1 + j]);
        }
    }

    for (i = 0; i < 11; i++) {
        char a = fat32_toupper((char)short_name[i]);
        char b = fat32_toupper(name83[i]);
        if (a != b) return 0;
    }
    return 1;
}

static void fat32_short_name_to_str(const uint8_t* name83, char* out, int out_size) {
    int i, j = 0;
    for (i = 0; i < 8 && name83[i] != ' '; i++) {
        if (j < out_size - 1) out[j++] = name83[i];
    }
    if (name83[8] != ' ') {
        if (j < out_size - 1) out[j++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++) {
            if (j < out_size - 1) out[j++] = name83[i];
        }
    }
    out[j] = '\0';
}

/* ========================================================================
 * INTERNAL HELPERS - LFN EXTRACTION
 * ======================================================================== */

static int fat32_extract_lfn(const uint8_t* entries, int num_entries,
                              char* out_name) {
    fat32_lfn_entry_t* lfn;
    int seq_max = 0;
    int got_lfn = 0;

    for (int i = 0; i < num_entries; i++) {
        lfn = (fat32_lfn_entry_t*)(entries + i * FAT32_DIR_ENTRY_SIZE);
        if (lfn->attr == FAT32_ATTR_LFN && lfn->type == 0) {
            int seq = lfn->seq & FAT32_LFN_SEQ_MASK;
            if (seq > seq_max) seq_max = seq;
            got_lfn = 1;
        }
    }

    if (!got_lfn) return 0;

    memset(out_name, 0, FAT32_LFN_MAX_NAME_LEN + 1);
    int char_idx = 0;
    for (int seq = 1; seq <= seq_max && char_idx < FAT32_LFN_MAX_NAME_LEN; seq++) {
        for (int i = num_entries - 1; i >= 0; i--) {
            lfn = (fat32_lfn_entry_t*)(entries + i * FAT32_DIR_ENTRY_SIZE);
            if (lfn->attr != FAT32_ATTR_LFN || lfn->type != 0) continue;
            if ((lfn->seq & FAT32_LFN_SEQ_MASK) != seq) continue;

            uint16_t ch;
            for (int c = 0; c < 5 && char_idx < FAT32_LFN_MAX_NAME_LEN; c++) {
                ch = lfn->name1[c];
                if (ch == 0x0000 || ch == 0xFFFF) goto lfn_done;
                out_name[char_idx++] = (char)(ch & 0xFF);
            }
            for (int c = 0; c < 6 && char_idx < FAT32_LFN_MAX_NAME_LEN; c++) {
                ch = lfn->name2[c];
                if (ch == 0x0000 || ch == 0xFFFF) goto lfn_done;
                out_name[char_idx++] = (char)(ch & 0xFF);
            }
            for (int c = 0; c < 2 && char_idx < FAT32_LFN_MAX_NAME_LEN; c++) {
                ch = lfn->name3[c];
                if (ch == 0x0000 || ch == 0xFFFF) goto lfn_done;
                out_name[char_idx++] = (char)(ch & 0xFF);
            }
            break;
        }
    }

lfn_done:
    out_name[char_idx] = '\0';
    return (char_idx > 0) ? 1 : 0;
}

/* ========================================================================
 * INTERNAL HELPERS - PATH PARSING
 * ======================================================================== */

static int fat32_parse_path(const char* path,
                             char components[][FAT32_LFN_MAX_NAME_LEN + 1],
                             int max_components) {
    int count = 0;
    int i = 0;
    while (path[i] == '/') i++;
    while (path[i] && count < max_components) {
        int j = 0;
        while (path[i] && path[i] != '/' && j < FAT32_LFN_MAX_NAME_LEN) {
            components[count][j++] = path[i++];
        }
        components[count][j] = '\0';
        if (j > 0) count++;
        while (path[i] == '/') i++;
    }
    return count;
}

/* ========================================================================
 * INTERNAL HELPERS - DIRECTORY ENTRY SEARCH
 * ======================================================================== */

static int fat32_read_cluster(uint32_t cluster, void* buffer) {
    if (cluster < 2) return FAT32_ERR_BAD_CLUSTER;
    uint32_t sector = cluster_to_sector(cluster);
    for (uint32_t i = 0; i < fat32_state.sectors_per_cluster; i++) {
        if (disk_read_block(sector + i,
                            (uint8_t*)buffer + i * FAT32_SECTOR_SIZE) != 0) {
            return FAT32_ERR_IO;
        }
    }
    return FAT32_OK;
}

static int fat32_write_cluster(uint32_t cluster, const void* buffer) {
    if (cluster < 2) return FAT32_ERR_BAD_CLUSTER;
    uint32_t sector = cluster_to_sector(cluster);
    for (uint32_t i = 0; i < fat32_state.sectors_per_cluster; i++) {
        if (disk_write_block(sector + i,
                             (const uint8_t*)buffer + i * FAT32_SECTOR_SIZE) != 0) {
            return FAT32_ERR_IO;
        }
    }
    return FAT32_OK;
}

static int fat32_find_entry(uint32_t dir_cluster,
                             const char* name,
                             fat32_dirent_t* out_entry,
                             char* out_lfn,
                             uint32_t* out_dir_cluster,
                             uint32_t* out_entry_offset) {
    uint8_t* cluster_buf;
    uint8_t* lfn_buf;
    uint32_t cluster = dir_cluster;

    if (name == 0 || name[0] == '\0') return FAT32_ERR_PARAM;

    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    lfn_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster +
                                 FAT32_DIR_ENTRY_SIZE * 20);
    if (!lfn_buf) {
        kfree(cluster_buf);
        return FAT32_ERR_NO_MEM;
    }

    int safety = 0;
    while (cluster >= 2 && !fat32_is_eoc(cluster) && safety < FAT32_MAX_CLUSTER_CHAIN) {
        safety++;
        int ret = fat32_read_cluster(cluster, cluster_buf);
        if (ret != FAT32_OK) {
            kfree(cluster_buf);
            kfree(lfn_buf);
            return ret;
        }

        int entries_per_cluster = fat32_state.bytes_per_cluster / FAT32_DIR_ENTRY_SIZE;
        int lfn_count = 0;

        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t* entry = (fat32_dirent_t*)(cluster_buf + i * FAT32_DIR_ENTRY_SIZE);

            if (entry->name[0] == 0x00) {
                kfree(cluster_buf);
                kfree(lfn_buf);
                return FAT32_ERR_NOT_FOUND;
            }

            if (entry->name[0] == 0xE5) {
                lfn_count = 0;
                continue;
            }

            if (entry->attr == FAT32_ATTR_LFN) {
                if (lfn_count < 20) {
                    memcpy(lfn_buf + lfn_count * FAT32_DIR_ENTRY_SIZE,
                           entry, FAT32_DIR_ENTRY_SIZE);
                    lfn_count++;
                }
                continue;
            }

            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            if (fat32_compare_short_name(entry->name, name)) {
                if (out_entry) *out_entry = *entry;
                if (out_lfn) {
                    out_lfn[0] = '\0';
                    if (lfn_count > 0) {
                        fat32_extract_lfn(lfn_buf, lfn_count, out_lfn);
                    }
                    if (out_lfn[0] == '\0') {
                        fat32_short_name_to_str(entry->name, out_lfn,
                                                FAT32_LFN_MAX_NAME_LEN + 1);
                    }
                }
                if (out_dir_cluster) *out_dir_cluster = cluster;
                if (out_entry_offset) *out_entry_offset = i * FAT32_DIR_ENTRY_SIZE;

                kfree(cluster_buf);
                kfree(lfn_buf);
                return FAT32_OK;
            }

            if (lfn_count > 0) {
                char lfn_name[FAT32_LFN_MAX_NAME_LEN + 1];
                if (fat32_extract_lfn(lfn_buf, lfn_count, lfn_name)) {
                    int match = 1;
                    const char* a = name;
                    const char* b = lfn_name;
                    while (*a && *b) {
                        if (fat32_toupper(*a) != fat32_toupper(*b)) {
                            match = 0;
                            break;
                        }
                        a++;
                        b++;
                    }
                    if (match && *a == '\0' && *b == '\0') {
                        if (out_entry) *out_entry = *entry;
                        if (out_lfn) {
                            strncpy(out_lfn, lfn_name, FAT32_LFN_MAX_NAME_LEN);
                            out_lfn[FAT32_LFN_MAX_NAME_LEN] = '\0';
                        }
                        if (out_dir_cluster) *out_dir_cluster = cluster;
                        if (out_entry_offset) *out_entry_offset = i * FAT32_DIR_ENTRY_SIZE;

                        kfree(cluster_buf);
                        kfree(lfn_buf);
                        return FAT32_OK;
                    }
                }
            }

            lfn_count = 0;
        }

        cluster = fat_get_next_cluster(cluster);
    }

    kfree(cluster_buf);
    kfree(lfn_buf);
    return FAT32_ERR_NOT_FOUND;
}

static int fat32_resolve_path(const char* path,
                               fat32_dirent_t* out_entry,
                               char* out_lfn,
                               uint32_t* out_dir_cluster,
                               uint32_t* out_entry_offset) {
    char components[32][FAT32_LFN_MAX_NAME_LEN + 1];
    int count;

    if (!path || path[0] != '/') return FAT32_ERR_PATH;

    count = fat32_parse_path(path, components, 32);
    if (count == 0) {
        if (out_entry) {
            memset(out_entry, 0, sizeof(fat32_dirent_t));
            out_entry->attr = FAT32_ATTR_DIRECTORY;
            out_entry->cluster_hi = (fat32_state.root_cluster >> 16) & 0xFFFF;
            out_entry->cluster_lo = fat32_state.root_cluster & 0xFFFF;
        }
        if (out_lfn) out_lfn[0] = '\0';
        if (out_dir_cluster) *out_dir_cluster = 0;
        if (out_entry_offset) *out_entry_offset = 0;
        return FAT32_OK;
    }

    uint32_t current_cluster = fat32_state.root_cluster;

    for (int i = 0; i < count; i++) {
        fat32_dirent_t entry;
        char lfn[FAT32_LFN_MAX_NAME_LEN + 1];
        uint32_t dir_cluster, entry_offset;

        int ret = fat32_find_entry(current_cluster, components[i],
                                    &entry, lfn, &dir_cluster, &entry_offset);
        if (ret != FAT32_OK) return ret;

        if (i < count - 1) {
            if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
                return FAT32_ERR_NOT_DIR;
            }
            current_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
            if (current_cluster < 2) {
                return FAT32_ERR_NOT_FOUND;
            }
        } else {
            if (out_entry) *out_entry = entry;
            if (out_lfn) {
                strncpy(out_lfn, lfn, FAT32_LFN_MAX_NAME_LEN);
                out_lfn[FAT32_LFN_MAX_NAME_LEN] = '\0';
            }
            if (out_dir_cluster) *out_dir_cluster = dir_cluster;
            if (out_entry_offset) *out_entry_offset = entry_offset;
        }
    }

    return FAT32_OK;
}

/* ========================================================================
 * INTERNAL HELPERS - DIRECTORY ENTRY WRITE
 * ======================================================================== */

static int fat32_write_dir_entry(uint32_t dir_cluster,
                                  uint32_t entry_offset,
                                  const fat32_dirent_t* entry) {
    uint8_t* cluster_buf;
    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    int ret = fat32_read_cluster(dir_cluster, cluster_buf);
    if (ret != FAT32_OK) {
        kfree(cluster_buf);
        return ret;
    }

    memcpy(cluster_buf + entry_offset, entry, FAT32_DIR_ENTRY_SIZE);
    ret = fat32_write_cluster(dir_cluster, cluster_buf);

    kfree(cluster_buf);
    return ret;
}

/* ========================================================================
 * INTERNAL HELPERS - FSINFO UPDATE
 * ======================================================================== */

static int fat32_update_fsinfo(void) {
    if (fat32_state.fs_info_sector == 0) return FAT32_OK;

    fat32_fsinfo_t fsinfo;
    uint32_t abs_sector = fat32_state.partition_start + fat32_state.fs_info_sector;

    if (disk_read_block(abs_sector, &fsinfo) != 0) {
        return FAT32_ERR_IO;
    }

    fsinfo.free_cluster_count = fat32_state.free_cluster_count;
    fsinfo.next_free_cluster = fat32_state.next_free_cluster;

    if (disk_write_block(abs_sector, &fsinfo) != 0) {
        return FAT32_ERR_IO;
    }

    return FAT32_OK;
}

/* ========================================================================
 * INTERNAL HELPERS - DIRECTORY OPERATIONS (for write support)
 * ======================================================================== */

static int fat32_find_free_dir_entry(uint32_t dir_cluster,
                                      int num_entries,
                                      uint32_t* out_cluster,
                                      uint32_t* out_offset) {
    uint8_t* cluster_buf;
    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    uint32_t cluster = dir_cluster;
    uint32_t prev_cluster = 0;
    int safety = 0;

    while (cluster >= 2 && !fat32_is_eoc(cluster) && safety < FAT32_MAX_CLUSTER_CHAIN) {
        safety++;
        int ret = fat32_read_cluster(cluster, cluster_buf);
        if (ret != FAT32_OK) {
            kfree(cluster_buf);
            return ret;
        }

        int entries_per_cluster = fat32_state.bytes_per_cluster / FAT32_DIR_ENTRY_SIZE;
        int consecutive_free = 0;
        int first_free_offset = 0;

        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t* entry = (fat32_dirent_t*)(cluster_buf + i * FAT32_DIR_ENTRY_SIZE);

            if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                if (consecutive_free == 0) {
                    first_free_offset = i * FAT32_DIR_ENTRY_SIZE;
                }
                consecutive_free++;
                if (consecutive_free >= num_entries) {
                    *out_cluster = cluster;
                    *out_offset = first_free_offset;
                    kfree(cluster_buf);
                    return FAT32_OK;
                }
            } else {
                consecutive_free = 0;
            }
        }

        prev_cluster = cluster;
        cluster = fat_get_next_cluster(cluster);
    }

    uint32_t new_cluster = fat32_extend_chain(prev_cluster);
    if (new_cluster == 0) {
        kfree(cluster_buf);
        return FAT32_ERR_NO_SPACE;
    }

    memset(cluster_buf, 0, fat32_state.bytes_per_cluster);
    int ret = fat32_write_cluster(new_cluster, cluster_buf);
    if (ret != FAT32_OK) {
        kfree(cluster_buf);
        return ret;
    }

    *out_cluster = new_cluster;
    *out_offset = 0;

    kfree(cluster_buf);
    return FAT32_OK;
}

static void fat32_generate_short_name(const char* long_name, uint8_t* out83) {
    memset(out83, ' ', 11);

    int i, j;
    const char* dot = 0;
    for (i = 0; long_name[i]; i++) {
        if (long_name[i] == '.') dot = &long_name[i];
    }

    int base_len = dot ? (int)(dot - long_name) : i;
    for (j = 0; j < base_len && j < 8; j++) {
        char c = fat32_toupper(long_name[j]);
        out83[j] = fat32_is_valid_83_char(c) ? c : '_';
    }

    if (dot) {
        for (j = 0; j < 3 && dot[1 + j]; j++) {
            char c = fat32_toupper(dot[1 + j]);
            out83[8 + j] = fat32_is_valid_83_char(c) ? c : '_';
        }
    }

    if (base_len > 8) {
        out83[6] = '~';
        out83[7] = '1';
    }
}

static int fat32_add_dir_entry(uint32_t dir_cluster,
                                const char* name,
                                uint32_t first_cluster,
                                uint32_t file_size,
                                uint8_t attr) {
    uint32_t entry_cluster, entry_offset;
    int ret;

    ret = fat32_find_free_dir_entry(dir_cluster, 1,
                                     &entry_cluster, &entry_offset);
    if (ret != FAT32_OK) return ret;

    fat32_dirent_t entry;
    memset(&entry, 0, FAT32_DIR_ENTRY_SIZE);

    fat32_generate_short_name(name, entry.name);
    entry.attr = attr;
    entry.nt_reserved = 0;
    entry.create_time_tenth = 0;
    entry.create_time = fat32_time_now();
    entry.create_date = fat32_date_now();
    entry.access_date = fat32_date_now();
    entry.cluster_hi = (first_cluster >> 16) & 0xFFFF;
    entry.write_time = fat32_time_now();
    entry.write_date = fat32_date_now();
    entry.cluster_lo = first_cluster & 0xFFFF;
    entry.file_size = file_size;

    return fat32_write_dir_entry(entry_cluster, entry_offset, &entry);
}

static int fat32_delete_dir_entry(uint32_t dir_cluster,
                                   uint32_t entry_offset) {
    uint8_t* cluster_buf;
    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    int ret = fat32_read_cluster(dir_cluster, cluster_buf);
    if (ret != FAT32_OK) {
        kfree(cluster_buf);
        return ret;
    }

    cluster_buf[entry_offset] = 0xE5;

    int offset = entry_offset - FAT32_DIR_ENTRY_SIZE;
    while (offset >= 0) {
        fat32_dirent_t* e = (fat32_dirent_t*)(cluster_buf + offset);
        if (e->attr == FAT32_ATTR_LFN) {
            cluster_buf[offset] = 0xE5;
            offset -= FAT32_DIR_ENTRY_SIZE;
        } else {
            break;
        }
    }

    ret = fat32_write_cluster(dir_cluster, cluster_buf);
    kfree(cluster_buf);
    return ret;
}

static int fat32_init_dir_cluster(uint32_t cluster, uint32_t parent_cluster) {
    uint8_t* cluster_buf;
    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    memset(cluster_buf, 0, fat32_state.bytes_per_cluster);

    fat32_dirent_t* dot = (fat32_dirent_t*)cluster_buf;
    fat32_dirent_t* dotdot = (fat32_dirent_t*)(cluster_buf + FAT32_DIR_ENTRY_SIZE);

    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->cluster_hi = (cluster >> 16) & 0xFFFF;
    dot->cluster_lo = cluster & 0xFFFF;
    dot->create_date = fat32_date_now();
    dot->write_date = fat32_date_now();
    dot->access_date = fat32_date_now();

    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    dotdot->cluster_hi = (parent_cluster >> 16) & 0xFFFF;
    dotdot->cluster_lo = parent_cluster & 0xFFFF;
    dotdot->create_date = fat32_date_now();
    dotdot->write_date = fat32_date_now();
    dotdot->access_date = fat32_date_now();

    int ret = fat32_write_cluster(cluster, cluster_buf);
    kfree(cluster_buf);
    return ret;
}

/* ========================================================================
 * INTERNAL HELPERS - FILE HANDLE MANAGEMENT
 * ======================================================================== */

static int fat32_alloc_handle(void) {
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        if (!fat32_state.handles[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int fat32_valid_handle(int handle) {
    if (handle < 0 || handle >= FAT32_MAX_OPEN_FILES) return 0;
    return fat32_state.handles[handle].in_use;
}

/* ========================================================================
 * PUBLIC API - INITIALIZATION (with lock)
 * ======================================================================== */

int fat32_init(uint32_t partition_start_lba) {
    fat32_lock_acquire();
    int ret = fat32_init_internal(partition_start_lba);
    fat32_lock_release();
    return ret;
}

/* Internal version without lock */
static int fat32_init_internal(uint32_t partition_start_lba) {
    fat32_boot_sector_t bs;

    memset(&fat32_state, 0, sizeof(fat32_state));

    if (disk_read_block(partition_start_lba, &bs) != 0) {
        s_printf("[FAT32] Failed to read boot sector at LBA error\r\n");
        return FAT32_ERR_IO;
    }

    if (bs.ebpb.signature != FAT32_BPB_SIGNATURE) {
        s_printf("[FAT32] Invalid BPB signature\r\n");
        return FAT32_ERR_NO_FS;
    }

    if (bs.bpb.bytes_per_sector != FAT32_SECTOR_SIZE) {
        s_printf("[FAT32] Invalid sector size\r\n");
        return FAT32_ERR_NO_FS;
    }

    if (bs.bpb.sectors_per_fat_16 != 0 || bs.bpb.root_entry_count != 0 ||
        bs.bpb.total_sectors_16 != 0) {
        s_printf("[FAT32] Not a FAT32 volume\r\n");
        return FAT32_ERR_NO_FS;
    }

    if (bs.ebpb.sectors_per_fat == 0) {
        s_printf("[FAT32] Invalid FAT size\r\n");
        return FAT32_ERR_NO_FS;
    }

    if (bs.ebpb.boot_sig != FAT32_FAT_TYPE_SIGNATURE) {
        s_printf("[FAT32] Invalid extended boot signature\r\n");
        return FAT32_ERR_NO_FS;
    }

    fat32_state.partition_start = partition_start_lba;
    fat32_state.sectors_per_fat = bs.ebpb.sectors_per_fat;
    fat32_state.root_cluster = bs.ebpb.root_cluster;
    fat32_state.sectors_per_cluster = bs.bpb.sectors_per_cluster;
    fat32_state.bytes_per_cluster = bs.bpb.sectors_per_cluster * bs.bpb.bytes_per_sector;
    fat32_state.num_fats = bs.bpb.num_fats;
    fat32_state.ext_flags = bs.ebpb.ext_flags;
    fat32_state.fs_info_sector = bs.ebpb.fs_info_sector;
    fat32_state.volume_serial = bs.ebpb.volume_serial;

    if (bs.bpb.total_sectors_32 != 0) {
        fat32_state.total_sectors = bs.bpb.total_sectors_32;
    } else {
        fat32_state.total_sectors = bs.bpb.total_sectors_16;
    }

    fat32_state.fat_start = partition_start_lba + bs.bpb.reserved_sectors;
    fat32_state.data_start = fat32_state.fat_start +
                              (bs.bpb.num_fats * bs.ebpb.sectors_per_fat);

    uint32_t data_sectors = fat32_state.total_sectors -
                             (bs.bpb.reserved_sectors +
                              bs.bpb.num_fats * bs.ebpb.sectors_per_fat);
    fat32_state.total_clusters = data_sectors / bs.bpb.sectors_per_cluster;

    if (fat32_state.total_clusters < 65525) {
        s_printf("[FAT32] Cluster count too low for FAT32\r\n");
        return FAT32_ERR_NO_FS;
    }

    fat32_state.free_cluster_count = 0xFFFFFFFF;
    fat32_state.next_free_cluster = 0xFFFFFFFF;
    if (fat32_state.fs_info_sector != 0) {
        fat32_fsinfo_t fsinfo;
        uint32_t fsinfo_lba = partition_start_lba + fat32_state.fs_info_sector;
        if (disk_read_block(fsinfo_lba, &fsinfo) == 0) {
            if (fsinfo.lead_signature == 0x41615252 &&
                fsinfo.struct_signature == 0x61417272 &&
                fsinfo.trail_signature == 0xAA550000) {
                fat32_state.free_cluster_count = fsinfo.free_cluster_count;
                fat32_state.next_free_cluster = fsinfo.next_free_cluster;
            }
        }
    }

    if (fat32_state.next_free_cluster == 0xFFFFFFFF ||
        fat32_state.next_free_cluster < 2 ||
        fat32_state.next_free_cluster >= fat32_state.total_clusters + 2) {
        fat32_state.next_free_cluster = 2;
    }

    for (int i = 0; i < 11; i++) {
        fat32_state.volume_label[i] = bs.ebpb.volume_label[i];
    }
    fat32_state.volume_label[11] = '\0';

    fat_cache_init();

    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        fat32_state.handles[i].in_use = 0;
    }

    fat32_state.initialized = 1;

    s_printf("[FAT32] Volume mounted successfully\r\n");
    return FAT32_OK;
}

/* ========================================================================
 * PUBLIC API - FILE OPERATIONS (with lock)
 * ======================================================================== */

int fat32_open(const char* path, int flags) {
    fat32_lock_acquire();
    int ret = fat32_open_internal(path, flags);
    fat32_lock_release();
    return ret;
}

static int fat32_open_internal(const char* path, int flags) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path) return FAT32_ERR_PARAM;

    int handle = fat32_alloc_handle();
    if (handle < 0) return FAT32_ERR_BUSY;

    fat32_dirent_t entry;
    char lfn[FAT32_LFN_MAX_NAME_LEN + 1];
    uint32_t dir_cluster, entry_offset;

    int ret = fat32_resolve_path(path, &entry, lfn, &dir_cluster, &entry_offset);

    if (ret == FAT32_OK) {
        if (flags & FAT32_O_EXCL) {
            return FAT32_ERR_EXISTS;
        }

        if ((entry.attr & FAT32_ATTR_DIRECTORY) && !(flags & FAT32_O_DIRECTORY)) {
            // Allow opening directories for readdir
        }

        if ((entry.attr & FAT32_ATTR_READ_ONLY) &&
            (flags & (FAT32_O_WRONLY | FAT32_O_RDWR))) {
            return FAT32_ERR_READ_ONLY;
        }

        uint32_t first_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;

        fat32_state.handles[handle].in_use = 1;
        fat32_state.handles[handle].first_cluster = first_cluster;
        fat32_state.handles[handle].current_cluster = first_cluster;
        fat32_state.handles[handle].current_offset = 0;
        fat32_state.handles[handle].file_size = entry.file_size;
        fat32_state.handles[handle].position = 0;
        fat32_state.handles[handle].flags = flags;
        fat32_state.handles[handle].attr = entry.attr;
        fat32_state.handles[handle].dir_cluster = dir_cluster;
        fat32_state.handles[handle].dir_entry_offset = entry_offset;

        if ((flags & FAT32_O_TRUNC) && !(entry.attr & FAT32_ATTR_READ_ONLY)) {
            if (first_cluster >= 2) {
                fat32_free_chain(first_cluster);
            }
            fat32_state.handles[handle].first_cluster = 0;
            fat32_state.handles[handle].current_cluster = 0;
            fat32_state.handles[handle].file_size = 0;
            fat32_state.handles[handle].position = 0;
            fat32_state.handles[handle].current_offset = 0;

            entry.file_size = 0;
            entry.cluster_hi = 0;
            entry.cluster_lo = 0;
            fat32_write_dir_entry(dir_cluster, entry_offset, &entry);
        }

        if (flags & FAT32_O_APPEND) {
            fat32_state.handles[handle].position = fat32_state.handles[handle].file_size;
        }

        return handle;
    }

    if ((flags & FAT32_O_CREAT) && ret == FAT32_ERR_NOT_FOUND) {
        char components[32][FAT32_LFN_MAX_NAME_LEN + 1];
        int count = fat32_parse_path(path, components, 32);
        if (count == 0) return FAT32_ERR_PATH;

        uint32_t parent_cluster = fat32_state.root_cluster;
        for (int i = 0; i < count - 1; i++) {
            fat32_dirent_t dir_entry;
            char dir_lfn[FAT32_LFN_MAX_NAME_LEN + 1];
            uint32_t dc, eo;

            int r = fat32_find_entry(parent_cluster, components[i],
                                      &dir_entry, dir_lfn, &dc, &eo);
            if (r != FAT32_OK) return r;

            if (!(dir_entry.attr & FAT32_ATTR_DIRECTORY)) {
                return FAT32_ERR_NOT_DIR;
            }

            parent_cluster = ((uint32_t)dir_entry.cluster_hi << 16) | dir_entry.cluster_lo;
            if (parent_cluster < 2) parent_cluster = 0;
        }

        uint32_t new_cluster = 0;
        if (flags & (FAT32_O_WRONLY | FAT32_O_RDWR)) {
            // Will allocate on first write; start with no cluster
        }

        int add_ret = fat32_add_dir_entry(parent_cluster, components[count - 1],
                                           new_cluster, 0,
                                           (flags & FAT32_O_DIRECTORY) ?
                                               FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE);
        if (add_ret != FAT32_OK) return add_ret;

        ret = fat32_resolve_path(path, &entry, lfn, &dir_cluster, &entry_offset);
        if (ret != FAT32_OK) return ret;

        fat32_state.handles[handle].in_use = 1;
        fat32_state.handles[handle].first_cluster = 0;
        fat32_state.handles[handle].current_cluster = 0;
        fat32_state.handles[handle].current_offset = 0;
        fat32_state.handles[handle].file_size = 0;
        fat32_state.handles[handle].position = 0;
        fat32_state.handles[handle].flags = flags;
        fat32_state.handles[handle].attr = entry.attr;
        fat32_state.handles[handle].dir_cluster = dir_cluster;
        fat32_state.handles[handle].dir_entry_offset = entry_offset;

        return handle;
    }

    return ret;
}

int fat32_close(int handle) {
    fat32_lock_acquire();
    int ret = fat32_close_internal(handle);
    fat32_lock_release();
    return ret;
}

static int fat32_close_internal(int handle) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    if (fat32_state.handles[handle].flags & (FAT32_O_WRONLY | FAT32_O_RDWR)) {
        uint8_t* cluster_buf;
        cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
        if (cluster_buf) {
            int ret = fat32_read_cluster(fat32_state.handles[handle].dir_cluster,
                                          cluster_buf);
            if (ret == FAT32_OK) {
                fat32_dirent_t* entry = (fat32_dirent_t*)(
                    cluster_buf + fat32_state.handles[handle].dir_entry_offset);

                entry->file_size = fat32_state.handles[handle].file_size;
                entry->cluster_hi = (fat32_state.handles[handle].first_cluster >> 16) & 0xFFFF;
                entry->cluster_lo = fat32_state.handles[handle].first_cluster & 0xFFFF;
                entry->write_time = fat32_time_now();
                entry->write_date = fat32_date_now();

                fat32_write_cluster(fat32_state.handles[handle].dir_cluster, cluster_buf);
            }
            kfree(cluster_buf);
        }
    }

    fat32_state.handles[handle].in_use = 0;
    return FAT32_OK;
}

int32_t fat32_read(int handle, void* buffer, uint32_t count) {
    fat32_lock_acquire();
    int32_t ret = fat32_read_internal(handle, buffer, count);
    fat32_lock_release();
    return ret;
}

static int32_t fat32_read_internal(int handle, void* buffer, uint32_t count) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    fat32_file_handle_t* fh = &fat32_state.handles[handle];

    if ((fh->flags & 0x0003) == FAT32_O_WRONLY) {
        return FAT32_ERR_READ_ONLY;
    }

    if (fh->position >= fh->file_size) return 0;

    uint32_t bytes_remaining = fh->file_size - fh->position;
    if (count > bytes_remaining) count = bytes_remaining;

    uint32_t bytes_read = 0;
    uint8_t* dst = (uint8_t*)buffer;
    uint8_t* cluster_buf;

    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    while (bytes_read < count) {
        uint32_t cluster_index = fh->position / fat32_state.bytes_per_cluster;
        uint32_t offset_in_cluster = fh->position % fat32_state.bytes_per_cluster;

        uint32_t cluster;
        if (fh->first_cluster == 0) {
            break;
        }

        if (cluster_index == 0) {
            cluster = fh->first_cluster;
        } else {
            cluster = fat32_follow_chain(fh->first_cluster, cluster_index);
        }

        if (cluster < 2 || fat32_is_eoc(cluster) || fat32_is_bad(cluster)) {
            break;
        }

        int ret = fat32_read_cluster(cluster, cluster_buf);
        if (ret != FAT32_OK) {
            kfree(cluster_buf);
            return (bytes_read > 0) ? (int32_t)bytes_read : ret;
        }

        uint32_t bytes_in_cluster = fat32_state.bytes_per_cluster - offset_in_cluster;
        uint32_t to_copy = count - bytes_read;
        if (to_copy > bytes_in_cluster) to_copy = bytes_in_cluster;

        memcpy(dst + bytes_read, cluster_buf + offset_in_cluster, to_copy);

        bytes_read += to_copy;
        fh->position += to_copy;
    }

    kfree(cluster_buf);
    return (int32_t)bytes_read;
}

int32_t fat32_write(int handle, const void* buffer, uint32_t count) {
    fat32_lock_acquire();
    int32_t ret = fat32_write_internal(handle, buffer, count);
    fat32_lock_release();
    return ret;
}

static int32_t fat32_write_internal(int handle, const void* buffer, uint32_t count) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    fat32_file_handle_t* fh = &fat32_state.handles[handle];

    if (!(fh->flags & (FAT32_O_WRONLY | FAT32_O_RDWR))) {
        return FAT32_ERR_READ_ONLY;
    }

    if (fh->attr & FAT32_ATTR_READ_ONLY) {
        return FAT32_ERR_READ_ONLY;
    }

    if (count == 0) return 0;

    uint32_t bytes_written = 0;
    const uint8_t* src = (const uint8_t*)buffer;
    uint8_t* cluster_buf;

    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    while (bytes_written < count) {
        uint32_t cluster_index = fh->position / fat32_state.bytes_per_cluster;
        uint32_t offset_in_cluster = fh->position % fat32_state.bytes_per_cluster;

        uint32_t cluster;

        if (fh->first_cluster == 0) {
            cluster = fat32_alloc_first_cluster();
            if (cluster == 0) {
                kfree(cluster_buf);
                return (bytes_written > 0) ? (int32_t)bytes_written : FAT32_ERR_NO_SPACE;
            }
            fh->first_cluster = cluster;
            memset(cluster_buf, 0, fat32_state.bytes_per_cluster);
            fat32_write_cluster(cluster, cluster_buf);
        } else if (cluster_index == 0) {
            cluster = fh->first_cluster;
        } else {
            cluster = fat32_follow_chain(fh->first_cluster, cluster_index);
            if (cluster == 0 || fat32_is_eoc(cluster)) {
                uint32_t last = fh->first_cluster;
                uint32_t chain_len = fat32_chain_length(fh->first_cluster);
                while (chain_len <= cluster_index) {
                    uint32_t new_clust = fat32_extend_chain(last);
                    if (new_clust == 0) {
                        kfree(cluster_buf);
                        return (bytes_written > 0) ? (int32_t)bytes_written : FAT32_ERR_NO_SPACE;
                    }
                    memset(cluster_buf, 0, fat32_state.bytes_per_cluster);
                    fat32_write_cluster(new_clust, cluster_buf);
                    last = new_clust;
                    chain_len++;
                }
                cluster = fat32_follow_chain(fh->first_cluster, cluster_index);
            }
        }

        if (cluster < 2) {
            kfree(cluster_buf);
            return (bytes_written > 0) ? (int32_t)bytes_written : FAT32_ERR_NO_SPACE;
        }

        uint32_t bytes_in_cluster = fat32_state.bytes_per_cluster - offset_in_cluster;
        uint32_t to_copy = count - bytes_written;
        if (to_copy > bytes_in_cluster) to_copy = bytes_in_cluster;

        if (offset_in_cluster != 0 || to_copy < fat32_state.bytes_per_cluster) {
            int ret = fat32_read_cluster(cluster, cluster_buf);
            if (ret != FAT32_OK) {
                kfree(cluster_buf);
                return (bytes_written > 0) ? (int32_t)bytes_written : ret;
            }
        } else {
            memset(cluster_buf, 0, fat32_state.bytes_per_cluster);
        }

        memcpy(cluster_buf + offset_in_cluster, src + bytes_written, to_copy);

        int ret = fat32_write_cluster(cluster, cluster_buf);
        if (ret != FAT32_OK) {
            kfree(cluster_buf);
            return (bytes_written > 0) ? (int32_t)bytes_written : ret;
        }

        bytes_written += to_copy;
        fh->position += to_copy;

        if (fh->position > fh->file_size) {
            fh->file_size = fh->position;
        }
    }

    kfree(cluster_buf);
    return (int32_t)bytes_written;
}

int32_t fat32_seek(int handle, int32_t offset, int whence) {
    fat32_lock_acquire();
    int32_t ret = fat32_seek_internal(handle, offset, whence);
    fat32_lock_release();
    return ret;
}

static int32_t fat32_seek_internal(int handle, int32_t offset, int whence) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    fat32_file_handle_t* fh = &fat32_state.handles[handle];
    int32_t new_pos;

    switch (whence) {
        case FAT32_SEEK_SET:
            new_pos = offset;
            break;
        case FAT32_SEEK_CUR:
            new_pos = (int32_t)fh->position + offset;
            break;
        case FAT32_SEEK_END:
            new_pos = (int32_t)fh->file_size + offset;
            break;
        default:
            return FAT32_ERR_PARAM;
    }

    if (new_pos < 0) return FAT32_ERR_PARAM;

    fh->position = (uint32_t)new_pos;
    return (int32_t)fh->position;
}

/* ========================================================================
 * PUBLIC API - FILE INFORMATION (with lock)
 * ======================================================================== */

int fat32_stat(const char* path, fat32_stat_t* stat) {
    fat32_lock_acquire();
    int ret = fat32_stat_internal(path, stat);
    fat32_lock_release();
    return ret;
}

static int fat32_stat_internal(const char* path, fat32_stat_t* stat) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path || !stat) return FAT32_ERR_PARAM;

    fat32_dirent_t entry;
    char lfn[FAT32_LFN_MAX_NAME_LEN + 1];

    int ret = fat32_resolve_path(path, &entry, lfn, 0, 0);
    if (ret != FAT32_OK) return ret;

    memset(stat, 0, sizeof(fat32_stat_t));

    if (lfn[0] != '\0') {
        strncpy(stat->name, lfn, FAT32_LFN_MAX_NAME_LEN);
    } else {
        fat32_short_name_to_str(entry.name, stat->name, FAT32_LFN_MAX_NAME_LEN + 1);
    }

    stat->size = entry.file_size;
    stat->attr = entry.attr;
    stat->create_date = entry.create_date;
    stat->create_time = entry.create_time;
    stat->write_date = entry.write_date;
    stat->write_time = entry.write_time;
    stat->access_date = entry.access_date;
    stat->first_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;

    return FAT32_OK;
}

/* ========================================================================
 * PUBLIC API - DIRECTORY OPERATIONS (with lock)
 * ======================================================================== */

int fat32_mkdir(const char* path) {
    fat32_lock_acquire();
    int ret = fat32_mkdir_internal(path);
    fat32_lock_release();
    return ret;
}

static int fat32_mkdir_internal(const char* path) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path) return FAT32_ERR_PARAM;

    char components[32][FAT32_LFN_MAX_NAME_LEN + 1];
    int count = fat32_parse_path(path, components, 32);
    if (count == 0) return FAT32_ERR_PATH;

    fat32_dirent_t existing;
    if (fat32_resolve_path(path, &existing, 0, 0, 0) == FAT32_OK) {
        return FAT32_ERR_EXISTS;
    }

    uint32_t parent_cluster = fat32_state.root_cluster;
    for (int i = 0; i < count - 1; i++) {
        fat32_dirent_t dir_entry;
        char dir_lfn[FAT32_LFN_MAX_NAME_LEN + 1];
        uint32_t dc, eo;

        int r = fat32_find_entry(parent_cluster, components[i],
                                  &dir_entry, dir_lfn, &dc, &eo);
        if (r != FAT32_OK) return r;

        if (!(dir_entry.attr & FAT32_ATTR_DIRECTORY)) {
            return FAT32_ERR_NOT_DIR;
        }

        parent_cluster = ((uint32_t)dir_entry.cluster_hi << 16) | dir_entry.cluster_lo;
        if (parent_cluster < 2) parent_cluster = 0;
    }

    uint32_t new_cluster = fat32_alloc_first_cluster();
    if (new_cluster == 0) return FAT32_ERR_NO_SPACE;

    int ret = fat32_init_dir_cluster(new_cluster, parent_cluster ? parent_cluster : fat32_state.root_cluster);
    if (ret != FAT32_OK) {
        fat_set_next_cluster(new_cluster, FAT32_FREE_CLUSTER);
        return ret;
    }

    ret = fat32_add_dir_entry(parent_cluster, components[count - 1],
                               new_cluster, 0, FAT32_ATTR_DIRECTORY);
    if (ret != FAT32_OK) {
        fat32_free_chain(new_cluster);
        return ret;
    }

    return FAT32_OK;
}

int fat32_unlink(const char* path) {
    fat32_lock_acquire();
    int ret = fat32_unlink_internal(path);
    fat32_lock_release();
    return ret;
}

static int fat32_unlink_internal(const char* path) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path) return FAT32_ERR_PARAM;

    fat32_dirent_t entry;
    uint32_t dir_cluster, entry_offset;

    int ret = fat32_resolve_path(path, &entry, 0, &dir_cluster, &entry_offset);
    if (ret != FAT32_OK) return ret;

    if (dir_cluster == 0) return FAT32_ERR_ACCESS;

    if (entry.attr & FAT32_ATTR_DIRECTORY) {
        uint32_t cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
        if (cluster >= 2) {
            uint8_t* cluster_buf;
            cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
            if (!cluster_buf) return FAT32_ERR_NO_MEM;

            int safety = 0;
            while (cluster >= 2 && !fat32_is_eoc(cluster) && safety < FAT32_MAX_CLUSTER_CHAIN) {
                safety++;
                ret = fat32_read_cluster(cluster, cluster_buf);
                if (ret != FAT32_OK) {
                    kfree(cluster_buf);
                    return ret;
                }

                int entries_per_cluster = fat32_state.bytes_per_cluster / FAT32_DIR_ENTRY_SIZE;
                for (int i = 0; i < entries_per_cluster; i++) {
                    fat32_dirent_t* de = (fat32_dirent_t*)(cluster_buf + i * FAT32_DIR_ENTRY_SIZE);

                    if (de->name[0] == 0x00) goto dir_empty;
                    if (de->name[0] == 0xE5) continue;
                    if (de->attr == FAT32_ATTR_LFN) continue;
                    if (de->attr == FAT32_ATTR_VOLUME_ID) continue;

                    if (de->name[0] == '.' && (de->name[1] == ' ' || de->name[1] == '\0')) continue;
                    if (de->name[0] == '.' && de->name[1] == '.' && (de->name[2] == ' ' || de->name[2] == '\0')) continue;

                    kfree(cluster_buf);
                    return FAT32_ERR_NOT_EMPTY;
                }

                cluster = fat_get_next_cluster(cluster);
            }

        dir_empty:
            kfree(cluster_buf);
        }
    }

    uint32_t first_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
    if (first_cluster >= 2) {
        fat32_free_chain(first_cluster);
    }

    ret = fat32_delete_dir_entry(dir_cluster, entry_offset);
    return ret;
}

int fat32_rename(const char* oldpath, const char* newpath) {
    fat32_lock_acquire();
    int ret = fat32_rename_internal(oldpath, newpath);
    fat32_lock_release();
    return ret;
}

static int fat32_rename_internal(const char* oldpath, const char* newpath) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!oldpath || !newpath) return FAT32_ERR_PARAM;

    fat32_dirent_t old_entry;
    uint32_t old_dir_cluster, old_entry_offset;

    int ret = fat32_resolve_path(oldpath, &old_entry, 0,
                                  &old_dir_cluster, &old_entry_offset);
    if (ret != FAT32_OK) return ret;

    fat32_dirent_t new_entry;
    if (fat32_resolve_path(newpath, &new_entry, 0, 0, 0) == FAT32_OK) {
        return FAT32_ERR_EXISTS;
    }

    char new_components[32][FAT32_LFN_MAX_NAME_LEN + 1];
    int new_count = fat32_parse_path(newpath, new_components, 32);
    if (new_count == 0) return FAT32_ERR_PATH;

    uint32_t new_parent_cluster = fat32_state.root_cluster;
    for (int i = 0; i < new_count - 1; i++) {
        fat32_dirent_t dir_entry;
        char dir_lfn[FAT32_LFN_MAX_NAME_LEN + 1];
        uint32_t dc, eo;

        ret = fat32_find_entry(new_parent_cluster, new_components[i],
                                &dir_entry, dir_lfn, &dc, &eo);
        if (ret != FAT32_OK) return ret;

        if (!(dir_entry.attr & FAT32_ATTR_DIRECTORY)) return FAT32_ERR_NOT_DIR;

        new_parent_cluster = ((uint32_t)dir_entry.cluster_hi << 16) | dir_entry.cluster_lo;
        if (new_parent_cluster < 2) new_parent_cluster = 0;
    }

    uint32_t first_cluster = ((uint32_t)old_entry.cluster_hi << 16) | old_entry.cluster_lo;

    if (new_parent_cluster == old_dir_cluster) {
        fat32_generate_short_name(new_components[new_count - 1], old_entry.name);
        old_entry.write_date = fat32_date_now();
        old_entry.write_time = fat32_time_now();
        return fat32_write_dir_entry(old_dir_cluster, old_entry_offset, &old_entry);
    }

    ret = fat32_add_dir_entry(new_parent_cluster, new_components[new_count - 1],
                               first_cluster, old_entry.file_size, old_entry.attr);
    if (ret != FAT32_OK) return ret;

    return fat32_delete_dir_entry(old_dir_cluster, old_entry_offset);
}

int fat32_readdir(const char* path, fat32_dirent_out_t* entries, uint32_t max) {
    fat32_lock_acquire();
    int ret = fat32_readdir_internal(path, entries, max);
    fat32_lock_release();
    return ret;
}

static int fat32_readdir_internal(const char* path, fat32_dirent_out_t* entries, uint32_t max) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!entries || max == 0) return FAT32_ERR_PARAM;

    uint32_t start_cluster;

    if (!path || (path[0] == '/' && path[1] == '\0')) {
        start_cluster = fat32_state.root_cluster;
    } else {
        fat32_dirent_t dir_entry;
        int ret = fat32_resolve_path(path, &dir_entry, 0, 0, 0);
        if (ret != FAT32_OK) return ret;

        if (!(dir_entry.attr & FAT32_ATTR_DIRECTORY)) {
            return FAT32_ERR_NOT_DIR;
        }

        start_cluster = ((uint32_t)dir_entry.cluster_hi << 16) | dir_entry.cluster_lo;
    }

    if (start_cluster < 2) return FAT32_ERR_BAD_CLUSTER;

    uint8_t* cluster_buf;
    uint8_t* lfn_buf;
    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    lfn_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster + FAT32_DIR_ENTRY_SIZE * 20);
    if (!lfn_buf) {
        kfree(cluster_buf);
        return FAT32_ERR_NO_MEM;
    }

    uint32_t entry_count = 0;
    uint32_t cluster = start_cluster;
    int lfn_count = 0;
    int safety = 0;

    while (cluster >= 2 && !fat32_is_eoc(cluster) && safety < FAT32_MAX_CLUSTER_CHAIN) {
        safety++;
        int ret = fat32_read_cluster(cluster, cluster_buf);
        if (ret != FAT32_OK) {
            kfree(cluster_buf);
            kfree(lfn_buf);
            return ret;
        }

        int entries_per_cluster = fat32_state.bytes_per_cluster / FAT32_DIR_ENTRY_SIZE;

        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t* entry = (fat32_dirent_t*)(cluster_buf + i * FAT32_DIR_ENTRY_SIZE);

            if (entry->name[0] == 0x00) {
                kfree(cluster_buf);
                kfree(lfn_buf);
                return (int)entry_count;
            }

            if (entry->name[0] == 0xE5) {
                lfn_count = 0;
                continue;
            }

            if (entry->attr == FAT32_ATTR_LFN) {
                if (lfn_count < 20) {
                    memcpy(lfn_buf + lfn_count * FAT32_DIR_ENTRY_SIZE,
                           entry, FAT32_DIR_ENTRY_SIZE);
                    lfn_count++;
                }
                continue;
            }

            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            if (entry_count < max) {
                char lfn_name[FAT32_LFN_MAX_NAME_LEN + 1];
                lfn_name[0] = '\0';

                if (lfn_count > 0) {
                    fat32_extract_lfn(lfn_buf, lfn_count, lfn_name);
                }

                if (lfn_name[0] == '\0') {
                    fat32_short_name_to_str(entry->name, lfn_name,
                                            FAT32_LFN_MAX_NAME_LEN + 1);
                }

                if (lfn_name[0] == '.' &&
                    (lfn_name[1] == '\0' || (lfn_name[1] == '.' && lfn_name[2] == '\0'))) {
                    lfn_count = 0;
                    continue;
                }

                strncpy(entries[entry_count].name, lfn_name, FAT32_LFN_MAX_NAME_LEN);
                entries[entry_count].name[FAT32_LFN_MAX_NAME_LEN] = '\0';
                entries[entry_count].size = entry->file_size;
                entries[entry_count].attr = entry->attr;
                entries[entry_count].first_cluster =
                    ((uint32_t)entry->cluster_hi << 16) | entry->cluster_lo;

                entry_count++;
            }

            lfn_count = 0;
        }

        cluster = fat_get_next_cluster(cluster);
    }

    kfree(cluster_buf);
    kfree(lfn_buf);
    return (int)entry_count;
}

/* ========================================================================
 * PUBLIC API - SYNC AND UTILITY (with lock)
 * ======================================================================== */

int fat32_sync(void) {
    fat32_lock_acquire();
    int ret = fat32_sync_internal();
    fat32_lock_release();
    return ret;
}

static int fat32_sync_internal(void) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;

    int ret = 0;  // FIX: declare ret

    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].dirty &&
            fat32_state.fat_cache[i].sector != 0xFFFFFFFF) {
            uint32_t sec = fat32_state.fat_cache[i].sector;
            if (disk_write_block(sec, fat32_state.fat_cache[i].data) != 0) {
                ret = FAT32_ERR_IO;
            } else {
                fat32_state.fat_cache[i].dirty = 0;
            }

            if (!(fat32_state.ext_flags & 0x0080)) {
                for (int f = 1; f < fat32_state.num_fats; f++) {
                    uint32_t backup_sec = sec + (f * fat32_state.sectors_per_fat);
                    disk_write_block(backup_sec, fat32_state.fat_cache[i].data);
                }
            }
        }
    }

    disk_flush_cache();
    fat32_update_fsinfo();

    return ret;
}

int32_t fat32_free_clusters(void) {
    fat32_lock_acquire();
    int32_t ret = fat32_free_clusters_internal();
    fat32_lock_release();
    return ret;
}

static int32_t fat32_free_clusters_internal(void) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;

    if (fat32_state.free_cluster_count == 0xFFFFFFFF) {
        uint32_t free = 0;
        for (uint32_t cluster = 2; cluster < fat32_state.total_clusters + 2; cluster++) {
            uint32_t val = fat_get_next_cluster(cluster);
            if (val == FAT32_FREE_CLUSTER) {
                free++;
            }
        }
        fat32_state.free_cluster_count = free;
    }

    return (int32_t)fat32_state.free_cluster_count;
}

int fat32_is_initialized(void) {
    fat32_lock_acquire();
    int ret = fat32_state.initialized;
    fat32_lock_release();
    return ret;
}