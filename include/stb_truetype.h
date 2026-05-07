// stb_truetype.h - Minimal TrueType/OpenType font rendering for CamelOS
// Based on the public domain stb_truetype by Sean Barrett
// Adapted for freestanding kernel environment: no libc, no FPU, integer-only math
//
// This is a MINIMAL implementation providing:
//   stbtt_fontinfo       - font info structure
//   stbtt_InitFont()     - parse TTF header
//   stbtt_ScaleForPixelHeight() - compute scale factor (integer math)
//   stbtt_GetCodepointBitmap()  - render a codepoint to monochrome bitmap
//   stbtt_GetCodepointBitmapBox() - get bitmap dimensions without rendering
//   stbtt_GetCodepointHMetrics() - get horizontal advance width
//   stbtt_GetFontVMetrics()      - get font vertical metrics
//
// All arithmetic uses 26.6 fixed-point (TrueType native format) or pure integer.
// No floating point, no stdlib, no string.h.

#ifndef STB_TRUETYPE_H
#define STB_TRUETYPE_H

#include "types.h"

// ============================================================================
// Kernel memory functions (provided by CamelOS core)
// ============================================================================
extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);
extern void* memset(void* ptr, int value, size_t num);
extern void* memcpy(void* destination, const void* source, size_t num);

// ============================================================================
// Fixed-point helpers (26.6 format used by TrueType)
// 26.6 means: 26 bits integer, 6 bits fraction
// To convert int -> 26.6:  val << 6
// To convert 26.6 -> int:  val >> 6  (truncates) or (val + 32) >> 6  (rounds)
// ============================================================================
typedef int stbtt_fixed;  // 26.6 fixed-point

#define STBTT_FIX_SHIFT  6
#define STBTT_FIX_SCALE  (1 << STBTT_FIX_SHIFT)   // 64
#define STBTT_FIX_MASK   (STBTT_FIX_SCALE - 1)     // 63

// Round 26.6 fixed-point to nearest integer
static inline int stbtt_fixed_round(stbtt_fixed v) {
    return (v + 32) >> STBTT_FIX_SHIFT;
}

// Floor 26.6 fixed-point to integer
static inline int stbtt_fixed_floor(stbtt_fixed v) {
    return v >> STBTT_FIX_SHIFT;
}

// Ceil 26.6 fixed-point to integer
static inline int stbtt_fixed_ceil(stbtt_fixed v) {
    return (v + STBTT_FIX_MASK) >> STBTT_FIX_SHIFT;
}

// Multiply two 26.6 fixed-point values (result is 26.6)
static inline stbtt_fixed stbtt_fix_mul(stbtt_fixed a, stbtt_fixed b) {
    // (a * b) >> 6, but we need to avoid overflow
    // Use: ((int64_t)a * b) >> 6
    // Since we don't have int64_t guaranteed, use safe decomposition
    // For kernel use, values are typically small enough
    return ((long long)a * b) >> STBTT_FIX_SHIFT;
}

// Integer * 26.6 fixed-point -> 26.6
static inline stbtt_fixed stbtt_int_mul_fix(int a, stbtt_fixed b) {
    return a * b;
}

// 26.6 fixed-point / integer -> 26.6
static inline stbtt_fixed stbtt_fix_div_int(stbtt_fixed a, int b) {
    return a / b;
}

// Integer / integer -> 26.6 fixed-point
static inline stbtt_fixed stbtt_int_div_fix(int a, int b) {
    return (stbtt_fixed)(((long long)a << STBTT_FIX_SHIFT) / b);
}

// ============================================================================
// Big-endian byte reading helpers
// ============================================================================
static inline uint8_t stbtt_ttBYTE(const uint8_t* p) { return *p; }
static inline int8_t  stbtt_ttCHAR(const uint8_t* p) { return (int8_t)*p; }
static inline uint16_t stbtt_ttUSHORT(const uint8_t* p) { return p[0]*256 + p[1]; }
static inline int16_t  stbtt_ttSHORT(const uint8_t* p) { return (int16_t)(p[0]*256 + p[1]); }
static inline uint32_t stbtt_ttULONG(const uint8_t* p) {
    return (p[0]<<24) + (p[1]<<16) + (p[2]<<8) + p[3];
}
static inline int32_t  stbtt_ttLONG(const uint8_t* p) {
    return (int32_t)((p[0]<<24) + (p[1]<<16) + (p[2]<<8) + p[3]);
}

// ============================================================================
// stbtt_fontinfo - Parsed font information
// ============================================================================
typedef struct {
    const uint8_t* data;        // Pointer to the TTF file data (must remain valid)
    int            fontstart;   // Offset of the font data within the file
    int            numGlyphs;   // Number of glyphs in the font

    // Table offsets (cached for fast access)
    int            loca;        // Offset to 'loca' table
    int            head;        // Offset to 'head' table
    int            glyf;        // Offset to 'glyf' table
    int            hhea;        // Offset to 'hhea' table
    int            hmtx;        // Offset to 'hmtx' table
    int            kern;        // Offset to 'kern' table
    int            cmap;        // Offset to 'cmap' table

    // Parsed head values
    int            indexToLocFormat;  // 0 = short, 1 = long loca format
    int            unitsPerEm;        // Units per em square

    // Parsed hhea values
    int            ascent;
    int            descent;
    int            lineGap;

    // cmap format info
    int            cmap_type;         // Which cmap subtable format we found
    int            cmap_subtable;     // Offset to cmap subtable from fontstart
} stbtt_fontinfo;

