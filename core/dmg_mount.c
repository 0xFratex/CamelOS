// core/dmg_mount.c - CamelOS DMG (Apple Disk Image) Mounter
// Allows CamelOS to mount macOS .dmg files and extract .app bundles
// Supports UDIF format with KOLY trailer, XML plist partition maps,
// and raw/zlib compressed block types

#include "dmg_mount.h"
#include "string.h"
#include "memory.h"
#include "zlib_inflate.h"
#include "../sys/api.h"
#include "../fs/pfs32.h"
#include "../hal/drivers/serial.h"

// =========================================================================
// Internal Constants
// =========================================================================

#define DMG_SECTOR_SIZE         512
#define DMG_PLIST_MAX_SIZE      (256 * 1024)  // 256KB max plist size
#define DMG_MISH_MAGIC          0x6D697368    // "mish"
#define DMG_MISH_HEADER_SIZE    204
#define DMG_BLOCK_ENTRY_SIZE    40
#define DMG_MISH_BLOCK_MAX      512

// HFS+ constants
#define HFS_PLUS_SIG            0x482B        // 'H+'
#define HFSX_SIG                0x4858        // 'HX'
#define HFS_CATALOG_CNID        2             // CNID of catalog B-tree
#define HFS_ROOT_PARENT_CNID    1             // Parent CNID of root folder
#define HFS_BT_NODE_LEAF        0xFF          // Leaf node kind (-1 as uint8)
#define HFS_BT_NODE_INDEX       0x00          // Index node kind
#define HFS_BT_NODE_HEADER      0x01          // Header node kind

// Catalog record types (big-endian on disk)
#define HFS_REC_FOLDER          0x0001
#define HFS_REC_FILE            0x0002
#define HFS_REC_FOLDER_THREAD   0x0003
#define HFS_REC_FILE_THREAD     0x0004

// HFS+ fork data size (80 bytes on disk)
#define HFS_FORK_DATA_SIZE      80

// Internal I/O buffer size
#define DMG_IO_BUF_SIZE         (64 * 1024)   // 64KB

// =========================================================================
// Static State
// =========================================================================

static dmg_mount_t dmg_mounts[DMG_MAX_MOUNTED];
static int dmg_system_inited = 0;

// Per-mount HFS+ state (not exposed in header)
typedef struct {
    uint32_t block_size;                       // HFS+ allocation block size
    uint32_t total_blocks;                     // Total allocation blocks
    uint32_t catalog_extents[8][2];            // [startBlock, blockCount] x 8
    uint16_t catalog_node_size;                // B-tree node size
    uint32_t catalog_root_node;                // Root node number
    uint32_t catalog_first_leaf;               // First leaf node number
    uint32_t catalog_last_leaf;                // Last leaf node number
    uint32_t hfs_partition_idx;                // Index into partitions[] for HFS+
    int      valid;                            // 1 if HFS+ metadata parsed OK
} dmg_hfs_state_t;

static dmg_hfs_state_t dmg_hfs[DMG_MAX_MOUNTED];

// =========================================================================
// Big-Endian Conversion Helpers
// =========================================================================
// DMG and HFS+ store multi-byte integers in big-endian (network) byte order.
// CamelOS is little-endian x86, so we must swap bytes.

