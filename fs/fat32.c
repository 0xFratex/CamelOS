/*
 * fat32.c - FAT32 Filesystem Driver Implementation for CamelOS
 *
 * Full FAT32 driver with read/write support, LFN reading, FAT caching,
 * and cluster chain management. Designed for interoperability with
 * USB drives, SD cards, and shared partitions.
 *
 * Reference: Microsoft FAT32 Specification (fatgen103.doc)
 */

#include "fat32.h"
#include "disk.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../core/memory.h"
#include "../hal/drivers/serial.h"

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static fat32_state_t fat32_state;

/* ========================================================================
 * INTERNAL HELPERS - FAT DATE/TIME
 * ======================================================================== */

static uint16_t fat32_date_now(void) {
    /* FIXME: Integrate with RTC. Return a fixed date for now. */
    return ((2025 - 1980) << 9) | (1 << 5) | 1;  /* 2025-01-01 */
}

static uint16_t fat32_time_now(void) {
    /* FIXME: Integrate with RTC. Return midnight for now. */
    return 0;
}

/* ========================================================================
 * INTERNAL HELPERS - CLUSTER / SECTOR CONVERSION
 * ======================================================================== */

/**
 * Convert a cluster number to the first absolute sector of that cluster.
 */
static uint32_t cluster_to_sector(uint32_t cluster) {
    if (cluster < 2) return 0;  /* Invalid cluster */
    return fat32_state.data_start +
           ((cluster - 2) * fat32_state.sectors_per_cluster);
}

/**
 * Determine which FAT sector contains the entry for a given cluster,
 * and the offset within that sector.
 */
static void fat_entry_location(uint32_t cluster,
                               uint32_t* fat_sector,
                               uint32_t* offset_in_sector) {
    /* Each FAT32 entry is 4 bytes. There are 128 entries per 512-byte sector. */
    uint32_t fat_offset = cluster * 4;
    *fat_sector = fat32_state.fat_start + (fat_offset / FAT32_SECTOR_SIZE);
    *offset_in_sector = fat_offset % FAT32_SECTOR_SIZE;
}

/* ========================================================================
 * INTERNAL HELPERS - FAT CACHE
 * ======================================================================== */

/**
 * Invalidate all FAT cache entries.
 */
static void fat_cache_init(void) {
    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        fat32_state.fat_cache[i].sector = 0xFFFFFFFF;
        fat32_state.fat_cache[i].dirty = 0;
        fat32_state.fat_cache[i].lru = 0;
        memset(fat32_state.fat_cache[i].data, 0, FAT32_SECTOR_SIZE);
    }
    fat32_state.cache_counter = 0;
}

/**
 * Look up a FAT sector in the cache. Returns cache index or -1.
 */
static int fat_cache_lookup(uint32_t sector) {
    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].sector == sector) {
            fat32_state.fat_cache[i].lru = ++fat32_state.cache_counter;
            return i;
        }
    }
    return -1;
}

/**
 * Find the least-recently-used cache slot, writing back if dirty.
 */
static int fat_cache_evict(void) {
    int victim = 0;
    int min_lru = fat32_state.fat_cache[0].lru;

    for (int i = 1; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].lru < min_lru) {
            min_lru = fat32_state.fat_cache[i].lru;
            victim = i;
        }
    }

    /* Write back if dirty */
    if (fat32_state.fat_cache[victim].dirty &&
        fat32_state.fat_cache[victim].sector != 0xFFFFFFFF) {
        uint32_t sec = fat32_state.fat_cache[victim].sector;
        disk_write_block(sec, fat32_state.fat_cache[victim].data);

        /* If FAT mirroring is enabled, write to the backup FAT(s) too */
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

    return victim;
}

/**
 * Read a FAT sector through the cache. Returns cache index.
 */