// ============================================================================
// Glyph bitmap result
// ============================================================================
typedef struct {
    unsigned char* pixels;      // Monochrome bitmap (1 byte per pixel, 0 or 255)
    int            width;       // Bitmap width in pixels
    int            height;      // Bitmap height in pixels
    int            xoff;        // X offset from origin
    int            yoff;        // Y offset from origin (positive = up)
} stbtt_bitmap;

// ============================================================================
// Internal: Find a table in the TTF file
// ============================================================================
static int stbtt__find_table(const uint8_t* data, int fontstart, const char* tag) {
    // Offset table: 4 bytes numTables, then table directory entries
    // Each entry: 4-byte tag, 4-byte checksum, 4-byte offset, 4-byte length
    int num_tables = stbtt_ttUSHORT(data + fontstart + 4);
    int i;
    for (i = 0; i < num_tables; i++) {
        int entry = fontstart + 12 + i * 16;  // 12 = size of offset table
        if (data[entry] == tag[0] && data[entry+1] == tag[1] &&
            data[entry+2] == tag[2] && data[entry+3] == tag[3]) {
            return stbtt_ttULONG(data + entry + 8);  // offset
        }
    }
    return 0;  // Table not found
}

// ============================================================================
// stbtt_InitFont - Parse TTF file and populate fontinfo
// Returns 1 on success, 0 on failure
// ============================================================================
static int stbtt_InitFont(stbtt_fontinfo* info, const uint8_t* data, int fontstart) {
    uint32_t version;
    int cmap_offset;
    int i;

    if (!info || !data) return 0;

    memset(info, 0, sizeof(*info));
    info->data = data;
    info->fontstart = fontstart;

    // Check font version
    version = stbtt_ttULONG(data + fontstart);
    // 0x00010000 = TrueType, 'OTTO' = OpenType/CFF, 'true' = Apple TrueType
    if (version != 0x00010000 && version != 0x74727565 && version != 0x4F54544F)
        return 0;

    // Find required tables
    info->head = stbtt__find_table(data, fontstart, "head");
    info->glyf = stbtt__find_table(data, fontstart, "glyf");
    info->loca = stbtt__find_table(data, fontstart, "loca");
    info->hhea = stbtt__find_table(data, fontstart, "hhea");
    info->hmtx = stbtt__find_table(data, fontstart, "hmtx");
    info->kern = stbtt__find_table(data, fontstart, "kern");
    info->cmap = stbtt__find_table(data, fontstart, "cmap");

    // We absolutely need glyf, loca, and head
    if (!info->head || !info->glyf || !info->loca)
        return 0;

    // Parse 'head' table (offsets relative to table start)
    // head table layout:
    //   0:  version (4 bytes)
    //   4:  fontRevision (4 bytes)
    //   8:  checksumAdjustment (4 bytes)
    //  12:  magicNumber (4 bytes, should be 0x5F0F3CF5)
    //  16:  flags (2 bytes)
    //  18:  unitsPerEm (2 bytes)
    //  ...
    //  50:  indexToLocFormat (2 bytes)
    info->unitsPerEm = stbtt_ttUSHORT(data + info->head + 18);
    if (info->unitsPerEm == 0) info->unitsPerEm = 2048;  // fallback
    info->indexToLocFormat = stbtt_ttSHORT(data + info->head + 50);

    // Parse 'maxp' table for number of glyphs
    {
        int maxp = stbtt__find_table(data, fontstart, "maxp");
        if (maxp) {
            info->numGlyphs = stbtt_ttUSHORT(data + maxp + 4);
        } else {
            info->numGlyphs = 0;
        }
    }

    // Parse 'hhea' table
    if (info->hhea) {
        // hhea table layout:
        //   0:  version (4 bytes)
        //   4:  ascent (2 bytes, FWORD)
        //   6:  descent (2 bytes, FWORD)
        //   8:  lineGap (2 bytes, FWORD)
        //  10:  advanceWidthMax (2 bytes)
        //  ...
        //  34:  numOfLongHorMetrics (2 bytes)
        info->ascent  = stbtt_ttSHORT(data + info->hhea + 4);
        info->descent = stbtt_ttSHORT(data + info->hhea + 6);
        info->lineGap = stbtt_ttSHORT(data + info->hhea + 8);
    }

    // Parse 'cmap' table - find a suitable subtable
    info->cmap_subtable = 0;
    info->cmap_type = 0;
    if (info->cmap) {
        cmap_offset = info->cmap;
        // cmap table layout:
        //   0:  version (2 bytes)
        //   2:  numTables (2 bytes)
        // Then encoding records: 2-byte platformID, 2-byte encodingID, 4-byte offset
        int cmap_num = stbtt_ttUSHORT(data + cmap_offset + 2);

        // Prefer: platform 3 (Windows), encoding 1 (Unicode BMP)
        // Also accept: platform 0 (Unicode), any encoding
        // Also accept: platform 1 (Mac), encoding 0 (Roman)
        int found_subtable = 0;
        int found_platform = -1;

        for (i = 0; i < cmap_num; i++) {
            int rec = cmap_offset + 4 + i * 8;
            int platform = stbtt_ttUSHORT(data + rec);
            int encoding = stbtt_ttUSHORT(data + rec + 2);
            int sub_offset = stbtt_ttULONG(data + rec + 4);

            if (platform == 3 && encoding == 1 && found_platform < 3) {
                // Windows Unicode BMP - best choice
                found_subtable = cmap_offset + sub_offset;
                found_platform = 3;
                break;  // Take the first Windows Unicode BMP entry
            } else if (platform == 0 && found_platform < 2) {
                // Unicode platform
                found_subtable = cmap_offset + sub_offset;
                found_platform = 2;
            } else if (platform == 1 && encoding == 0 && found_platform < 1) {
                // Mac Roman
                found_subtable = cmap_offset + sub_offset;
                found_platform = 1;
            }
        }

        if (found_subtable) {
            info->cmap_subtable = found_subtable - fontstart;
            info->cmap_type = stbtt_ttUSHORT(data + found_subtable);
        }
    }

    return 1;
}