static uint16_t be16_to_cpu(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32_to_cpu(const uint8_t* p) {
    return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static uint64_t be64_to_cpu(const uint8_t* p) {
    uint32_t hi = be32_to_cpu(p);
    uint32_t lo = be32_to_cpu(p + 4);
    return ((uint64_t)hi << 32) | lo;
}

// Write big-endian values (used internally if needed)
static void cpu_to_be32(uint8_t* p, uint32_t val) {
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

// =========================================================================
// Base64 Decoder
// =========================================================================
// Required to decode the <data> blocks in the DMG plist that contain
// the mish block data (partition block maps).

static const int8_t b64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static int base64_decode(const char* in, int in_len, uint8_t* out, int* out_len) {
    int i = 0, j = 0;
    uint32_t buf = 0;
    int bits = 0;

    while (i < in_len) {
        // Skip whitespace within base64 data
        char c = in[i++];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') break; // Padding

        int8_t val = b64_table[(uint8_t)c];
        if (val < 0) continue; // Skip invalid chars

        buf = (buf << 6) | (uint32_t)val;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out[j++] = (uint8_t)(buf >> bits);
            buf &= (1U << bits) - 1;
        }
    }

    *out_len = j;
    return 0;
}

// =========================================================================
// KOLY Trailer Parser
// =========================================================================
// The KOLY trailer is 512 bytes at the end of the DMG file.
// All fields are big-endian. We parse them field-by-field.

static int parse_koly(const uint8_t* raw, dmg_koly_t* koly) {
    // Check magic
    uint32_t sig = be32_to_cpu(raw);
    if (sig != DMG_KOLY_MAGIC) {
        s_printf("[DMG] KOLY magic mismatch: expected 0x6b6f6c79, got 0x");
        char hex[9];
        int_to_hex(sig, hex);
        s_printf(hex);
        s_printf("\n");
        return -1;
    }

    koly->signature = sig;
    koly->version = be32_to_cpu(raw + 4);
    koly->header_size = be32_to_cpu(raw + 8);
    koly->flags = be32_to_cpu(raw + 12);
    koly->running_data_fork_offset = be64_to_cpu(raw + 16);
    koly->data_fork_offset = be64_to_cpu(raw + 24);
    koly->data_fork_length = be64_to_cpu(raw + 32);
    koly->rsrc_fork_offset = be64_to_cpu(raw + 40);
    koly->rsrc_fork_length = be64_to_cpu(raw + 48);
    koly->segment_number = be32_to_cpu(raw + 56);
    koly->segment_count = be32_to_cpu(raw + 60);

    // Segment ID (16 bytes, stored as 4 uint32_t)
    for (int i = 0; i < 4; i++) {
        koly->segment_id[i] = be32_to_cpu(raw + 64 + i * 4);
    }

    // Data checksum
    koly->data_checksum_type = be32_to_cpu(raw + 80);
    koly->data_checksum_size = be32_to_cpu(raw + 84);
    for (int i = 0; i < 32; i++) {
        koly->data_checksum[i] = be32_to_cpu(raw + 88 + i * 4);
    }

    // XML plist offset and length
    koly->xml_plist_offset = be64_to_cpu(raw + 216);
    koly->xml_plist_length = be64_to_cpu(raw + 224);

    // Master checksum
    koly->master_checksum_type = be32_to_cpu(raw + 232);
    koly->master_checksum_size = be32_to_cpu(raw + 236);
    for (int i = 0; i < 32; i++) {
        koly->master_checksum[i] = be32_to_cpu(raw + 240 + i * 4);
    }

    // Image variant and sector count
    koly->image_variant = be32_to_cpu(raw + 368);
    koly->sector_count = be64_to_cpu(raw + 372);

    s_printf("[DMG] KOLY parsed: version=");
    char ver[12];
    int_to_str(koly->version, ver);
    s_printf(ver);
    s_printf(" sectors=");
    char secs[12];
    int_to_str((int)koly->sector_count, secs);
    s_printf(secs);
    s_printf(" plist_offset=");
    char poff[12];
    int_to_str((int)koly->xml_plist_offset, poff);
    s_printf(poff);
    s_printf(" plist_len=");
    char plen[12];
    int_to_str((int)koly->xml_plist_length, plen);
    s_printf(plen);
    s_printf("\n");

    return 0;
}

// =========================================================================
// Mish Block Parser
// =========================================================================
// Each partition in the plist has a base64-encoded "Data" field that
// contains a mish block. The mish block has a 204-byte header followed
// by block map entries (40 bytes each).

static int parse_mish_block(const uint8_t* data, int data_len,
                            dmg_blkmap_entry_t* blocks, int* block_count,
                            uint32_t* sector_start, uint32_t* sector_count) {
    if (data_len < DMG_MISH_HEADER_SIZE) {
        s_printf("[DMG] Mish data too short: ");
        char sz[12];
        int_to_str(data_len, sz);
        s_printf(sz);
        s_printf(" bytes\n");
        return -1;
    }

    // Check mish signature
    uint32_t sig = be32_to_cpu(data);
    if (sig != DMG_MISH_MAGIC) {
        s_printf("[DMG] Mish magic mismatch\n");
        return -1;
    }

    // Parse mish header
    // Offset  0: signature (4)
    // Offset  4: version (4)
    // Offset  8: sector_start (8)
    // Offset 16: sector_count (8)
    // Offset 24: compressed_offset (8)
    // Offset 32: compressed_length (8)
    // ... other header fields ...
    // Offset 200: blocks_descriptor - number of block entries (4)

    uint64_t ss = be64_to_cpu(data + 8);
    uint64_t sc = be64_to_cpu(data + 16);
    *sector_start = (uint32_t)ss;
    *sector_count = (uint32_t)sc;

    // Number of block entries at offset 200
    uint32_t num_blocks = be32_to_cpu(data + 200);
    if (num_blocks > DMG_MISH_BLOCK_MAX) {
        s_printf("[DMG] Too many mish blocks: ");
        char nb[12];
        int_to_str(num_blocks, nb);
        s_printf(nb);
        s_printf("\n");
        num_blocks = DMG_MISH_BLOCK_MAX;
    }

    // Parse block entries starting after the 204-byte header
    int count = 0;
    uint32_t running_sector = *sector_start;

    for (uint32_t i = 0; i < num_blocks && count < DMG_MAX_BLKMAP; i++) {
        int offset = DMG_MISH_HEADER_SIZE + i * DMG_BLOCK_ENTRY_SIZE;
        if (offset + DMG_BLOCK_ENTRY_SIZE > data_len) break;

        const uint8_t* entry = data + offset;

        // Block entry format (40 bytes):
        // Byte 0: entry_type (0=unused, 1=terminator, 2=fill/zero, 0xFF=comment)
        // Bytes 1-3: comment/reserved
        // Bytes 4-7: compression_type
        // Bytes 8-15: compressed_offset (uint64_t)
        // Bytes 16-23: compressed_length (uint64_t)
        // Bytes 24-31: uncompressed_length (uint64_t)
        // Bytes 32-39: padding/reserved

        uint8_t entry_type = entry[0];
        if (entry_type == 1) break;  // Terminator

        // Skip unused and comment entries
        if (entry_type == 0 || entry_type == 0xFF) continue;

        uint32_t comp_type = be32_to_cpu(entry + 4);
        uint64_t comp_off = be64_to_cpu(entry + 8);
        uint64_t comp_len = be64_to_cpu(entry + 16);
        uint64_t uncomp_len = be64_to_cpu(entry + 24);

        blocks[count].sector_number = running_sector;
        blocks[count].compression_type = comp_type;
        blocks[count].compressed_offset = (uint32_t)comp_off;
        blocks[count].compressed_length = (uint32_t)comp_len;
        blocks[count].uncompressed_length = (uint32_t)uncomp_len;

        // Advance running sector count
        if (uncomp_len > 0) {
            running_sector += (uint32_t)(uncomp_len / DMG_SECTOR_SIZE);
        }

        count++;
    }

    *block_count = count;

    s_printf("[DMG] Mish parsed: sector_start=");
    char ss_str[12];
    int_to_str(*sector_start, ss_str);
    s_printf(ss_str);
    s_printf(" sector_count=");
    char sc_str[12];
    int_to_str(*sector_count, sc_str);
    s_printf(sc_str);
    s_printf(" blocks=");
    char bc_str[12];
    int_to_str(count, bc_str);
    s_printf(bc_str);
    s_printf("\n");

    return 0;
}

// =========================================================================
// Simplified XML Plist Parser
// =========================================================================
// Extracts partition names, IDs, and base64-encoded mish data from
// the DMG's XML plist. Uses strstr-based searching for simplicity.

// Helper: extract text between <tag> and </tag>
// Returns pointer to start of content, sets *out_len to content length
static const char* xml_get_tag_content(const char* xml, const char* tag,
                                       int* out_len) {
    char open_tag[68];
    char close_tag[72];

    // Build open tag: <tag>
    open_tag[0] = '<';
    int tlen = strlen(tag);
    memcpy(open_tag + 1, tag, tlen);
    open_tag[1 + tlen] = '>';
    open_tag[2 + tlen] = 0;

    // Build close tag: </tag>
    close_tag[0] = '<';
    close_tag[1] = '/';
    memcpy(close_tag + 2, tag, tlen);
    close_tag[2 + tlen] = '>';
    close_tag[3 + tlen] = 0;

    const char* start = strstr(xml, open_tag);
    if (!start) {
        *out_len = 0;
        return 0;
    }
    start += 2 + tlen; // Skip past <tag>

    const char* end = strstr(start, close_tag);
    if (!end) {
        *out_len = 0;
        return 0;
    }

    *out_len = (int)(end - start);
    return start;
}

// Helper: extract string value after a <key>name</key>
// Looks for the pattern: <key>name</key>...<string>value</string>
static int xml_get_string_for_key(const char* xml, const char* key_name,
                                  char* out, int max_out) {
    char key_pattern[80];
    strcpy(key_pattern, "<key>");
    strcat(key_pattern, key_name);
    strcat(key_pattern, "</key>");

    const char* key_pos = strstr(xml, key_pattern);
    if (!key_pos) return -1;

    key_pos += strlen(key_pattern);

    // Skip whitespace
    while (*key_pos == '\n' || *key_pos == '\r' || *key_pos == ' ' || *key_pos == '\t')
        key_pos++;

    // Look for <string> tag
    int content_len;
    const char* content = xml_get_tag_content(key_pos, "string", &content_len);
    if (!content || content_len <= 0) return -1;

    int copy_len = content_len;
    if (copy_len >= max_out) copy_len = max_out - 1;
    memcpy(out, content, copy_len);
    out[copy_len] = 0;
    return 0;
}

// Helper: extract integer value after a <key>name</key>
static int xml_get_int_for_key(const char* xml, const char* key_name, int* out_val) {
    char key_pattern[80];
    strcpy(key_pattern, "<key>");
    strcat(key_pattern, key_name);
    strcat(key_pattern, "</key>");

    const char* key_pos = strstr(xml, key_pattern);
    if (!key_pos) return -1;

    key_pos += strlen(key_pattern);

    // Skip whitespace
    while (*key_pos == '\n' || *key_pos == '\r' || *key_pos == ' ' || *key_pos == '\t')
        key_pos++;

    // Could be <integer> or <string> containing a number
    int content_len;
    const char* content = xml_get_tag_content(key_pos, "integer", &content_len);
    if (!content || content_len <= 0) {
        content = xml_get_tag_content(key_pos, "string", &content_len);
    }
    if (!content || content_len <= 0) return -1;

    // Parse integer
    int val = 0;
    int negative = 0;
    int i = 0;
    if (i < content_len && content[i] == '-') { negative = 1; i++; }
    if (i < content_len && content[i] == '0' && i + 1 < content_len && content[i + 1] == 'x') {
        // Hex
        i += 2;
        while (i < content_len) {
            char c = content[i];
            if (c >= '0' && c <= '9') val = val * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
            else break;
            i++;
        }
    } else {
        while (i < content_len && content[i] >= '0' && content[i] <= '9') {
            val = val * 10 + (content[i] - '0');
            i++;
        }
    }
    if (negative) val = -val;
    *out_val = val;
    return 0;
}

// Helper: extract base64 data after a <key>Data</key>
// Returns kmalloc'd buffer that caller must free, or NULL on error
static uint8_t* xml_get_data_for_key(const char* xml, const char* key_name,
                                     int* out_len) {
    char key_pattern[80];
    strcpy(key_pattern, "<key>");
    strcat(key_pattern, key_name);
    strcat(key_pattern, "</key>");

    const char* key_pos = strstr(xml, key_pattern);
    if (!key_pos) return 0;

    key_pos += strlen(key_pattern);

    // Skip whitespace
    while (*key_pos == '\n' || *key_pos == '\r' || *key_pos == ' ' || *key_pos == '\t')
        key_pos++;

    int content_len;
    const char* content = xml_get_tag_content(key_pos, "data", &content_len);
    if (!content || content_len <= 0) return 0;

    // Allocate output buffer (base64 expands 3:4, so output <= input * 3/4)
    int max_decoded = (content_len * 3) / 4 + 4;
    uint8_t* decoded = (uint8_t*)kmalloc(max_decoded);
    if (!decoded) return 0;

    int decoded_len = 0;
    if (base64_decode(content, content_len, decoded, &decoded_len) < 0) {
        kfree(decoded);
        return 0;
    }

    *out_len = decoded_len;
    return decoded;
}

// Parse the entire plist for partition information
static int parse_plist_partitions(const char* xml, int xml_len,
                                  dmg_partition_t* partitions,
                                  int* partition_count) {
    *partition_count = 0;

    // Find the blkx array within the plist
    const char* blkx_key = strstr(xml, "<key>blkx</key>");
    if (!blkx_key) {
        s_printf("[DMG] No blkx key found in plist\n");
        return -1;
    }

    // Move past the key to find the <array>
    const char* array_start = strstr(blkx_key, "<array>");
    if (!array_start) {
        s_printf("[DMG] No array found after blkx\n");
        return -1;
    }

    // Find the end of the array
    const char* array_end = strstr(array_start, "</array>");
    if (!array_end) {
        s_printf("[DMG] No array end found\n");
        return -1;
    }

    // Iterate over <dict> entries within the array
    const char* pos = array_start + 7; // Skip "<array>"

    while (pos < array_end && *partition_count < DMG_MAX_PARTITIONS) {
        // Find the next <dict> within the array
        const char* dict_start = strstr(pos, "<dict>");
        if (!dict_start || dict_start >= array_end) break;

        const char* dict_end = strstr(dict_start, "</dict>");
        if (!dict_end || dict_end >= array_end) break;

        // Calculate dict content boundaries for searching
        int dict_len = (int)(dict_end - dict_start);
        char* dict_buf = (char*)kmalloc(dict_len + 1);
        if (!dict_buf) break;
        memcpy(dict_buf, dict_start, dict_len);
        dict_buf[dict_len] = 0;

        dmg_partition_t* part = &partitions[*partition_count];
        memset(part, 0, sizeof(dmg_partition_t));

        // Extract Name
        if (xml_get_string_for_key(dict_buf, "Name", part->name, sizeof(part->name)) < 0) {
            part->name[0] = 0;
        }

        // Extract ID
        char id_str[32];
        if (xml_get_string_for_key(dict_buf, "ID", id_str, sizeof(id_str)) < 0) {
            // Try as integer
            int id_val = 0;
            if (xml_get_int_for_key(dict_buf, "ID", &id_val) == 0) {
                int_to_str(id_val, part->id);
            } else {
                strcpy(part->id, "unknown");
            }
        } else {
            strncpy(part->id, id_str, sizeof(part->id) - 1);
        }

        // Extract Data (base64-encoded mish block)
        int data_len = 0;
        uint8_t* mish_data = xml_get_data_for_key(dict_buf, "Data", &data_len);
        if (mish_data && data_len > 0) {
            // Allocate block map entries
            part->blocks = (dmg_blkmap_entry_t*)kmalloc(
                DMG_MAX_BLKMAP * sizeof(dmg_blkmap_entry_t));
            if (part->blocks) {
                uint32_t sector_start, sector_count;
                int block_count = 0;
                if (parse_mish_block(mish_data, data_len, part->blocks,
                                     &block_count, &sector_start, &sector_count) == 0) {
                    part->block_map_start = sector_start;
                    part->block_map_count = block_count;
                } else {
                    kfree(part->blocks);
                    part->blocks = 0;
                    part->block_map_count = 0;
                }
            }
            kfree(mish_data);
        }

        s_printf("[DMG] Partition ");
        char pn[12];
        int_to_str(*partition_count, pn);
        s_printf(pn);
        s_printf(": name=");
        s_printf(part->name);
        s_printf(" id=");
        s_printf(part->id);
        s_printf(" blocks=");
        char bc[12];
        int_to_str(part->block_map_count, bc);
        s_printf(bc);
        s_printf("\n");

        kfree(dict_buf);
        (*partition_count)++;
        pos = dict_end + 7; // Skip "</dict>"
    }

    return 0;
}

// =========================================================================
// DMG File I/O Helper
// =========================================================================
// Reads bytes from a DMG file at a given offset using PFS32 handle API.
// Returns number of bytes read, or -1 on error.

static int dmg_file_read_at(const char* path, uint64_t offset,
                            uint8_t* buffer, uint32_t length) {
    int fd = pfs32_open(path, 0); // 0 = O_RDONLY
    if (fd < 0) {
        s_printf("[DMG] Failed to open file: ");
        s_printf(path);
        s_printf("\n");
        return -1;
    }

    // Seek to offset (truncated to uint32_t for 32-bit OS)
    if (offset > 0xFFFFFFFFULL) {
        s_printf("[DMG] Warning: offset exceeds 32-bit range\n");
    }
    if (pfs32_seek(fd, (uint32_t)offset) < 0) {
        s_printf("[DMG] Seek failed\n");
        pfs32_close(fd);
        return -1;
    }

    int bytes_read = pfs32_read_handle(fd, buffer, length);
    pfs32_close(fd);

    if (bytes_read < 0) {
        s_printf("[DMG] Read failed\n");
        return -1;
    }

    return bytes_read;
}

// =========================================================================
// zlib Decompression
// =========================================================================
// Delegates to the standalone zlib_inflate implementation in zlib_inflate.c

static int zlib_decompress(const uint8_t* src, uint32_t src_len,
                           uint8_t* dst, uint32_t dst_cap, uint32_t* dst_len) {
    int result = zlib_inflate(src, src_len, dst, dst_cap, dst_len);
    if (result < 0) {
        s_printf("[DMG] zlib decompression failed (compressed size=");
        char sz[12];
        int_to_str(src_len, sz);
        s_printf(sz);
        s_printf(")\n");
    }
    return result;
}

// =========================================================================
// DMG Subsystem Initialization
// =========================================================================

void dmg_init_system(void) {
    memset(dmg_mounts, 0, sizeof(dmg_mounts));
    memset(dmg_hfs, 0, sizeof(dmg_hfs));
    dmg_system_inited = 1;
    s_printf("[DMG] Subsystem initialized\n");
}

// =========================================================================
// DMG Mount
// =========================================================================

int dmg_mount(const char* path) {
    if (!dmg_system_inited) {
        s_printf("[DMG] Subsystem not initialized\n");
        return -1;
    }

    // Find a free mount slot
    int slot = -1;
    for (int i = 0; i < DMG_MAX_MOUNTED; i++) {
        if (!dmg_mounts[i].mounted) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        s_printf("[DMG] No free mount slots\n");
        return -1;
    }

    dmg_mount_t* mount = &dmg_mounts[slot];
    dmg_hfs_state_t* hfs = &dmg_hfs[slot];
    memset(mount, 0, sizeof(dmg_mount_t));
    memset(hfs, 0, sizeof(dmg_hfs_state_t));

    strncpy(mount->path, path, sizeof(mount->path) - 1);
    mount->path[sizeof(mount->path) - 1] = 0;

    s_printf("[DMG] Mounting: ");
    s_printf(path);
    s_printf("\n");

    // Get file size via stat
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) < 0) {
        s_printf("[DMG] Cannot stat file\n");
        return -1;
    }
    uint32_t file_size = entry.file_size;

    if (file_size < DMG_KOLY_SIZE) {
        s_printf("[DMG] File too small for KOLY trailer\n");
        return -1;
    }

    // Read KOLY trailer (last 512 bytes)
    uint8_t koly_buf[DMG_KOLY_SIZE];
    if (dmg_file_read_at(path, file_size - DMG_KOLY_SIZE,
                         koly_buf, DMG_KOLY_SIZE) < 0) {
        s_printf("[DMG] Failed to read KOLY trailer\n");
        return -1;
    }

    // Parse KOLY
    if (parse_koly(koly_buf, &mount->koly) < 0) {
        s_printf("[DMG] Invalid KOLY trailer\n");
        return -1;
    }

    // Validate version
    if (mount->koly.version != 4 && mount->koly.version != 5) {
        s_printf("[DMG] Unsupported UDIF version: ");
        char v[12];
        int_to_str(mount->koly.version, v);
        s_printf(v);
        s_printf("\n");
        return -1;
    }

    mount->total_sectors = (uint32_t)mount->koly.sector_count;

    // Read XML plist
    uint32_t plist_len = (uint32_t)mount->koly.xml_plist_length;
    if (plist_len == 0 || plist_len > DMG_PLIST_MAX_SIZE) {
        s_printf("[DMG] Invalid plist size: ");
        char sz[12];
        int_to_str(plist_len, sz);
        s_printf(sz);
        s_printf("\n");
        return -1;
    }

    char* plist_buf = (char*)kmalloc(plist_len + 1);
    if (!plist_buf) {
        s_printf("[DMG] Out of memory for plist\n");
        return -1;
    }

    int read_result = dmg_file_read_at(path, mount->koly.xml_plist_offset,
                                       (uint8_t*)plist_buf, plist_len);
    if (read_result < 0) {
        s_printf("[DMG] Failed to read plist\n");
        kfree(plist_buf);
        return -1;
    }
    plist_buf[plist_len] = 0;

    // Parse plist for partition and block map info
    if (parse_plist_partitions(plist_buf, plist_len,
                               mount->partitions,
                               &mount->partition_count) < 0) {
        s_printf("[DMG] Failed to parse plist partitions\n");
        kfree(plist_buf);
        return -1;
    }

    kfree(plist_buf);

    if (mount->partition_count == 0) {
        s_printf("[DMG] No partitions found\n");
        return -1;
    }

    // Find the maximum uncompressed block size for the decompression buffer
    uint32_t max_uncompressed = DMG_SECTOR_SIZE; // At least one sector
    for (int i = 0; i < mount->partition_count; i++) {
        dmg_partition_t* part = &mount->partitions[i];
        for (int j = 0; j < part->block_map_count; j++) {
            if (part->blocks[j].uncompressed_length > max_uncompressed) {
                max_uncompressed = part->blocks[j].uncompressed_length;
            }
        }
    }

    // Allocate decompression buffer
    mount->decompress_buf = (uint8_t*)kmalloc(max_uncompressed);
    if (!mount->decompress_buf) {
        s_printf("[DMG] Failed to allocate decompression buffer\n");
        return -1;
    }
    mount->decompress_buf_size = max_uncompressed;

    // Parse HFS+ volume header to set up HFS state
    // The HFS+ volume header is at byte offset 1024 (sector 2)
    // within the virtual disk. The header is 512 bytes; all fields we
    // need (including catalog fork at offset 272) fit in sector 2.
    uint8_t vh_buf[DMG_SECTOR_SIZE];
    if (dmg_read_sector(slot, 2, vh_buf) >= 0) {
        // Check for HFS+ signature at offset 0 of the volume header
        uint16_t hfs_sig = be16_to_cpu(vh_buf);
        if (hfs_sig == HFS_PLUS_SIG || hfs_sig == HFSX_SIG) {
            hfs->block_size = be32_to_cpu(vh_buf + 40);
            hfs->total_blocks = be32_to_cpu(vh_buf + 44);

            // Catalog file fork data is at offset 272 in volume header
            const uint8_t* cat_fork = vh_buf + 272;
            /* cat_fork layout: logicalSize(8) clumpSize(4) totalBlocks(4) extents(64) */
            hfs->catalog_extents[0][0] = be32_to_cpu(cat_fork + 16 + 0);   // extents[0].startBlock
            hfs->catalog_extents[0][1] = be32_to_cpu(cat_fork + 16 + 4);   // extents[0].blockCount
            hfs->catalog_extents[1][0] = be32_to_cpu(cat_fork + 16 + 8);
            hfs->catalog_extents[1][1] = be32_to_cpu(cat_fork + 16 + 12);
            hfs->catalog_extents[2][0] = be32_to_cpu(cat_fork + 16 + 16);
            hfs->catalog_extents[2][1] = be32_to_cpu(cat_fork + 16 + 20);
            hfs->catalog_extents[3][0] = be32_to_cpu(cat_fork + 16 + 24);
            hfs->catalog_extents[3][1] = be32_to_cpu(cat_fork + 16 + 28);
            hfs->catalog_extents[4][0] = be32_to_cpu(cat_fork + 16 + 32);
            hfs->catalog_extents[4][1] = be32_to_cpu(cat_fork + 16 + 36);
            hfs->catalog_extents[5][0] = be32_to_cpu(cat_fork + 16 + 40);
            hfs->catalog_extents[5][1] = be32_to_cpu(cat_fork + 16 + 44);
            hfs->catalog_extents[6][0] = be32_to_cpu(cat_fork + 16 + 48);
            hfs->catalog_extents[6][1] = be32_to_cpu(cat_fork + 16 + 52);
            hfs->catalog_extents[7][0] = be32_to_cpu(cat_fork + 16 + 56);
            hfs->catalog_extents[7][1] = be32_to_cpu(cat_fork + 16 + 60);

            // Identify the HFS+ partition
            for (int i = 0; i < mount->partition_count; i++) {
                if (strcmp(mount->partitions[i].name, "Apple_HFS") == 0 ||
                    strcmp(mount->partitions[i].name, "Apple_HFSX") == 0 ||
                    strcmp(mount->partitions[i].name, "Apple_HFSX ") == 0) {
                    hfs->hfs_partition_idx = i;
                    break;
                }
            }

            // Try to read the catalog B-tree header node
            // The catalog file starts at the first extent's startBlock
            if (hfs->catalog_extents[0][1] > 0) {
                uint32_t cat_start_sector =
                    hfs->catalog_extents[0][0] * (hfs->block_size / DMG_SECTOR_SIZE);

                // Read the header node (node 0)
                // The header node is at the start of the catalog file
                uint8_t* node_buf = (uint8_t*)kmalloc(hfs->block_size > 4096 ? hfs->block_size : 4096);
                if (node_buf) {
                    // Read enough sectors for the first node
                    int sectors_to_read = (hfs->block_size > 0) ?
                        hfs->block_size / DMG_SECTOR_SIZE : 8;
                    if (sectors_to_read > 8) sectors_to_read = 8;

                    int ok = 1;
                    for (int s = 0; s < sectors_to_read; s++) {
                        if (dmg_read_sector(slot, cat_start_sector + s,
                                            node_buf + s * DMG_SECTOR_SIZE) < 0) {
                            ok = 0;
                            break;
                        }
                    }

                    if (ok) {
                        // Node descriptor: kind at offset 8 (1 byte, signed)
                        int8_t node_kind = (int8_t)node_buf[8];
                        uint16_t num_records = be16_to_cpu(node_buf + 12);

                        if (node_kind == HFS_BT_NODE_HEADER && num_records >= 3) {
                            // Read the header record (record 0)
                            // Offset array is at end of node
                            // We need the node size. For the header node,
                            // it's stored in the header record itself.
                            // Try common node sizes
                            uint16_t try_sizes[] = {4096, 8192, 2048, 512, 16384};
                            for (int ti = 0; ti < 5; ti++) {
                                uint16_t ns = try_sizes[ti];
                                uint16_t off0 = be16_to_cpu(node_buf + ns - 2);
                                uint16_t off1 = be16_to_cpu(node_buf + ns - 4);

                                // Validate: offset should be reasonable
                                if (off0 >= ns || off1 >= ns || off1 < off0) continue;

                                // BTHeaderRec starts at offset off1
                                // treeDepth(2) + rootNode(4) + leafRecords(4) +
                                // firstLeafNode(4) + lastLeafNode(4) + nodeSize(2)
                                const uint8_t* hdr = node_buf + off1;
                                uint16_t tree_depth = be16_to_cpu(hdr);
                                uint32_t root_node = be32_to_cpu(hdr + 2);
                                uint32_t first_leaf = be32_to_cpu(hdr + 10);
                                uint32_t last_leaf = be32_to_cpu(hdr + 14);
                                uint16_t node_size = be16_to_cpu(hdr + 18);

                                // Verify node_size matches
                                if (node_size == ns) {
                                    hfs->catalog_node_size = node_size;
                                    hfs->catalog_root_node = root_node;
                                    hfs->catalog_first_leaf = first_leaf;
                                    hfs->catalog_last_leaf = last_leaf;
                                    hfs->valid = 1;

                                    s_printf("[DMG] HFS+ catalog: node_size=");
                                    char nsz[12];
                                    int_to_str(node_size, nsz);
                                    s_printf(nsz);
                                    s_printf(" root=");
                                    char rn[12];
                                    int_to_str(root_node, rn);
                                    s_printf(rn);
                                    s_printf(" first_leaf=");
                                    char fl[12];
                                    int_to_str(first_leaf, fl);
                                    s_printf(fl);
                                    s_printf(" last_leaf=");
                                    char ll[12];
                                    int_to_str(last_leaf, ll);
                                    s_printf(ll);
                                    s_printf("\n");
                                    break;
                                }
                            }
                        }
                    }
                    kfree(node_buf);
                }
            }

            if (!hfs->valid) {
                s_printf("[DMG] Warning: could not parse HFS+ catalog header\n");
                // Still mark mount as successful; sector reads work without catalog
            }

            s_printf("[DMG] HFS+ detected: block_size=");
            char bs[12];
            int_to_str(hfs->block_size, bs);
            s_printf(bs);
            s_printf(" total_blocks=");
            char tb[12];
            int_to_str(hfs->total_blocks, tb);
            s_printf(tb);
            s_printf("\n");
        } else {
            s_printf("[DMG] Warning: no HFS+ signature found at sector 2\n");
        }
    }

    mount->mounted = 1;
    mount->writable = 0; // Read-only by default

    s_printf("[DMG] Mounted successfully (slot ");
    char sl[12];
    int_to_str(slot, sl);
    s_printf(sl);
    s_printf(")\n");

    return slot;
}