static int fat_cache_read(uint32_t sector) {
    int idx = fat_cache_lookup(sector);
    if (idx >= 0) return idx;

    /* Cache miss: evict and load */
    idx = fat_cache_evict();
    if (disk_read_block(sector, fat32_state.fat_cache[idx].data) != 0) {
        fat32_state.fat_cache[idx].sector = 0xFFFFFFFF;
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

/**
 * Get the next cluster in the chain. Returns the 28-bit value.
 */
static uint32_t fat_get_next_cluster(uint32_t cluster) {
    uint32_t fat_sector, offset;
    fat_entry_location(cluster, &fat_sector, &offset);

    int idx = fat_cache_read(fat_sector);
    if (idx < 0) return FAT32_BAD_CLUSTER;

    uint32_t entry;
    memcpy(&entry, &fat32_state.fat_cache[idx].data[offset], 4);

    /* FAT32 uses only lower 28 bits; upper 4 are reserved */
    return entry & FAT32_CLUSTER_MASK;
}

/**
 * Set the next cluster value in the FAT. Writes through cache.
 */
static int fat_set_next_cluster(uint32_t cluster, uint32_t next) {
    uint32_t fat_sector, offset;
    fat_entry_location(cluster, &fat_sector, &offset);

    int idx = fat_cache_read(fat_sector);
    if (idx < 0) return FAT32_ERR_IO;

    /* Preserve upper 4 reserved bits */
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

/**
 * Check if a cluster value marks the end of a chain.
 */
static int fat32_is_eoc(uint32_t cluster) {
    cluster &= FAT32_CLUSTER_MASK;
    return (cluster >= FAT32_EOC_MARKER_MIN && cluster <= FAT32_EOC_MARKER);
}

/**
 * Check if a cluster is free.
 */
static int fat32_is_free(uint32_t cluster) {
    return (cluster & FAT32_CLUSTER_MASK) == FAT32_FREE_CLUSTER;
}

/**
 * Check if a cluster is bad.
 */
static int fat32_is_bad(uint32_t cluster) {
    return (cluster & FAT32_CLUSTER_MASK) == FAT32_BAD_CLUSTER;
}

/**
 * Follow a cluster chain and return the Nth cluster.
 * cluster_0 is the first cluster (index 0).
 * Returns the cluster number, or 0 on error / end-of-chain.
 */
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

/**
 * Count the number of clusters in a chain.
 */
static uint32_t fat32_chain_length(uint32_t start_cluster) {
    uint32_t count = 0;
    uint32_t cluster = start_cluster;

    while (cluster >= 2 && !fat32_is_eoc(cluster) && !fat32_is_bad(cluster)) {
        count++;
        if (count > FAT32_MAX_CLUSTER_CHAIN) return 0;  /* Safety */
        cluster = fat_get_next_cluster(cluster);
    }

    return count;
}

/**
 * Free an entire cluster chain starting from start_cluster.
 * Sets each FAT entry to 0.
 */
static int fat32_free_chain(uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    uint32_t count = 0;

    while (cluster >= 2 && !fat32_is_eoc(cluster) && !fat32_is_bad(cluster)) {
        uint32_t next = fat_get_next_cluster(cluster);
        fat_set_next_cluster(cluster, FAT32_FREE_CLUSTER);
        cluster = next;
        count++;
        if (count > FAT32_MAX_CLUSTER_CHAIN) break;  /* Safety */
    }

    /* Update free cluster count */
    if (fat32_state.free_cluster_count != 0xFFFFFFFF) {
        fat32_state.free_cluster_count += count;
    }
    fat32_state.next_free_cluster = start_cluster;

    return FAT32_OK;
}

/* ========================================================================
 * INTERNAL HELPERS - CLUSTER ALLOCATION
 * ======================================================================== */

/**
 * Allocate a single free cluster. Returns cluster number or 0 on failure.
 * The allocated cluster's FAT entry is set to EOC.
 */
static uint32_t fat32_alloc_cluster(void) {
    uint32_t cluster = fat32_state.next_free_cluster;
    uint32_t total = fat32_state.total_clusters + 2;  /* Clusters start at 2 */

    /* Search from the hint position */
    for (uint32_t i = 0; i < total; i++) {
        if (cluster < 2) cluster = 2;
        if (cluster >= total) cluster = 2;

        uint32_t val = fat_get_next_cluster(cluster);
        if (val == FAT32_FREE_CLUSTER) {
            /* Found a free cluster */
            fat_set_next_cluster(cluster, FAT32_EOC_MARKER);

            /* Update hint */
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
        if (cluster >= total) cluster = 2;
    }

    /* No free clusters found */
    return 0;
}

/**
 * Extend a cluster chain by one cluster. The new cluster is linked
 * after the last cluster in the chain.
 *
 * @param start_cluster  First cluster of the chain (must be valid)
 * @return New cluster number, or 0 on failure
 */
static uint32_t fat32_extend_chain(uint32_t start_cluster) {
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) return 0;

    /* Walk to the end of the chain and link */
    uint32_t cluster = start_cluster;
    uint32_t count = 0;
    while (!fat32_is_eoc(cluster)) {
        cluster = fat_get_next_cluster(cluster);
        count++;
        if (count > FAT32_MAX_CLUSTER_CHAIN) return 0;  /* Safety */
    }

    /* cluster is now the EOC cluster; link it to new_cluster */
    fat_set_next_cluster(cluster, new_cluster);
    /* new_cluster already has EOC from fat32_alloc_cluster */

    return new_cluster;
}

/**
 * Allocate the first cluster for a new file/directory.
 * Returns the new cluster number or 0 on failure.
 */
static uint32_t fat32_alloc_first_cluster(void) {
    return fat32_alloc_cluster();
}

/* ========================================================================
 * INTERNAL HELPERS - SHORT NAME UTILITIES
 * ======================================================================== */

/**
 * Convert a character to uppercase (ASCII only).
 */
static char fat32_toupper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

/**
 * Check if a character is valid in an 8.3 short name.
 */
static int fat32_is_valid_83_char(char c) {
    /* Valid: A-Z, 0-9, and certain special chars */
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

/**
 * Calculate the checksum for an 8.3 short name (used by LFN entries).
 * Algorithm from Microsoft FAT spec.
 */
static uint8_t fat32_lfn_checksum(const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0x00) + (sum >> 1) + short_name[i];
    }
    return sum;
}

/**
 * Compare two 8.3 short names (case-insensitive).
 * short_name is the 11-byte on-disk format.
 */
static int fat32_compare_short_name(const uint8_t* short_name,
                                     const char* long_name) {
    char name83[12];
    int i, j;

    /* Extract the name part (before the dot) */
    const char* dot = 0;
    int name_len = 0;
    int ext_len = 0;

    /* Find the dot in the long name */
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

    /* Build 8.3 format name */
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

    /* Compare with the on-disk short name */
    for (i = 0; i < 11; i++) {
        char a = fat32_toupper((char)short_name[i]);
        char b = fat32_toupper(name83[i]);
        if (a != b) return 0;
    }

    return 1;
}

/**
 * Convert a short (8.3) directory entry name to a null-terminated string.
 * Removes trailing spaces and inserts a dot before the extension.
 */
static void fat32_short_name_to_str(const uint8_t* name83, char* out, int out_size) {
    int i, j = 0;

    /* Copy name part */
    for (i = 0; i < 8 && name83[i] != ' '; i++) {
        if (j < out_size - 1) {
            out[j++] = name83[i];
        }
    }

    /* Copy extension part */
    if (name83[8] != ' ') {
        if (j < out_size - 1) out[j++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++) {
            if (j < out_size - 1) {
                out[j++] = name83[i];
            }
        }
    }

    out[j] = '\0';
}

/* ========================================================================
 * INTERNAL HELPERS - LFN EXTRACTION
 * ======================================================================== */

/**
 * Extract the long file name from a sequence of LFN directory entries.
 *
 * @param entries      Array of raw 32-byte directory entries
 * @param num_entries  Number of entries in the array
 * @param out_name     Output buffer (at least FAT32_LFN_MAX_NAME_LEN + 1)
 * @return 1 if LFN was extracted, 0 if no valid LFN
 */
static int fat32_extract_lfn(const uint8_t* entries, int num_entries,
                              char* out_name) {
    fat32_lfn_entry_t* lfn;
    int seq_max = 0;
    int got_lfn = 0;

    /* First pass: find the highest sequence number */
    for (int i = 0; i < num_entries; i++) {
        lfn = (fat32_lfn_entry_t*)(entries + i * FAT32_DIR_ENTRY_SIZE);
        if (lfn->attr == FAT32_ATTR_LFN && lfn->type == 0) {
            int seq = lfn->seq & FAT32_LFN_SEQ_MASK;
            if (seq > seq_max) seq_max = seq;
            got_lfn = 1;
        }
    }

    if (!got_lfn) return 0;

    /* Clear output */
    memset(out_name, 0, FAT32_LFN_MAX_NAME_LEN + 1);

    /* Second pass: assemble the name in order */
    int char_idx = 0;
    for (int seq = 1; seq <= seq_max && char_idx < FAT32_LFN_MAX_NAME_LEN; seq++) {
        /* Find the entry with this sequence number (scan in reverse to get
           the entry closest to the short name entry) */
        for (int i = num_entries - 1; i >= 0; i--) {
            lfn = (fat32_lfn_entry_t*)(entries + i * FAT32_DIR_ENTRY_SIZE);
            if (lfn->attr != FAT32_ATTR_LFN || lfn->type != 0) continue;
            if ((lfn->seq & FAT32_LFN_SEQ_MASK) != seq) continue;

            /* Extract characters from name1 (5), name2 (6), name3 (2) = 13 total */
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

/**
 * Parse a path into components separated by '/'.
 * Returns the number of components found.
 * Components are stored in the provided array.
 *
 * E.g. "/dir/subdir/file.txt" => ["dir", "subdir", "file.txt"] => 3
 */
static int fat32_parse_path(const char* path,
                             char components[][FAT32_LFN_MAX_NAME_LEN + 1],
                             int max_components) {
    int count = 0;
    int i = 0;

    /* Skip leading slashes */
    while (path[i] == '/') i++;

    while (path[i] && count < max_components) {
        int j = 0;
        while (path[i] && path[i] != '/' && j < FAT32_LFN_MAX_NAME_LEN) {
            components[count][j++] = path[i++];
        }
        components[count][j] = '\0';
        if (j > 0) count++;

        /* Skip slashes */
        while (path[i] == '/') i++;
    }

    return count;
}

/* ========================================================================
 * INTERNAL HELPERS - DIRECTORY ENTRY SEARCH
 * ======================================================================== */

/**
 * Read a cluster's data into a buffer.
 * The buffer must be at least bytes_per_cluster bytes.
 */
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

/**
 * Write a cluster's data from a buffer.
 */
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

/**
 * Find a directory entry by name within a directory's cluster chain.
 *
 * @param dir_cluster   Starting cluster of the directory
 * @param name          Name to search for (case-insensitive)
 * @param out_entry     Output: the found directory entry
 * @param out_lfn       Output: long file name (if any)
 * @param out_dir_cluster    Output: cluster containing the entry
 * @param out_entry_offset   Output: byte offset within cluster
 * @return FAT32_OK on success, FAT32_ERR_NOT_FOUND if not found
 */
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

            /* End of directory */
            if (entry->name[0] == 0x00) {
                kfree(cluster_buf);
                kfree(lfn_buf);
                return FAT32_ERR_NOT_FOUND;
            }

            /* Deleted entry */
            if (entry->name[0] == 0xE5) {
                lfn_count = 0;
                continue;
            }

            /* LFN entry */
            if (entry->attr == FAT32_ATTR_LFN) {
                /* Accumulate LFN entries */
                if (lfn_count < 20) {
                    memcpy(lfn_buf + lfn_count * FAT32_DIR_ENTRY_SIZE,
                           entry, FAT32_DIR_ENTRY_SIZE);
                    lfn_count++;
                }
                continue;
            }

            /* Short name entry - check if it matches */
            /* Skip volume ID entries */
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            /* Try to match */
            if (fat32_compare_short_name(entry->name, name)) {
                /* Found it! */
                if (out_entry) *out_entry = *entry;

                /* Extract LFN if we have one */
                if (out_lfn) {
                    out_lfn[0] = '\0';
                    if (lfn_count > 0) {
                        fat32_extract_lfn(lfn_buf, lfn_count, out_lfn);
                    }
                    if (out_lfn[0] == '\0') {
                        /* No LFN, use short name */
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

            /* Also try matching the LFN against the search name */
            if (lfn_count > 0) {
                char lfn_name[FAT32_LFN_MAX_NAME_LEN + 1];
                if (fat32_extract_lfn(lfn_buf, lfn_count, lfn_name)) {
                    /* Case-insensitive comparison */
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

        /* Move to next cluster */
        cluster = fat_get_next_cluster(cluster);
    }

    kfree(cluster_buf);
    kfree(lfn_buf);
    return FAT32_ERR_NOT_FOUND;
}

/**
 * Resolve a full path to a directory entry.
 *
 * @param path          Absolute path (e.g. "/dir/subdir/file.txt")
 * @param out_entry     Output directory entry
 * @param out_lfn       Output long file name
 * @param out_dir_cluster    Output: cluster of parent directory
 * @param out_entry_offset   Output: offset within parent cluster
 * @return FAT32_OK on success
 */
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
        /* Root directory - special case */
        if (out_entry) {
            memset(out_entry, 0, sizeof(fat32_dirent_t));
            out_entry->attr = FAT32_ATTR_DIRECTORY;
            out_entry->cluster_hi = (fat32_state.root_cluster >> 16) & 0xFFFF;
            out_entry->cluster_lo = fat32_state.root_cluster & 0xFFFF;
        }
        if (out_lfn) out_lfn[0] = '\0';
        if (out_dir_cluster) *out_dir_cluster = 0;  /* Root has no parent dir entry */
        if (out_entry_offset) *out_entry_offset = 0;
        return FAT32_OK;
    }

    /* Walk the path from root */
    uint32_t current_cluster = fat32_state.root_cluster;

    for (int i = 0; i < count; i++) {
        fat32_dirent_t entry;
        char lfn[FAT32_LFN_MAX_NAME_LEN + 1];
        uint32_t dir_cluster, entry_offset;

        int ret = fat32_find_entry(current_cluster, components[i],
                                    &entry, lfn, &dir_cluster, &entry_offset);
        if (ret != FAT32_OK) return ret;

        if (i < count - 1) {
            /* This must be a directory to continue */
            if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
                return FAT32_ERR_NOT_DIR;
            }
            current_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
            if (current_cluster < 2) {
                /* Empty directory cluster - can't go deeper */
                return FAT32_ERR_NOT_FOUND;
            }
        } else {
            /* Last component - this is the target */
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

/**
 * Write a directory entry back to disk at a specific location.
 */
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

/**
 * Write the FSInfo sector back to disk.
 */
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

/**
 * Find a free directory entry slot in a directory.
 * If none exists, extends the directory by one cluster.
 *
 * @param dir_cluster   Starting cluster of the directory
 * @param num_entries   Number of consecutive free entries needed (1 for short, more for LFN)
 * @param out_cluster   Output: cluster where free slot was found
 * @param out_offset    Output: byte offset within cluster
 * @return FAT32_OK on success
 */
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

    /* Need to extend the directory */
    uint32_t new_cluster = fat32_extend_chain(prev_cluster);
    if (new_cluster == 0) {
        kfree(cluster_buf);
        return FAT32_ERR_NO_SPACE;
    }

    /* Zero out the new cluster */
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

/**
 * Generate an 8.3 short name from a long name.
 * Handles the numeric tail (~1, ~2, etc.) for names that don't fit 8.3.
 */
static void fat32_generate_short_name(const char* long_name, uint8_t* out83) {
    memset(out83, ' ', 11);

    int i, j;
    const char* dot = 0;

    /* Find the last dot */
    for (i = 0; long_name[i]; i++) {
        if (long_name[i] == '.') dot = &long_name[i];
    }

    /* Copy base name (up to 8 chars, uppercase) */
    int base_len = dot ? (int)(dot - long_name) : i;
    for (j = 0; j < base_len && j < 8; j++) {
        char c = fat32_toupper(long_name[j]);
        if (fat32_is_valid_83_char(c)) {
            out83[j] = c;
        } else {
            out83[j] = '_';
        }
    }

    /* Copy extension (up to 3 chars) */
    if (dot) {
        for (j = 0; j < 3 && dot[1 + j]; j++) {
            char c = fat32_toupper(dot[1 + j]);
            if (fat32_is_valid_83_char(c)) {
                out83[8 + j] = c;
            } else {
                out83[8 + j] = '_';
            }
        }
    }

    /* If base name is longer than 8, add numeric tail */
    if (base_len > 8) {
        /* Use ~1 suffix for simplicity */
        out83[6] = '~';
        out83[7] = '1';
    }
}

/**
 * Add a directory entry to a directory.
 * For simplicity, this only writes the short name entry (no LFN entries).
 */
static int fat32_add_dir_entry(uint32_t dir_cluster,
                                const char* name,
                                uint32_t first_cluster,
                                uint32_t file_size,
                                uint8_t attr) {
    uint32_t entry_cluster, entry_offset;
    int ret;

    /* Find a free slot (need 1 entry for short name) */
    ret = fat32_find_free_dir_entry(dir_cluster, 1,
                                     &entry_cluster, &entry_offset);
    if (ret != FAT32_OK) return ret;

    /* Build the directory entry */
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

    /* Write the entry */
    return fat32_write_dir_entry(entry_cluster, entry_offset, &entry);
}

/**
 * Mark a directory entry as deleted (set first byte to 0xE5).
 */
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

    /* Mark entry as deleted */
    cluster_buf[entry_offset] = 0xE5;

    /* Also mark any preceding LFN entries as deleted */
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

/**
 * Initialize a new directory cluster with . and .. entries.
 */
static int fat32_init_dir_cluster(uint32_t cluster, uint32_t parent_cluster) {
    uint8_t* cluster_buf;
    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    memset(cluster_buf, 0, fat32_state.bytes_per_cluster);

    fat32_dirent_t* dot = (fat32_dirent_t*)cluster_buf;
    fat32_dirent_t* dotdot = (fat32_dirent_t*)(cluster_buf + FAT32_DIR_ENTRY_SIZE);

    /* "." entry */
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->cluster_hi = (cluster >> 16) & 0xFFFF;
    dot->cluster_lo = cluster & 0xFFFF;
    dot->create_date = fat32_date_now();
    dot->write_date = fat32_date_now();
    dot->access_date = fat32_date_now();

    /* ".." entry */
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

/**
 * Allocate a free file handle. Returns handle index or -1.
 */
static int fat32_alloc_handle(void) {
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        if (!fat32_state.handles[i].in_use) {
            return i;
        }
    }
    return -1;
}

/**
 * Validate a file handle. Returns 1 if valid, 0 if not.
 */
static int fat32_valid_handle(int handle) {
    if (handle < 0 || handle >= FAT32_MAX_OPEN_FILES) return 0;
    return fat32_state.handles[handle].in_use;
}

/* ========================================================================
 * PUBLIC API - INITIALIZATION
 * ======================================================================== */

int fat32_init(uint32_t partition_start_lba) {
    fat32_boot_sector_t bs;

    /* Reset state */
    memset(&fat32_state, 0, sizeof(fat32_state));

    /* Read the boot sector (sector 0 of the partition) */
    if (disk_read_block(partition_start_lba, &bs) != 0) {
        s_printf("[FAT32] Failed to read boot sector at LBA ");
        /* Can't easily print uint32_t with s_printf - just indicate error */
        s_printf("error\r\n");
        return FAT32_ERR_IO;
    }

    /* Validate BPB signature */
    if (bs.ebpb.signature != FAT32_BPB_SIGNATURE) {
        s_printf("[FAT32] Invalid BPB signature\r\n");
        return FAT32_ERR_NO_FS;
    }

    /* Validate bytes per sector */
    if (bs.bpb.bytes_per_sector != FAT32_SECTOR_SIZE) {
        s_printf("[FAT32] Invalid sector size\r\n");
        return FAT32_ERR_NO_FS;
    }

    /* Validate this is actually FAT32 */
    if (bs.bpb.sectors_per_fat_16 != 0 || bs.bpb.root_entry_count != 0 ||
        bs.bpb.total_sectors_16 != 0) {
        /* This might be FAT12/16, not FAT32 */
        s_printf("[FAT32] Not a FAT32 volume\r\n");
        return FAT32_ERR_NO_FS;
    }

    /* Validate sectors_per_fat */
    if (bs.ebpb.sectors_per_fat == 0) {
        s_printf("[FAT32] Invalid FAT size\r\n");
        return FAT32_ERR_NO_FS;
    }

    /* Validate extended boot signature */
    if (bs.ebpb.boot_sig != FAT32_FAT_TYPE_SIGNATURE) {
        s_printf("[FAT32] Invalid extended boot signature\r\n");
        return FAT32_ERR_NO_FS;
    }

    /* Store partition parameters */
    fat32_state.partition_start = partition_start_lba;
    fat32_state.sectors_per_fat = bs.ebpb.sectors_per_fat;
    fat32_state.root_cluster = bs.ebpb.root_cluster;
    fat32_state.sectors_per_cluster = bs.bpb.sectors_per_cluster;
    fat32_state.bytes_per_cluster = bs.bpb.sectors_per_cluster * bs.bpb.bytes_per_sector;
    fat32_state.num_fats = bs.bpb.num_fats;
    fat32_state.ext_flags = bs.ebpb.ext_flags;
    fat32_state.fs_info_sector = bs.ebpb.fs_info_sector;
    fat32_state.volume_serial = bs.ebpb.volume_serial;

    /* Calculate total sectors */
    if (bs.bpb.total_sectors_32 != 0) {
        fat32_state.total_sectors = bs.bpb.total_sectors_32;
    } else {
        fat32_state.total_sectors = bs.bpb.total_sectors_16;
    }

    /* Calculate key sector positions */
    fat32_state.fat_start = partition_start_lba + bs.bpb.reserved_sectors;
    fat32_state.data_start = fat32_state.fat_start +
                              (bs.bpb.num_fats * bs.ebpb.sectors_per_fat);

    /* Calculate total data clusters */
    uint32_t data_sectors = fat32_state.total_sectors -
                             (bs.bpb.reserved_sectors +
                              bs.bpb.num_fats * bs.ebpb.sectors_per_fat);
    fat32_state.total_clusters = data_sectors / bs.bpb.sectors_per_cluster;

    /* FAT32 must have >= 65525 clusters */
    if (fat32_state.total_clusters < 65525) {
        s_printf("[FAT32] Cluster count too low for FAT32\r\n");
        return FAT32_ERR_NO_FS;
    }

    /* Read FSInfo sector if available */
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

    /* Validate free cluster count if available */
    if (fat32_state.next_free_cluster == 0xFFFFFFFF ||
        fat32_state.next_free_cluster < 2 ||
        fat32_state.next_free_cluster >= fat32_state.total_clusters + 2) {
        fat32_state.next_free_cluster = 2;
    }

    /* Copy volume label */
    for (int i = 0; i < 11; i++) {
        fat32_state.volume_label[i] = bs.ebpb.volume_label[i];
    }
    fat32_state.volume_label[11] = '\0';

    /* Initialize FAT cache */
    fat_cache_init();

    /* Initialize file handles */
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        fat32_state.handles[i].in_use = 0;
    }

    fat32_state.initialized = 1;

    s_printf("[FAT32] Initialized: root_cluster=, sectors_per_fat=, total_clusters=\r\n");
    /* Note: s_printf is limited, so we log a simplified message */
    s_printf("[FAT32] Volume mounted successfully\r\n");

    return FAT32_OK;
}

/* ========================================================================
 * PUBLIC API - FILE OPERATIONS
 * ======================================================================== */

int fat32_open(const char* path, int flags) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path) return FAT32_ERR_PARAM;

    int handle = fat32_alloc_handle();
    if (handle < 0) return FAT32_ERR_BUSY;

    fat32_dirent_t entry;
    char lfn[FAT32_LFN_MAX_NAME_LEN + 1];
    uint32_t dir_cluster, entry_offset;

    int ret = fat32_resolve_path(path, &entry, lfn, &dir_cluster, &entry_offset);

    if (ret == FAT32_OK) {
        /* File exists */
        if (flags & FAT32_O_EXCL) {
            /* Handle was not marked in_use yet, so no cleanup needed */
            return FAT32_ERR_EXISTS;
        }

        /* Check if it's a directory and we didn't ask for one */
        if ((entry.attr & FAT32_ATTR_DIRECTORY) && !(flags & FAT32_O_DIRECTORY)) {
            /* Allow opening directories for readdir */
        }

        /* Check read-only */
        if ((entry.attr & FAT32_ATTR_READ_ONLY) &&
            (flags & (FAT32_O_WRONLY | FAT32_O_RDWR))) {
            /* Handle was not marked in_use yet, so no cleanup needed */
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

        /* Truncate if requested */
        if ((flags & FAT32_O_TRUNC) && !(entry.attr & FAT32_ATTR_READ_ONLY)) {
            if (first_cluster >= 2) {
                fat32_free_chain(first_cluster);
            }
            fat32_state.handles[handle].first_cluster = 0;
            fat32_state.handles[handle].current_cluster = 0;
            fat32_state.handles[handle].file_size = 0;
            fat32_state.handles[handle].position = 0;
            fat32_state.handles[handle].current_offset = 0;

            /* Update directory entry */
            entry.file_size = 0;
            entry.cluster_hi = 0;
            entry.cluster_lo = 0;
            fat32_write_dir_entry(dir_cluster, entry_offset, &entry);
        }

        /* Append mode: seek to end */
        if (flags & FAT32_O_APPEND) {
            fat32_state.handles[handle].position = fat32_state.handles[handle].file_size;
        }

        return handle;
    }

    /* File not found - create if O_CREAT */
    if ((flags & FAT32_O_CREAT) && ret == FAT32_ERR_NOT_FOUND) {
        /* Parse path to get parent directory and filename */
        char components[32][FAT32_LFN_MAX_NAME_LEN + 1];
        int count = fat32_parse_path(path, components, 32);
        if (count == 0) return FAT32_ERR_PATH;

        /* Resolve parent directory */
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

        /* Allocate a cluster for the new file (or use 0 for empty file) */
        uint32_t new_cluster = 0;
        if (flags & (FAT32_O_WRONLY | FAT32_O_RDWR)) {
            /* Will allocate on first write; start with no cluster */
        }

        /* Add directory entry */
        int add_ret = fat32_add_dir_entry(parent_cluster, components[count - 1],
                                           new_cluster, 0,
                                           (flags & FAT32_O_DIRECTORY) ?
                                               FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE);
        if (add_ret != FAT32_OK) return add_ret;

        /* Re-resolve to get the entry position */
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
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    /* Update the directory entry if file was modified */
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
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    fat32_file_handle_t* fh = &fat32_state.handles[handle];

    /* Check read permission */
    if ((fh->flags & 0x0003) == FAT32_O_WRONLY) {
        return FAT32_ERR_READ_ONLY;
    }

    /* Don't read past end of file */
    if (fh->position >= fh->file_size) return 0;

    uint32_t bytes_remaining = fh->file_size - fh->position;
    if (count > bytes_remaining) count = bytes_remaining;

    uint32_t bytes_read = 0;
    uint8_t* dst = (uint8_t*)buffer;
    uint8_t* cluster_buf;

    cluster_buf = (uint8_t*)kmalloc(fat32_state.bytes_per_cluster);
    if (!cluster_buf) return FAT32_ERR_NO_MEM;

    while (bytes_read < count) {
        /* Calculate which cluster and offset within cluster */
        uint32_t cluster_index = fh->position / fat32_state.bytes_per_cluster;
        uint32_t offset_in_cluster = fh->position % fat32_state.bytes_per_cluster;

        /* Follow chain to the right cluster */
        uint32_t cluster;
        if (fh->first_cluster == 0) {
            /* No clusters allocated - file is empty */
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

        /* Read the cluster */
        int ret = fat32_read_cluster(cluster, cluster_buf);
        if (ret != FAT32_OK) {
            kfree(cluster_buf);
            return (bytes_read > 0) ? (int32_t)bytes_read : ret;
        }

        /* Copy data from cluster to buffer */
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
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!fat32_valid_handle(handle)) return FAT32_ERR_HANDLE;

    fat32_file_handle_t* fh = &fat32_state.handles[handle];

    /* Check write permission */
    if (!(fh->flags & (FAT32_O_WRONLY | FAT32_O_RDWR))) {
        return FAT32_ERR_READ_ONLY;
    }

    /* Check read-only attribute */
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
            /* Need to allocate first cluster */
            cluster = fat32_alloc_first_cluster();
            if (cluster == 0) {
                kfree(cluster_buf);
                return (bytes_written > 0) ? (int32_t)bytes_written : FAT32_ERR_NO_SPACE;
            }
            fh->first_cluster = cluster;

            /* Zero the new cluster */
            memset(cluster_buf, 0, fat32_state.bytes_per_cluster);
            fat32_write_cluster(cluster, cluster_buf);
        } else if (cluster_index == 0) {
            cluster = fh->first_cluster;
        } else {
            /* Follow chain; extend if necessary */
            cluster = fat32_follow_chain(fh->first_cluster, cluster_index);
            if (cluster == 0 || fat32_is_eoc(cluster)) {
                /* Need to extend the chain */
                uint32_t last = fh->first_cluster;
                uint32_t chain_len = fat32_chain_length(fh->first_cluster);
                while (chain_len <= cluster_index) {
                    uint32_t new_clust = fat32_extend_chain(last);
                    if (new_clust == 0) {
                        kfree(cluster_buf);
                        return (bytes_written > 0) ? (int32_t)bytes_written : FAT32_ERR_NO_SPACE;
                    }

                    /* Zero the new cluster */
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

        /* Read-modify-write: read the cluster first if we're not writing the whole thing */
        uint32_t bytes_in_cluster = fat32_state.bytes_per_cluster - offset_in_cluster;
        uint32_t to_copy = count - bytes_written;
        if (to_copy > bytes_in_cluster) to_copy = bytes_in_cluster;

        if (offset_in_cluster != 0 || to_copy < fat32_state.bytes_per_cluster) {
            /* Partial write - need to read first */
            int ret = fat32_read_cluster(cluster, cluster_buf);
            if (ret != FAT32_OK) {
                kfree(cluster_buf);
                return (bytes_written > 0) ? (int32_t)bytes_written : ret;
            }
        } else {
            /* Full cluster write - no need to read first */
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

        /* Update file size */
        if (fh->position > fh->file_size) {
            fh->file_size = fh->position;
        }
    }

    kfree(cluster_buf);
    return (int32_t)bytes_written;
}

int32_t fat32_seek(int handle, int32_t offset, int whence) {
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
 * PUBLIC API - FILE INFORMATION
 * ======================================================================== */

int fat32_stat(const char* path, fat32_stat_t* stat) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path || !stat) return FAT32_ERR_PARAM;

    fat32_dirent_t entry;
    char lfn[FAT32_LFN_MAX_NAME_LEN + 1];

    int ret = fat32_resolve_path(path, &entry, lfn, 0, 0);
    if (ret != FAT32_OK) return ret;

    memset(stat, 0, sizeof(fat32_stat_t));

    /* Copy name */
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
 * PUBLIC API - DIRECTORY OPERATIONS
 * ======================================================================== */

int fat32_mkdir(const char* path) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path) return FAT32_ERR_PARAM;

    char components[32][FAT32_LFN_MAX_NAME_LEN + 1];
    int count = fat32_parse_path(path, components, 32);
    if (count == 0) return FAT32_ERR_PATH;

    /* Check if it already exists */
    fat32_dirent_t existing;
    if (fat32_resolve_path(path, &existing, 0, 0, 0) == FAT32_OK) {
        return FAT32_ERR_EXISTS;
    }

    /* Resolve parent directory */
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

    /* Allocate a cluster for the new directory */
    uint32_t new_cluster = fat32_alloc_first_cluster();
    if (new_cluster == 0) return FAT32_ERR_NO_SPACE;

    /* Initialize directory cluster with . and .. */
    int ret = fat32_init_dir_cluster(new_cluster, parent_cluster ? parent_cluster : fat32_state.root_cluster);
    if (ret != FAT32_OK) {
        fat_set_next_cluster(new_cluster, FAT32_FREE_CLUSTER);
        return ret;
    }

    /* Add directory entry in parent */
    ret = fat32_add_dir_entry(parent_cluster, components[count - 1],
                               new_cluster, 0, FAT32_ATTR_DIRECTORY);
    if (ret != FAT32_OK) {
        fat32_free_chain(new_cluster);
        return ret;
    }

    return FAT32_OK;
}

int fat32_unlink(const char* path) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!path) return FAT32_ERR_PARAM;

    fat32_dirent_t entry;
    uint32_t dir_cluster, entry_offset;

    int ret = fat32_resolve_path(path, &entry, 0, &dir_cluster, &entry_offset);
    if (ret != FAT32_OK) return ret;

    /* Can't delete root directory */
    if (dir_cluster == 0) return FAT32_ERR_ACCESS;

    /* If it's a directory, check that it's empty */
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

                    /* Skip . and .. */
                    if (de->name[0] == '.' && (de->name[1] == ' ' || de->name[1] == '\0')) continue;
                    if (de->name[0] == '.' && de->name[1] == '.' && (de->name[2] == ' ' || de->name[2] == '\0')) continue;

                    /* Directory is not empty */
                    kfree(cluster_buf);
                    return FAT32_ERR_NOT_EMPTY;
                }

                cluster = fat_get_next_cluster(cluster);
            }

        dir_empty:
            kfree(cluster_buf);
        }
    }

    /* Free the cluster chain */
    uint32_t first_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
    if (first_cluster >= 2) {
        fat32_free_chain(first_cluster);
    }

    /* Mark directory entry as deleted */
    ret = fat32_delete_dir_entry(dir_cluster, entry_offset);

    return ret;
}

int fat32_rename(const char* oldpath, const char* newpath) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!oldpath || !newpath) return FAT32_ERR_PARAM;

    fat32_dirent_t old_entry;
    uint32_t old_dir_cluster, old_entry_offset;

    int ret = fat32_resolve_path(oldpath, &old_entry, 0,
                                  &old_dir_cluster, &old_entry_offset);
    if (ret != FAT32_OK) return ret;

    /* Check if new path already exists */
    fat32_dirent_t new_entry;
    if (fat32_resolve_path(newpath, &new_entry, 0, 0, 0) == FAT32_OK) {
        return FAT32_ERR_EXISTS;
    }

    /* Parse the new path */
    char new_components[32][FAT32_LFN_MAX_NAME_LEN + 1];
    int new_count = fat32_parse_path(newpath, new_components, 32);
    if (new_count == 0) return FAT32_ERR_PATH;

    /* Resolve the new parent directory */
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

    /* If same directory, just update the name in-place */
    if (new_parent_cluster == old_dir_cluster) {
        /* Update the short name */
        fat32_generate_short_name(new_components[new_count - 1], old_entry.name);
        old_entry.write_date = fat32_date_now();
        old_entry.write_time = fat32_time_now();
        return fat32_write_dir_entry(old_dir_cluster, old_entry_offset, &old_entry);
    }

    /* Different directory: add entry in new parent, delete from old */
    ret = fat32_add_dir_entry(new_parent_cluster, new_components[new_count - 1],
                               first_cluster, old_entry.file_size, old_entry.attr);
    if (ret != FAT32_OK) return ret;

    return fat32_delete_dir_entry(old_dir_cluster, old_entry_offset);
}