// ============================================================================
// stbtt_ScaleForPixelHeight - Compute scale factor (integer fixed-point)
// Returns the scale as a value where: pixel = (font_unit * scale) >> 16
// We use 16.16 fixed-point for the scale factor itself.
// ============================================================================
static int stbtt_ScaleForPixelHeight(stbtt_fontinfo* info, int pixels) {
    if (!info || info->unitsPerEm == 0) return 0;
    // scale = pixels / (ascent - descent)
    // But unitsPerEm is the full em size, and (ascent - descent) should equal unitsPerEm
    // Use: scale = (pixels << 16) / (ascent - descent)
    int em = info->ascent - info->descent;
    if (em <= 0) em = info->unitsPerEm;
    return (int)(((long long)pixels << 16) / em);
}

// ============================================================================
// Internal: Find glyph index for a Unicode codepoint
// ============================================================================
static int stbtt__find_glyph_index(stbtt_fontinfo* info, int codepoint) {
    const uint8_t* data = info->data;
    int cmap = info->cmap_subtable + info->fontstart;
    int format;

    if (!info->cmap_subtable) return 0;  // .notdef

    format = info->cmap_type;

    if (format == 0) {
        // Format 0: byte encoding table
        // 256-byte array mapping byte values to glyph indices
        if (codepoint < 0 || codepoint >= 256) return 0;
        return stbtt_ttBYTE(data + cmap + 6 + codepoint);
    }
    else if (format == 4) {
        // Format 4: segment mapping to delta values (most common for BMP)
        int segcount = stbtt_ttUSHORT(data + cmap + 6) >> 1;
        int search_range = stbtt_ttUSHORT(data + cmap + 8);
        int entry_selector = stbtt_ttUSHORT(data + cmap + 10);
        int range_shift = stbtt_ttUSHORT(data + cmap + 12) >> 1;

        int endCodes = cmap + 14;
        int startCodes = endCodes + 2 + segcount * 2;  // +2 for reservedPad
        int idDeltas = startCodes + segcount * 2;
        int idRangeOffsets = idDeltas + segcount * 2;

        // Binary search through segments
        int seg;
        int start = 0, end = segcount - 1;

        while (start <= end) {
            int mid = (start + end) >> 1;
            int end_code = stbtt_ttUSHORT(data + endCodes + mid * 2);
            if (codepoint <= end_code) {
                end = mid - 1;
            } else {
                start = mid + 1;
            }
        }

        // Check the segment we landed on
        seg = start;
        if (seg >= segcount) return 0;

        {
            int start_code = stbtt_ttUSHORT(data + startCodes + seg * 2);
            int end_code = stbtt_ttUSHORT(data + endCodes + seg * 2);

            if (codepoint < start_code || codepoint > end_code)
                return 0;  // Not in this segment

            int id_delta = stbtt_ttSHORT(data + idDeltas + seg * 2);
            int id_range_offset = stbtt_ttUSHORT(data + idRangeOffsets + seg * 2);

            if (id_range_offset == 0) {
                return (codepoint + id_delta) & 0xFFFF;
            } else {
                int offset = idRangeOffsets + seg * 2 + id_range_offset +
                             (codepoint - start_code) * 2;
                int glyph = stbtt_ttUSHORT(data + offset);
                if (glyph != 0) {
                    glyph = (glyph + id_delta) & 0xFFFF;
                }
                return glyph;
            }
        }
    }
    else if (format == 6) {
        // Format 6: trimmed table mapping
        int first = stbtt_ttUSHORT(data + cmap + 6);
        int count = stbtt_ttUSHORT(data + cmap + 8);
        if (codepoint >= first && codepoint < first + count) {
            return stbtt_ttUSHORT(data + cmap + 10 + (codepoint - first) * 2);
        }
        return 0;
    }
    else if (format == 12) {
        // Format 12: segmented coverage (for codepoints > U+FFFF)
        int ngroups = stbtt_ttULONG(data + cmap + 12);
        int i;
        for (i = 0; i < ngroups; i++) {
            int rec = cmap + 16 + i * 12;
            uint32_t start_code = stbtt_ttULONG(data + rec);
            uint32_t end_code = stbtt_ttULONG(data + rec + 4);
            uint32_t start_glyph = stbtt_ttULONG(data + rec + 8);
            if ((uint32_t)codepoint >= start_code && (uint32_t)codepoint <= end_code) {
                return (int)(start_glyph + (uint32_t)codepoint - start_code);
            }
        }
        return 0;
    }

    // Unsupported cmap format
    return 0;
}