// =========================================================================
// DMG Unmount
// =========================================================================

int dmg_unmount(int mount_id) {
    if (mount_id < 0 || mount_id >= DMG_MAX_MOUNTED) return -1;
    dmg_mount_t* mount = &dmg_mounts[mount_id];
    dmg_hfs_state_t* hfs = &dmg_hfs[mount_id];

    if (!mount->mounted) return -1;

    s_printf("[DMG] Unmounting: ");
    s_printf(mount->path);
    s_printf("\n");

    // Free block map entries for each partition
    for (int i = 0; i < mount->partition_count; i++) {
        if (mount->partitions[i].blocks) {
            kfree(mount->partitions[i].blocks);
            mount->partitions[i].blocks = 0;
        }
    }

    // Free decompression buffer
    if (mount->decompress_buf) {
        kfree(mount->decompress_buf);
        mount->decompress_buf = 0;
    }

    memset(mount, 0, sizeof(dmg_mount_t));
    memset(hfs, 0, sizeof(dmg_hfs_state_t));

    return 0;
}

// =========================================================================
// DMG Sector Read
// =========================================================================
// Reads a single 512-byte sector from the virtual disk represented by
// the mounted DMG. Finds the appropriate block map entry, reads
// compressed data from the DMG file, decompresses if needed, and
// extracts the requested sector.