int fat32_readdir(const char* path, fat32_dirent_out_t* entries, uint32_t max) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;
    if (!entries || max == 0) return FAT32_ERR_PARAM;

    uint32_t start_cluster;

    if (!path || (path[0] == '/' && path[1] == '\0')) {
        /* Root directory */
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

            /* End of directory */
            if (entry->name[0] == 0x00) {
                kfree(cluster_buf);
                kfree(lfn_buf);
                return (int)entry_count;
            }

            /* Deleted entry */
            if (entry->name[0] == 0xE5) {
                lfn_count = 0;
                continue;
            }

            /* LFN entry */
            if (entry->attr == FAT32_ATTR_LFN) {
                if (lfn_count < 20) {
                    memcpy(lfn_buf + lfn_count * FAT32_DIR_ENTRY_SIZE,
                           entry, FAT32_DIR_ENTRY_SIZE);
                    lfn_count++;
                }
                continue;
            }

            /* Skip volume ID */
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_count = 0;
                continue;
            }

            /* This is a real directory entry */
            if (entry_count < max) {
                /* Try to get LFN */
                char lfn_name[FAT32_LFN_MAX_NAME_LEN + 1];
                lfn_name[0] = '\0';

                if (lfn_count > 0) {
                    fat32_extract_lfn(lfn_buf, lfn_count, lfn_name);
                }

                if (lfn_name[0] == '\0') {
                    fat32_short_name_to_str(entry->name, lfn_name,
                                            FAT32_LFN_MAX_NAME_LEN + 1);
                }

                /* Skip . and .. */
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
 * PUBLIC API - SYNC AND UTILITY
 * ======================================================================== */

int fat32_sync(void) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;

    /* Flush FAT cache */
    for (int i = 0; i < FAT32_FAT_CACHE_SECTORS; i++) {
        if (fat32_state.fat_cache[i].dirty &&
            fat32_state.fat_cache[i].sector != 0xFFFFFFFF) {
            uint32_t sec = fat32_state.fat_cache[i].sector;
            disk_write_block(sec, fat32_state.fat_cache[i].data);

            /* Mirror to backup FAT(s) */
            if (!(fat32_state.ext_flags & 0x0080)) {
                for (int f = 1; f < fat32_state.num_fats; f++) {
                    uint32_t backup_sec = sec + (f * fat32_state.sectors_per_fat);
                    disk_write_block(backup_sec, fat32_state.fat_cache[i].data);
                }
            }

            fat32_state.fat_cache[i].dirty = 0;
        }
    }

    /* Flush disk block cache */
    disk_flush_cache();

    /* Update FSInfo */
    fat32_update_fsinfo();

    return FAT32_OK;
}

int32_t fat32_free_clusters(void) {
    if (!fat32_state.initialized) return FAT32_ERR_NO_FS;

    /* If we don't know the count, scan the FAT */
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
    return fat32_state.initialized;
}