// ============================================================================
// Internal: Get glyph offset in glyf table
// Returns 0 if glyph not found or has no outline
// ============================================================================
static int stbtt__get_glyph_offset(stbtt_fontinfo* info, int glyph_index) {
    int g1, g2;

    if (glyph_index < 0 || glyph_index >= info->numGlyphs)
        return 0;

    if (info->indexToLocFormat == 0) {
        // Short format: 2-byte offsets, multiply by 2
        g1 = stbtt_ttUSHORT(info->data + info->loca + glyph_index * 2) * 2;
        g2 = stbtt_ttUSHORT(info->data + info->loca + glyph_index * 2 + 2) * 2;
    } else {
        // Long format: 4-byte offsets
        g1 = stbtt_ttULONG(info->data + info->loca + glyph_index * 4);
        g2 = stbtt_ttULONG(info->data + info->loca + glyph_index * 4 + 4);
    }

    if (g1 == g2)
        return 0;  // Empty glyph (no outline, e.g. space)

    return info->glyf + g1;
}

// ============================================================================
// stbtt_GetCodepointHMetrics - Get horizontal advance width and left side bearing
// Values are in font units (not scaled)
// ============================================================================
static void stbtt_GetCodepointHMetrics(stbtt_fontinfo* info, int codepoint,
                                        int* advanceWidth, int* leftSideBearing) {
    int glyph_index = stbtt__find_glyph_index(info, codepoint);
    int numOfLongHorMetrics;

    if (!info->hhea || !info->hmtx) {
        if (advanceWidth) *advanceWidth = info->unitsPerEm;
        if (leftSideBearing) *leftSideBearing = 0;
        return;
    }

    numOfLongHorMetrics = stbtt_ttUSHORT(info->data + info->hhea + 34);

    if (glyph_index < numOfLongHorMetrics) {
        if (advanceWidth)
            *advanceWidth = stbtt_ttUSHORT(info->data + info->hmtx + glyph_index * 4);
        if (leftSideBearing)
            *leftSideBearing = stbtt_ttSHORT(info->data + info->hmtx + glyph_index * 4 + 2);
    } else {
        // Use last entry's advanceWidth, but this glyph's LSB
        if (advanceWidth)
            *advanceWidth = stbtt_ttUSHORT(info->data + info->hmtx + (numOfLongHorMetrics - 1) * 4);
        if (leftSideBearing) {
            int lsb_offset = numOfLongHorMetrics * 4 + (glyph_index - numOfLongHorMetrics) * 2;
            *leftSideBearing = stbtt_ttSHORT(info->data + info->hmtx + lsb_offset);
        }
    }
}

// ============================================================================
// stbtt_GetFontVMetrics - Get font vertical metrics (in font units)
// ============================================================================
static void stbtt_GetFontVMetrics(stbtt_fontinfo* info, int* ascent, int* descent, int* lineGap) {
    if (ascent)  *ascent  = info->ascent;
    if (descent) *descent = info->descent;
    if (lineGap) *lineGap = info->lineGap;
}

// ============================================================================
// Internal: Simple glyph rasterizer using active-edge scanline
// ============================================================================
// Maximum number of edges we can track per scanline
#define STBTT_MAX_EDGES 512

// An edge crossing a scanline
typedef struct {
    int x;          // X position in 26.6 fixed-point
    int direction;  // +1 or -1 (up or down winding)
} stbtt__edge;

// Insert sorted edge into list
static void stbtt__insert_edge(stbtt__edge* list, int* count, int max_count, int x, int dir) {
    int i;
    if (*count >= max_count) return;
    // Insertion sort by x
    i = *count;
    while (i > 0 && list[i-1].x > x) {
        list[i] = list[i-1];
        i--;
    }
    list[i].x = x;
    list[i].direction = dir;
    (*count)++;
}