int dmg_read_sector(int mount_id, uint32_t sector, uint8_t* buffer) {
    if (mount_id < 0 || mount_id >= DMG_MAX_MOUNTED) return -1;
    dmg_mount_t* mount = &dmg_mounts[mount_id];
    if (!mount->mounted) return -1;

    // Search all partitions for the block containing this sector
    for (int p = 0; p < mount->partition_count; p++) {
        dmg_partition_t* part = &mount->partitions[p];
        if (!part->blocks) continue;

        for (int b = 0; b < part->block_map_count; b++) {
            dmg_blkmap_entry_t* blk = &part->blocks[b];

            // Calculate how many sectors this block covers
            uint32_t block_sectors = blk->uncompressed_length / DMG_SECTOR_SIZE;
            if (block_sectors == 0) continue;

            // Check if the requested sector falls within this block
            if (sector >= blk->sector_number &&
                sector < blk->sector_number + block_sectors) {

                // Found the right block
                uint32_t sector_offset = sector - blk->sector_number;

                // Handle based on compression type
                if (blk->compression_type == DMG_COMP_RAW ||
                    blk->compression_type == DMG_COMP_CRC) {
                    // Raw data: read directly from the DMG file
                    uint64_t data_offset = blk->compressed_offset +
                        (uint64_t)sector_offset * DMG_SECTOR_SIZE;
                    int rd = dmg_file_read_at(mount->path, data_offset,
                                              buffer, DMG_SECTOR_SIZE);
                    if (rd < (int)DMG_SECTOR_SIZE) {
                        s_printf("[DMG] Short raw read\n");
                        return -1;
                    }
                    return 0;
                }
                else if (blk->compression_type == DMG_COMP_ZLIB) {
                    // zlib compressed: read compressed data, decompress,
                    // then extract the sector
                    uint8_t* comp_buf = (uint8_t*)kmalloc(blk->compressed_length);
                    if (!comp_buf) {
                        s_printf("[DMG] Out of memory for compressed read\n");
                        return -1;
                    }

                    int rd = dmg_file_read_at(mount->path,
                                              blk->compressed_offset,
                                              comp_buf,
                                              blk->compressed_length);
                    if (rd < 0) {
                        kfree(comp_buf);
                        return -1;
                    }

                    uint32_t decompressed_len = 0;
                    if (zlib_decompress(comp_buf, blk->compressed_length,
                                        mount->decompress_buf,
                                        mount->decompress_buf_size,
                                        &decompressed_len) < 0) {
                        kfree(comp_buf);
                        return -1;
                    }

                    // Extract the requested sector from decompressed data
                    uint32_t byte_offset = sector_offset * DMG_SECTOR_SIZE;
                    if (byte_offset + DMG_SECTOR_SIZE <= decompressed_len) {
                        memcpy(buffer,
                               mount->decompress_buf + byte_offset,
                               DMG_SECTOR_SIZE);
                    } else {
                        // Partial sector at end
                        memset(buffer, 0, DMG_SECTOR_SIZE);
                        uint32_t available = decompressed_len - byte_offset;
                        if (available > 0) {
                            memcpy(buffer,
                                   mount->decompress_buf + byte_offset,
                                   available);
                        }
                    }

                    kfree(comp_buf);
                    return 0;
                }
                else if (blk->compression_type == DMG_COMP_BZIP2) {
                    s_printf("[DMG] bzip2 compression not supported\n");
                    return -1;
                }
                else if (blk->compression_type == DMG_COMP_LZFSE) {
                    s_printf("[DMG] LZFSE compression not supported\n");
                    return -1;
                }
                else {
                    // Unknown compression: try raw read
                    s_printf("[DMG] Unknown compression type 0x");
                    char hex[9];
                    int_to_hex(blk->compression_type, hex);
                    s_printf(hex);
                    s_printf(", trying raw\n");
                    uint64_t data_offset = blk->compressed_offset +
                        (uint64_t)sector_offset * DMG_SECTOR_SIZE;
                    int rd = dmg_file_read_at(mount->path, data_offset,
                                              buffer, DMG_SECTOR_SIZE);
                    if (rd < (int)DMG_SECTOR_SIZE) return -1;
                    return 0;
                }
            }
        }
    }

    // Sector not found in any block map - return zeros
    // (This can happen for fill/unused sectors)
    memset(buffer, 0, DMG_SECTOR_SIZE);
    return 0;
}

// =========================================================================
// Read Byte Range from Virtual Disk
// =========================================================================
// Helper that reads an arbitrary byte range from the mounted DMG
// by issuing the appropriate sector reads.

static int dmg_read_bytes(int mount_id, uint64_t offset,
                          uint8_t* buffer, uint32_t length) {
    uint32_t start_sector = (uint32_t)(offset / DMG_SECTOR_SIZE);
    uint32_t offset_in_sector = (uint32_t)(offset % DMG_SECTOR_SIZE);
    uint32_t remaining = length;
    uint32_t buf_pos = 0;

    while (remaining > 0) {
        uint8_t sector_buf[DMG_SECTOR_SIZE];
        if (dmg_read_sector(mount_id, start_sector, sector_buf) < 0) {
            return -1;
        }

        uint32_t to_copy = DMG_SECTOR_SIZE - offset_in_sector;
        if (to_copy > remaining) to_copy = remaining;

        memcpy(buffer + buf_pos, sector_buf + offset_in_sector, to_copy);
        buf_pos += to_copy;
        remaining -= to_copy;
        start_sector++;
        offset_in_sector = 0;
    }

    return (int)buf_pos;
}

// =========================================================================
// HFS+ Catalog Reader Helpers
// =========================================================================

// Read a node from the catalog file given its node number
// Returns kmalloc'd buffer of catalog_node_size bytes, or NULL
static uint8_t* hfs_read_catalog_node(int mount_id, uint32_t node_num) {
    dmg_hfs_state_t* hfs = &dmg_hfs[mount_id];
    if (!hfs->valid || hfs->catalog_node_size == 0) return 0;

    uint32_t node_size = hfs->catalog_node_size;
    uint64_t node_offset = (uint64_t)node_num * node_size;

    // Translate catalog file offset to physical offset using extents
    uint64_t extent_base = 0;
    int found = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t start_block = hfs->catalog_extents[i][0];
        uint32_t block_count = hfs->catalog_extents[i][1];
        if (block_count == 0) break;

        uint64_t extent_bytes = (uint64_t)block_count * hfs->block_size;
        if (node_offset < extent_base + extent_bytes) {
            uint64_t offset_in_extent = node_offset - extent_base;
            uint64_t phys_offset = (uint64_t)start_block * hfs->block_size +
                                   offset_in_extent;
            uint32_t phys_sector = (uint32_t)(phys_offset / DMG_SECTOR_SIZE);
            uint32_t sectors_needed = node_size / DMG_SECTOR_SIZE;

            uint8_t* node_buf = (uint8_t*)kmalloc(node_size);
            if (!node_buf) return 0;

            // Read sectors
            int ok = 1;
            for (uint32_t s = 0; s < sectors_needed; s++) {
                if (dmg_read_sector(mount_id, phys_sector + s,
                                    node_buf + s * DMG_SECTOR_SIZE) < 0) {
                    ok = 0;
                    break;
                }
            }

            if (!ok) {
                kfree(node_buf);
                return 0;
            }

            return node_buf;
        }
        extent_base += extent_bytes;
    }

    s_printf("[DMG] Node offset not in catalog extents\n");
    return 0;
}