// ============================================================================
// Internal: Rasterize a simple glyph outline to bitmap
// glyf_data points to the start of the glyph data (after the header)
// ============================================================================
static void stbtt__rasterize_simple_glyph(
    stbtt_fontinfo* info,
    const uint8_t* glyph_data, int glyph_offset,
    int scale_x16, int scale_y16,   // 16.16 fixed-point scale
    int ix0, int iy0,               // Pixel origin offset
    unsigned char* pixels, int pw, int ph,
    int xoff, int yoff)
{
    const uint8_t* data = info->data;
    int num_contours = stbtt_ttSHORT(glyph_data);
    // glyph_data + 2: xMin, yMin, xMax, yMax (each 2 bytes, FWORD)
    int end_pts_offset = 10;  // Offset from glyph_data to endPtsOfContours array
    int i, j;
    int instruction_len;
    int flag_offset;
    int x_coords_offset;
    int y_coords_offset;

    if (num_contours <= 0) return;  // Negative = compound glyph

    // Parse the glyph structure:
    // endPtsOfContours[num_contours]: 2 bytes each
    // instructionLength: 2 bytes
    // instructions[instructionLength]: variable
    // flags[]: variable length
    // xCoordinates[]: variable length
    // yCoordinates[]: variable length

    // Total number of points = last endPtsOfContours + 1
    int total_points = stbtt_ttUSHORT(glyph_data + end_pts_offset + (num_contours - 1) * 2) + 1;

    // Skip endPtsOfContours
    int offset = end_pts_offset + num_contours * 2;

    // Instructions
    instruction_len = stbtt_ttUSHORT(glyph_data + offset);
    offset += 2 + instruction_len;

    // Flags
    flag_offset = offset;

    // Decode flags into a temporary array
    uint8_t* flags = (uint8_t*)kmalloc(total_points);
    if (!flags) return;

    {
        int flag_i = 0;
        while (flag_i < total_points) {
            uint8_t f = glyph_data[offset++];
            flags[flag_i++] = f;
            if (f & 0x08) {
                // Repeat flag
                int rep = glyph_data[offset++];
                while (rep > 0 && flag_i < total_points) {
                    flags[flag_i++] = f;
                    rep--;
                }
            }
        }
    }

    // Decode X coordinates
    // X coords come right after flags
    x_coords_offset = offset;

    int* x_coords = (int*)kmalloc(total_points * sizeof(int));
    if (!x_coords) { kfree(flags); return; }

    {
        int x = 0;  // Delta from previous, accumulated
        for (i = 0; i < total_points; i++) {
            uint8_t f = flags[i];
            int dx;
            if (f & 0x02) {
                // 1-byte delta
                dx = glyph_data[offset++];
                if (!(f & 0x10)) dx = -dx;  // Negative
            } else if (f & 0x10) {
                // Same as previous
                dx = 0;
            } else {
                // 2-byte delta
                dx = stbtt_ttSHORT(glyph_data + offset);
                offset += 2;
            }
            x += dx;
            x_coords[i] = x;
        }
    }

    // Decode Y coordinates
    int* y_coords = (int*)kmalloc(total_points * sizeof(int));
    if (!y_coords) { kfree(flags); kfree(x_coords); return; }

    {
        int y = 0;
        for (i = 0; i < total_points; i++) {
            uint8_t f = flags[i];
            int dy;
            if (f & 0x04) {
                dy = glyph_data[offset++];
                if (!(f & 0x20)) dy = -dy;
            } else if (f & 0x20) {
                dy = 0;
            } else {
                dy = stbtt_ttSHORT(glyph_data + offset);
                offset += 2;
            }
            y += dy;
            y_coords[i] = y;
        }
    }

    // Now rasterize using scanline with non-zero winding rule
    // We process each contour and render it
    stbtt__edge* edges = (stbtt__edge*)kmalloc(STBTT_MAX_EDGES * sizeof(stbtt__edge));
    if (!edges) { kfree(flags); kfree(x_coords); kfree(y_coords); return; }

    // Find Y bounds in pixel space
    int min_y_pixel = iy0;
    int max_y_pixel = iy0 + ph - 1;

    // For each scanline
    for (int scan_y = min_y_pixel; scan_y <= max_y_pixel; scan_y++) {
        int edge_count = 0;

        // Process each contour
        int contour_start = 0;
        for (int c = 0; c < num_contours; c++) {
            int contour_end = stbtt_ttUSHORT(glyph_data + end_pts_offset + c * 2);

            // Flatten the contour to line segments (skip curves for simplicity in this minimal impl)
            // Walk the points in the contour
            int prev_on_curve = -1;
            int prev_x = 0, prev_y = 0;

            for (j = contour_start; j <= contour_end; j++) {
                uint8_t f = flags[j];
                int is_on_curve = (f & 0x01) != 0;

                // Scale coordinates to pixel space
                int px = (int)(((long long)x_coords[j] * scale_x16) >> 16);
                int py = (int)(((long long)y_coords[j] * scale_y16) >> 16);

                if (j == contour_start) {
                    prev_on_curve = is_on_curve;
                    prev_x = px;
                    prev_y = py;
                    continue;
                }

                // For a minimal implementation, treat all segments as lines
                // (curves are approximated by their endpoints)
                // This gives readable but slightly blocky results

                // Check if this edge crosses the scanline
                int y0 = prev_y;
                int y1 = py;

                // Convert scan_y to the same coordinate space
                // scan_y is in pixel space, y0/y1 are also in pixel space
                // Note: TrueType Y axis points up, screen Y points down
                // We need to flip: screen_y = -font_y + baseline_offset

                // We'll handle Y-flipping at the coordinate level
                // The edges go from y0 to y1 in TrueType space (up is positive)
                // In screen space (down is positive), we need to negate

                // For scanline intersection: check if the edge crosses scan_y
                int sy0 = -y0;  // Flip to screen space
                int sy1 = -y1;

                if ((sy0 <= scan_y && sy1 > scan_y) || (sy1 <= scan_y && sy0 > scan_y)) {
                    // Compute x intersection
                    int x_intersect;
                    if (sy1 != sy0) {
                        x_intersect = prev_x + (scan_y - sy0) * (px - prev_x) / (sy1 - sy0);
                    } else {
                        x_intersect = prev_x;
                    }

                    // Determine winding direction
                    int dir = (sy1 > sy0) ? 1 : -1;
                    stbtt__insert_edge(edges, &edge_count, STBTT_MAX_EDGES, x_intersect, dir);
                }

                prev_x = px;
                prev_y = py;
                prev_on_curve = is_on_curve;
            }

            // Close contour: line from last point to first point
            {
                int px0 = (int)(((long long)x_coords[contour_start] * scale_x16) >> 16);
                int py0 = (int)(((long long)y_coords[contour_start] * scale_y16) >> 16);
                int sy0 = -prev_y;
                int sy1 = -py0;

                if ((sy0 <= scan_y && sy1 > scan_y) || (sy1 <= scan_y && sy0 > scan_y)) {
                    int x_intersect;
                    if (sy1 != sy0) {
                        x_intersect = prev_x + (scan_y - sy0) * (px0 - prev_x) / (sy1 - sy0);
                    } else {
                        x_intersect = prev_x;
                    }
                    int dir = (sy1 > sy0) ? 1 : -1;
                    stbtt__insert_edge(edges, &edge_count, STBTT_MAX_EDGES, x_intersect, dir);
                }
            }

            contour_start = contour_end + 1;
        }

        // Fill pixels between edge pairs (non-zero winding rule)
        int winding = 0;
        for (i = 0; i < edge_count; i++) {
            winding += edges[i].direction;
            if (winding != 0 && i + 1 < edge_count) {
                // We're inside a filled region
                int x_start = edges[i].x;
                int x_end = edges[i + 1].x;

                // Skip if winding becomes 0 after the next edge
                int next_winding = winding + edges[i + 1].direction;

                // Fill from x_start to x_end
                int xs = x_start;
                int xe = x_end;
                if (xs > xe) { int t = xs; xs = xe; xe = t; }

                // Clip to bitmap bounds
                xs -= ix0;
                xe -= ix0;
                if (xs < 0) xs = 0;
                if (xe > pw) xe = pw;

                // Draw the scanline segment
                int bitmap_y = scan_y - min_y_pixel;
                if (bitmap_y >= 0 && bitmap_y < ph) {
                    for (int px = xs; px < xe; px++) {
                        pixels[bitmap_y * pw + px] = 255;
                    }
                }

                // Skip the next edge (it's the closing edge)
                if (next_winding == 0) {
                    i++;  // Skip closing edge
                    winding = 0;
                }
            }
        }
    }

    kfree(edges);
    kfree(y_coords);
    kfree(x_coords);
    kfree(flags);
}