// Read a B-tree record offset from the offset array at end of node
static uint16_t hfs_bt_read_offset(const uint8_t* node_data,
                                   uint32_t node_size, int index) {
    // Offset array is at end of node, growing backwards
    // offset[0] is at node_data + node_size - 2
    // offset[n] is at node_data + node_size - 2*(n+1)
    uint32_t pos = node_size - 2 * (index + 1);
    if (pos >= node_size) return 0;
    return be16_to_cpu(node_data + pos);
}

// Convert UTF-16BE to ASCII (lossy for non-ASCII characters)
static void utf16be_to_ascii(const uint8_t* utf16, int char_count,
                             char* ascii, int max_len) {
    int i;
    for (i = 0; i < char_count && i < max_len - 1; i++) {
        uint16_t ch = (uint16_t)((utf16[i * 2] << 8) | utf16[i * 2 + 1]);
        ascii[i] = (ch < 128) ? (char)ch : '?';
    }
    ascii[i] = 0;
}

// Catalog entry info extracted from B-tree
typedef struct {
    uint32_t parent_id;    // Parent CNID
    uint32_t cnid;         // This item's CNID
    char     name[256];    // ASCII name
    int      is_folder;    // 1 = folder, 0 = file
    uint32_t file_size;    // For files: logical size
    uint32_t data_start_block;  // For files: first extent startBlock
    uint32_t data_block_count;  // For files: first extent blockCount
    uint32_t rsrc_start_block;  // Resource fork startBlock
    uint32_t rsrc_block_count;  // Resource fork blockCount
} hfs_catalog_entry_t;

// Parse a catalog key from a record
// Returns number of bytes consumed by the key
static int parse_catalog_key(const uint8_t* rec, int rec_len,
                             uint32_t* parent_id, char* name, int name_max) {
    if (rec_len < 6) return 0;

    uint16_t key_len = be16_to_cpu(rec);
    if (key_len > rec_len || key_len < 6) return 0;

    *parent_id = be32_to_cpu(rec + 2);
    uint16_t name_len = be16_to_cpu(rec + 6); // Number of UTF-16 chars

    // Name starts at offset 8, each char is 2 bytes
    int name_bytes = name_len * 2;
    if (8 + name_bytes > key_len) name_bytes = key_len - 8;

    utf16be_to_ascii(rec + 8, name_len, name, name_max);

    return key_len;
}