// ============================================================================
// Internal: Rasterize a compound glyph (composed of multiple simple glyphs)
// ============================================================================
static void stbtt__rasterize_compound_glyph(
    stbtt_fontinfo* info,
    const uint8_t* glyph_data,
    int scale_x16, int scale_y16,
    int ix0, int iy0,
    unsigned char* pixels, int pw, int ph,
    int xoff, int yoff)
{
    // Compound glyph: numContours is -1 (already checked before calling)
    // Structure:
    //   10 bytes: header (numContours=0xFFFF, xMin, yMin, xMax, yMax)
    //   Then components:
    //     2 bytes: flags
    //     2 bytes: glyphIndex
    //     args... (depends on flags)
    //     optional transform

    int offset = 10;  // Skip header
    int has_more = 1;

    while (has_more) {
        uint16_t flags;
        int glyph_index;
        int arg1, arg2;
        int dx, dy;
        int a, b, c, d;  // Transform matrix (2.14 fixed-point for a,b,c,d)

        if (offset + 4 > 1024 * 64) break;  // Safety limit

        flags = stbtt_ttUSHORT(glyph_data + offset);
        glyph_index = stbtt_ttUSHORT(glyph_data + offset + 2);
        offset += 4;

        // Decode arguments
        if (flags & 0x0001) {
            // Args are 2-byte values
            arg1 = stbtt_ttSHORT(glyph_data + offset);
            arg2 = stbtt_ttSHORT(glyph_data + offset + 2);
            offset += 4;
        } else {
            arg1 = stbtt_ttCHAR(glyph_data + offset);
            arg2 = stbtt_ttCHAR(glyph_data + offset + 1);
            offset += 2;
        }

        // Determine if args are offsets or point numbers
        if (flags & 0x0002) {
            // Args are point numbers (we'll treat as offsets for simplicity)
            dx = arg1;
            dy = arg2;
        } else {
            // Args are x,y offsets
            dx = arg1;
            dy = arg2;
        }

        // Transform matrix (identity by default)
        a = 1 << 14;  // 1.0 in 2.14
        b = 0;
        c = 0;
        d = 1 << 14;

        if (flags & 0x0008) {
            // Has 2x2 transform
            a = stbtt_ttSHORT(glyph_data + offset);
            b = stbtt_ttSHORT(glyph_data + offset + 2);
            c = stbtt_ttSHORT(glyph_data + offset + 4);
            d = stbtt_ttSHORT(glyph_data + offset + 6);
            offset += 8;
        } else if (flags & 0x0040) {
            // Has single scale value
            a = stbtt_ttSHORT(glyph_data + offset);
            d = a;
            offset += 2;
        } else if (flags & 0x0080) {
            // Has separate X and Y scale values
            a = stbtt_ttSHORT(glyph_data + offset);
            d = stbtt_ttSHORT(glyph_data + offset + 2);
            offset += 4;
        }

        // Recursively rasterize the component glyph
        {
            int comp_offset = stbtt__get_glyph_offset(info, glyph_index);
            if (comp_offset) {
                const uint8_t* comp_data = info->data + comp_offset;
                int comp_num_contours = stbtt_ttSHORT(comp_data);

                // For compound glyphs in a compound, just render simple ones
                // Adjust position by dx, dy
                int saved_ix0 = ix0;
                int saved_iy0 = iy0;

                // Apply offset: dx, dy are in font units
                ix0 += (int)(((long long)dx * scale_x16) >> 16);
                iy0 -= (int)(((long long)dy * scale_y16) >> 16);  // Flip Y

                if (comp_num_contours > 0) {
                    stbtt__rasterize_simple_glyph(info, comp_data, comp_offset,
                                                  scale_x16, scale_y16,
                                                  ix0, iy0, pixels, pw, ph,
                                                  xoff, yoff);
                }

                ix0 = saved_ix0;
                iy0 = saved_iy0;
            }
        }

        has_more = (flags & 0x0020) != 0;  // MORE_COMPONENTS flag
    }
}

// ============================================================================
// stbtt_GetCodepointBitmapBox - Get bitmap dimensions without rendering
// Returns the bounding box in pixels
// ============================================================================
static void stbtt_GetCodepointBitmapBox(stbtt_fontinfo* info, int codepoint,
                                         int scale_x16, int scale_y16,
                                         int* ix0, int* iy0, int* ix1, int* iy1)
{
    int glyph_index = stbtt__find_glyph_index(info, codepoint);
    int glyph_offset = stbtt__get_glyph_offset(info, glyph_index);

    if (glyph_offset) {
        const uint8_t* glyph_data = info->data + glyph_offset;
        int xMin = stbtt_ttSHORT(glyph_data + 2);
        int yMin = stbtt_ttSHORT(glyph_data + 4);
        int xMax = stbtt_ttSHORT(glyph_data + 6);
        int yMax = stbtt_ttSHORT(glyph_data + 8);

        if (ix0) *ix0 = (int)(((long long)xMin * scale_x16) >> 16);
        if (iy0) *iy0 = -(int)(((long long)yMax * scale_y16) >> 16);  // Flip Y
        if (ix1) *ix1 = (int)(((long long)xMax * scale_x16) >> 16);
        if (iy1) *iy1 = -(int)(((long long)yMin * scale_y16) >> 16);  // Flip Y
    } else {
        if (ix0) *ix0 = 0;
        if (iy0) *iy0 = 0;
        if (ix1) *ix1 = 0;
        if (iy1) *iy1 = 0;
    }
}

// ============================================================================
// stbtt_GetCodepointBitmap - Render a codepoint to a monochrome bitmap
// Returns a kmalloc'd bitmap that the caller must kfree, or NULL on failure
// ============================================================================
static unsigned char* stbtt_GetCodepointBitmap(
    stbtt_fontinfo* info,
    int scale_x16, int scale_y16,  // 16.16 fixed-point scale
    int codepoint,
    int* out_width, int* out_height,
    int* out_xoff, int* out_yoff)
{
    int glyph_index = stbtt__find_glyph_index(info, codepoint);
    int glyph_offset = stbtt__get_glyph_offset(info, glyph_index);

    if (!glyph_offset) {
        // No outline for this glyph (e.g., space)
        if (out_width)  *out_width = 0;
        if (out_height) *out_height = 0;
        if (out_xoff)   *out_xoff = 0;
        if (out_yoff)   *out_yoff = 0;
        return NULL;
    }

    {
        const uint8_t* glyph_data = info->data + glyph_offset;
        int num_contours = stbtt_ttSHORT(glyph_data);
        int xMin = stbtt_ttSHORT(glyph_data + 2);
        int yMin = stbtt_ttSHORT(glyph_data + 4);
        int xMax = stbtt_ttSHORT(glyph_data + 6);
        int yMax = stbtt_ttSHORT(glyph_data + 8);

        int ix0, iy0, ix1, iy1;
        int pw, ph;
        unsigned char* pixels;

        // Compute pixel bounding box (flip Y for screen coordinates)
        ix0 = (int)(((long long)xMin * scale_x16) >> 16);
        iy0 = -(int)(((long long)yMax * scale_y16) >> 16);
        ix1 = (int)(((long long)xMax * scale_x16) >> 16);
        iy1 = -(int)(((long long)yMin * scale_y16) >> 16);

        // Add 1 pixel padding for anti-aliasing potential
        ix0--; iy0--;
        ix1++; iy1++;

        pw = ix1 - ix0;
        ph = iy1 - iy0;

        if (pw <= 0 || ph <= 0 || pw > 256 || ph > 256) {
            // Glyph too small or too large
            if (out_width)  *out_width = 0;
            if (out_height) *out_height = 0;
            if (out_xoff)   *out_xoff = 0;
            if (out_yoff)   *out_yoff = 0;
            return NULL;
        }

        pixels = (unsigned char*)kmalloc(pw * ph);
        if (!pixels) return NULL;
        memset(pixels, 0, pw * ph);

        if (num_contours > 0) {
            // Simple glyph
            stbtt__rasterize_simple_glyph(info, glyph_data, glyph_offset,
                                          scale_x16, scale_y16,
                                          ix0, iy0, pixels, pw, ph,
                                          ix0, iy0);
        } else if (num_contours == -1) {
            // Compound glyph
            stbtt__rasterize_compound_glyph(info, glyph_data,
                                            scale_x16, scale_y16,
                                            ix0, iy0, pixels, pw, ph,
                                            ix0, iy0);
        }

        if (out_width)  *out_width = pw;
        if (out_height) *out_height = ph;
        if (out_xoff)   *out_xoff = ix0;
        if (out_yoff)   *out_yoff = iy0;

        return pixels;
    }
}