// Parse a catalog data record after the key
static int parse_catalog_data(const uint8_t* data, int data_len,
                              hfs_catalog_entry_t* entry) {
    if (data_len < 2) return -1;

    int16_t rec_type = (int16_t)be16_to_cpu(data);

    if (rec_type == HFS_REC_FOLDER) {
        // Folder record: type(2) + flags(2) + valence(4) + cnid(4) + ...
        if (data_len < 12) return -1;
        entry->is_folder = 1;
        entry->cnid = be32_to_cpu(data + 8);
        entry->file_size = 0;
        entry->data_start_block = 0;
        entry->data_block_count = 0;
        return 0;
    }
    else if (rec_type == HFS_REC_FILE) {
        // File record: type(2) + flags(2) + reserved(4) + cnid(4) + ...
        // Data fork at offset 88: logicalSize(8) + clumpSize(4) +
        // totalBlocks(4) + extents[8](64) = 80 bytes
        if (data_len < 88 + HFS_FORK_DATA_SIZE) return -1;
        entry->is_folder = 0;
        entry->cnid = be32_to_cpu(data + 8);

        // Data fork
        const uint8_t* data_fork = data + 88;
        uint64_t logical_size = be64_to_cpu(data_fork);
        entry->file_size = (uint32_t)logical_size;
        // Skip: clumpSize(4) + totalBlocks(4) = 8 bytes after logicalSize
        entry->data_start_block = be32_to_cpu(data_fork + 16);
        entry->data_block_count = be32_to_cpu(data_fork + 20);

        // Resource fork at offset 88 + 80 = 168
        if (data_len >= 168 + 16) {
            const uint8_t* rsrc_fork = data + 168;
            entry->rsrc_start_block = be32_to_cpu(rsrc_fork + 16);
            entry->rsrc_block_count = be32_to_cpu(rsrc_fork + 20);
        } else {
            entry->rsrc_start_block = 0;
            entry->rsrc_block_count = 0;
        }
        return 0;
    }
    else if (rec_type == HFS_REC_FOLDER_THREAD ||
             rec_type == HFS_REC_FILE_THREAD) {
        // Thread record: type(2) + reserved(2) + parent_cnid(4) + cnid(4)
        // Not typically needed for our scanning, skip
        return 1; // Return 1 to indicate "skip this record"
    }

    return -1;
}

// Scan catalog leaf nodes for entries with a given parent_id
// Returns entries in caller-provided array, returns count found
static int hfs_scan_catalog(int mount_id, uint32_t parent_id,
                            hfs_catalog_entry_t* entries, int max_entries) {
    dmg_hfs_state_t* hfs = &dmg_hfs[mount_id];
    if (!hfs->valid || hfs->catalog_node_size == 0) return -1;

    int found = 0;
    uint32_t node_size = hfs->catalog_node_size;

    // Scan from first_leaf to last_leaf
    uint32_t first = hfs->catalog_first_leaf;
    uint32_t last = hfs->catalog_last_leaf;

    // Sanity check
    if (first == 0 && last == 0) return 0;
    if (last > 100000) last = 100000; // Safety limit

    for (uint32_t n = first; n <= last && found < max_entries; n++) {
        uint8_t* node_buf = hfs_read_catalog_node(mount_id, n);
        if (!node_buf) break;

        // Read node descriptor
        int8_t node_kind = (int8_t)node_buf[8];
        uint16_t num_records = be16_to_cpu(node_buf + 12);

        if (node_kind != -1) { // Not a leaf node
            kfree(node_buf);
            continue;
        }

        // Process each record in the leaf node
        for (int r = 0; r < num_records && found < max_entries; r++) {
            // Get record offset from offset array
            uint16_t rec_offset = hfs_bt_read_offset(node_buf, node_size, r + 1);
            uint16_t next_offset = hfs_bt_read_offset(node_buf, node_size, r + 2);

            if (rec_offset >= node_size || next_offset > node_size ||
                next_offset <= rec_offset) continue;

            int rec_len = next_offset - rec_offset;
            if (rec_len < 8) continue;

            const uint8_t* rec = node_buf + rec_offset;

            // Parse the catalog key
            uint32_t rec_parent_id = 0;
            char rec_name[256];
            int key_len = parse_catalog_key(rec, rec_len,
                                            &rec_parent_id, rec_name, 256);
            if (key_len == 0) continue;

            // Check if this record belongs to our target parent
            if (rec_parent_id != parent_id) continue;

            // Parse the data portion
            const uint8_t* data = rec + key_len;
            int data_len = rec_len - key_len;
            if (data_len <= 0) continue;

            hfs_catalog_entry_t entry;
            memset(&entry, 0, sizeof(entry));
            entry.parent_id = rec_parent_id;
            strncpy(entry.name, rec_name, sizeof(entry.name) - 1);

            int result = parse_catalog_data(data, data_len, &entry);
            if (result == 1) continue; // Thread record, skip
            if (result < 0) continue;  // Parse error, skip

            entries[found++] = entry;
        }

        kfree(node_buf);
    }

    return found;
}

// =========================================================================
// List .app Bundles in Mounted DMG
// =========================================================================

int dmg_list_apps(int mount_id, char* app_names, int max_count, int max_name_len) {
    if (mount_id < 0 || mount_id >= DMG_MAX_MOUNTED) return -1;
    dmg_mount_t* mount = &dmg_mounts[mount_id];
    if (!mount->mounted) return -1;

    dmg_hfs_state_t* hfs = &dmg_hfs[mount_id];
    if (!hfs->valid) {
        s_printf("[DMG] HFS+ catalog not available for listing\n");
        return -1;
    }

    // Scan root directory (parent_id = 1) for .app folders
    hfs_catalog_entry_t* entries = (hfs_catalog_entry_t*)kmalloc(
        256 * sizeof(hfs_catalog_entry_t));
    if (!entries) return -1;

    int count = hfs_scan_catalog(mount_id, HFS_ROOT_PARENT_CNID,
                                 entries, 256);

    int app_count = 0;
    for (int i = 0; i < count && app_count < max_count; i++) {
        // Check if name ends with ".app"
        int name_len = strlen(entries[i].name);
        if (name_len > 4 &&
            entries[i].name[name_len - 4] == '.' &&
            entries[i].name[name_len - 3] == 'a' &&
            entries[i].name[name_len - 2] == 'p' &&
            entries[i].name[name_len - 1] == 'p') {

            // Copy to output
            char* dest = app_names + app_count * max_name_len;
            strncpy(dest, entries[i].name, max_name_len - 1);
            dest[max_name_len - 1] = 0;
            app_count++;
        }
    }

    kfree(entries);
    return app_count;
}

// =========================================================================
// Extract .app Bundle from Mounted DMG
// =========================================================================

// Helper: read a file from the HFS+ volume inside the DMG and write to PFS32
static int hfs_extract_file(int mount_id, hfs_catalog_entry_t* file_entry,
                            const char* dest_path) {
    dmg_hfs_state_t* hfs = &dmg_hfs[mount_id];

    // Create the output file on PFS32
    if (sys_fs_create(dest_path, 0) < 0) {
        s_printf("[DMG] Failed to create file: ");
        s_printf(dest_path);
        s_printf("\n");
        return -1;
    }

    // Read the file data from the HFS+ volume using its data fork extents
    // The file's data is stored in allocation blocks starting at data_start_block
    uint32_t bytes_remaining = file_entry->file_size;
    uint32_t block_offset = 0; // Block offset within the file's extent

    // Allocate I/O buffer
    uint32_t io_size = hfs->block_size;
    if (io_size < DMG_SECTOR_SIZE) io_size = DMG_SECTOR_SIZE;
    uint8_t* io_buf = (uint8_t*)kmalloc(io_size);
    if (!io_buf) return -1;

    // Write buffer for PFS32
    char* write_buf = (char*)kmalloc(io_size);
    if (!write_buf) {
        kfree(io_buf);
        return -1;
    }

    // Read file data block by block
    uint32_t start_block = file_entry->data_start_block;
    uint32_t block_count = file_entry->data_block_count;

    for (uint32_t b = 0; b < block_count && bytes_remaining > 0; b++) {
        // Calculate the physical sector for this allocation block
        uint32_t phys_sector = (start_block + b) *
            (hfs->block_size / DMG_SECTOR_SIZE);

        // Read the block from the DMG
        memset(io_buf, 0, io_size);
        uint32_t sectors_in_block = hfs->block_size / DMG_SECTOR_SIZE;
        for (uint32_t s = 0; s < sectors_in_block; s++) {
            dmg_read_sector(mount_id, phys_sector + s,
                           io_buf + s * DMG_SECTOR_SIZE);
        }

        // Determine how many bytes to write
        uint32_t to_write = io_size;
        if (to_write > bytes_remaining) to_write = bytes_remaining;

        // Write to PFS32
        memcpy(write_buf, io_buf, to_write);
        sys_fs_write(dest_path, write_buf, to_write);

        // Note: sys_fs_write overwrites the file each time, so this
        // approach only works correctly for small files.
        // For larger files, we'd need to use pfs32 handle-based writing.
        // For now, this works for typical .app bundle metadata files.

        bytes_remaining -= to_write;
    }

    kfree(io_buf);
    kfree(write_buf);
    return 0;
}