// ============================================================================
// stbtt_GetCodepointBitmapBoxSubpixel - Same as BitmapBox but with subpixel shift
// ============================================================================
static void stbtt_GetCodepointBitmapBoxSubpixel(stbtt_fontinfo* info, int codepoint,
                                                  int scale_x16, int scale_y16,
                                                  int shift_x, int shift_y,
                                                  int* ix0, int* iy0, int* ix1, int* iy1)
{
    stbtt_GetCodepointBitmapBox(info, codepoint, scale_x16, scale_y16, ix0, iy0, ix1, iy1);
    if (ix0) *ix0 += (shift_x >> 6);
    if (iy0) *iy0 += (shift_y >> 6);
    if (ix1) *ix1 += (shift_x >> 6);
    if (iy1) *iy1 += (shift_y >> 6);
}

// ============================================================================
// stbtt_GetCodepointBitmapSubpixel - Render with subpixel positioning
// ============================================================================
static unsigned char* stbtt_GetCodepointBitmapSubpixel(
    stbtt_fontinfo* info,
    int scale_x16, int scale_y16,
    int shift_x, int shift_y,
    int codepoint,
    int* out_width, int* out_height,
    int* out_xoff, int* out_yoff)
{
    // For this minimal implementation, ignore subpixel shift
    return stbtt_GetCodepointBitmap(info, scale_x16, scale_y16, codepoint,
                                     out_width, out_height, out_xoff, out_yoff);
}

// ============================================================================
// stbtt_GetKerningTable - Get kerning between two codepoints (in font units)
// Returns 0 if no kerning or kern table not present
// ============================================================================
static int stbtt_GetCodepointKernAdvance(stbtt_fontinfo* info, int ch1, int ch2) {
    if (!info->kern) return 0;

    int g1 = stbtt__find_glyph_index(info, ch1);
    int g2 = stbtt__find_glyph_index(info, ch2);

    // Parse 'kern' table - format 0
    const uint8_t* data = info->data + info->kern;
    int version = stbtt_ttUSHORT(data);
    int nTables = stbtt_ttUSHORT(data + 2);

    // Microsoft-style kern table
    if (version == 0) {
        int i;
        for (i = 0; i < nTables; i++) {
            int subtable = 8 + i * 6;  // Each subtable header is 6 bytes
            // subtable header: version(2), length(2), coverage(2)
            int coverage = stbtt_ttUSHORT(data + subtable + 4);
            int format = (coverage >> 8);

            if (format == 0) {
                // Format 0: sorted list of kerning pairs
                int pair_count = stbtt_ttUSHORT(data + subtable + 6);
                int search_range = stbtt_ttUSHORT(data + subtable + 8);
                int entry_selector = stbtt_ttUSHORT(data + subtable + 10);
                int range_shift = stbtt_ttUSHORT(data + subtable + 12);

                int pairs_start = subtable + 14;

                // Binary search
                int lo = 0, hi = pair_count - 1;
                while (lo <= hi) {
                    int mid = (lo + hi) >> 1;
                    int pair = pairs_start + mid * 6;
                    int left = stbtt_ttUSHORT(data + pair);
                    int right = stbtt_ttUSHORT(data + pair + 2);
                    int combined = (g1 << 16) | g2;

                    if (left == g1 && right == g2) {
                        return stbtt_ttSHORT(data + pair + 4);
                    }

                    int pair_key = (left << 16) | right;
                    if (combined < pair_key) {
                        hi = mid - 1;
                    } else {
                        lo = mid + 1;
                    }
                }
            }
        }
    }

    return 0;
}

#endif // STB_TRUETYPE_H