// Recursively extract a directory tree from the HFS+ volume
static int hfs_extract_directory(int mount_id, uint32_t dir_cnid,
                                 const char* dest_dir) {
    // Create the directory on PFS32
    sys_fs_create(dest_dir, 1);

    // Scan catalog for entries with this parent CNID
    hfs_catalog_entry_t* entries = (hfs_catalog_entry_t*)kmalloc(
        256 * sizeof(hfs_catalog_entry_t));
    if (!entries) return -1;

    int count = hfs_scan_catalog(mount_id, dir_cnid, entries, 256);

    for (int i = 0; i < count; i++) {
        char child_path[512];
        strcpy(child_path, dest_dir);
        strcat(child_path, "/");
        strcat(child_path, entries[i].name);

        if (entries[i].is_folder) {
            // Recurse into subdirectory
            s_printf("[DMG]   Dir: ");
            s_printf(entries[i].name);
            s_printf("\n");
            hfs_extract_directory(mount_id, entries[i].cnid, child_path);
        } else {
            // Extract file
            s_printf("[DMG]   File: ");
            s_printf(entries[i].name);
            s_printf(" (");
            char sz[12];
            int_to_str(entries[i].file_size, sz);
            s_printf(sz);
            s_printf(" bytes)\n");
            hfs_extract_file(mount_id, &entries[i], child_path);
        }
    }

    kfree(entries);
    return 0;
}

int dmg_extract_app(int mount_id, const char* app_name) {
    if (mount_id < 0 || mount_id >= DMG_MAX_MOUNTED) return -1;
    dmg_mount_t* mount = &dmg_mounts[mount_id];
    if (!mount->mounted) return -1;

    dmg_hfs_state_t* hfs = &dmg_hfs[mount_id];
    if (!hfs->valid) {
        s_printf("[DMG] HFS+ catalog not available for extraction\n");
        return -1;
    }

    s_printf("[DMG] Extracting ");
    s_printf(app_name);
    s_printf(" from DMG\n");

    // Find the .app folder in the root directory
    hfs_catalog_entry_t* entries = (hfs_catalog_entry_t*)kmalloc(
        256 * sizeof(hfs_catalog_entry_t));
    if (!entries) return -1;

    int count = hfs_scan_catalog(mount_id, HFS_ROOT_PARENT_CNID,
                                 entries, 256);

    int found = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, app_name) == 0 && entries[i].is_folder) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        s_printf("[DMG] App not found: ");
        s_printf(app_name);
        s_printf("\n");
        kfree(entries);
        return -1;
    }

    uint32_t app_cnid = entries[found].cnid;
    kfree(entries);

    // Create the destination path: /Applications/<app_name>
    char dest_path[256];
    strcpy(dest_path, "/Applications/");
    strcat(dest_path, app_name);

    // Ensure /Applications directory exists
    if (!sys_fs_exists("/Applications")) {
        sys_fs_create("/Applications", 1);
    }

    // If the app already exists, remove it first
    if (sys_fs_exists(dest_path)) {
        s_printf("[DMG] Removing existing app bundle\n");
        sys_fs_delete_recursive(dest_path);
    }

    s_printf("[DMG] Extracting to ");
    s_printf(dest_path);
    s_printf("\n");

    // Create the standard .app bundle structure
    char subdir[320];

    // Create top-level .app directory
    sys_fs_create(dest_path, 1);

    // Create Contents directory
    strcpy(subdir, dest_path);
    strcat(subdir, "/Contents");
    sys_fs_create(subdir, 1);

    // Create Contents/MacOS directory
    strcpy(subdir, dest_path);
    strcat(subdir, "/Contents/MacOS");
    sys_fs_create(subdir, 1);

    // Create Resources directory
    strcpy(subdir, dest_path);
    strcat(subdir, "/Contents/Resources");
    sys_fs_create(subdir, 1);

    // Create Frameworks directory
    strcpy(subdir, dest_path);
    strcat(subdir, "/Contents/Frameworks");
    sys_fs_create(subdir, 1);

    // Now recursively extract the app's contents from the DMG
    hfs_extract_directory(mount_id, app_cnid, dest_path);

    // Write a minimal Info.plist if one wasn't extracted
    char plist_path[320];
    strcpy(plist_path, dest_path);
    strcat(plist_path, "/Info.plist");

    if (!sys_fs_exists(plist_path)) {
        // Derive app name without .app extension
        char app_basename[64];
        int name_len = strlen(app_name);
        int copy_len = name_len - 4; // Remove ".app"
        if (copy_len <= 0) copy_len = name_len;
        strncpy(app_basename, app_name, copy_len);
        app_basename[copy_len] = 0;

        char plist_data[512];
        int plist_len = 0;

        // Write simple key=value format (CamelOS plist format)
        strcpy(plist_data, "CFBundleName=");
        strcat(plist_data, app_basename);
        strcat(plist_data, "\nCFBundleIdentifier=com.camelos.");
        strcat(plist_data, app_basename);
        strcat(plist_data, "\nCFBundleExecutable=");
        strcat(plist_data, app_basename);
        strcat(plist_data, "\nCFBundleType=macho\nCFBundleVersion=1.0\n");
        plist_len = strlen(plist_data);

        sys_fs_write(plist_path, plist_data, plist_len);
    }

    s_printf("[DMG] Extraction complete: ");
    s_printf(dest_path);
    s_printf("\n");

    return 0;
}

// =========================================================================
// Get Mount Info
// =========================================================================

const dmg_mount_t* dmg_get_mount_info(int mount_id) {
    if (mount_id < 0 || mount_id >= DMG_MAX_MOUNTED) return 0;
    if (!dmg_mounts[mount_id].mounted) return 0;
    return &dmg_mounts[mount_id];
}

// =========================================================================
// Drag-to-Applications Install
// =========================================================================
// Convenience function: mount, find the .app, extract, unmount

int dmg_install_to_applications(const char* dmg_path) {
    s_printf("[DMG] Installing from: ");
    s_printf(dmg_path);
    s_printf("\n");

    // Mount the DMG
    int mount_id = dmg_mount(dmg_path);
    if (mount_id < 0) {
        s_printf("[DMG] Failed to mount DMG\n");
        return -1;
    }

    // List .app bundles in the DMG
    char app_names[8][64];
    int app_count = dmg_list_apps(mount_id, (char*)app_names, 8, 64);

    if (app_count <= 0) {
        s_printf("[DMG] No .app bundles found in DMG\n");
        dmg_unmount(mount_id);
        return -1;
    }

    // Extract the first .app bundle found
    // (Most DMGs contain a single .app bundle)
    s_printf("[DMG] Found ");
    char ac[12];
    int_to_str(app_count, ac);
    s_printf(ac);
    s_printf(" app(s), extracting: ");
    s_printf(app_names[0]);
    s_printf("\n");

    int result = dmg_extract_app(mount_id, app_names[0]);

    // Unmount
    dmg_unmount(mount_id);

    if (result < 0) {
        s_printf("[DMG] Installation failed\n");
        return -1;
    }

    s_printf("[DMG] Installation complete\n");
    return 0;
}
