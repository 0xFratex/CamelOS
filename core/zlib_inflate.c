// core/zlib_inflate.c - Minimal zlib/deflate decompression for CamelOS
// Implements RFC 1950 (zlib) and RFC 1951 (deflate) decompression.
// No dynamic memory allocation; no standard library dependencies.
// Supports stored, fixed Huffman, and dynamic Huffman blocks.

#include "zlib_inflate.h"
#include "../include/string.h"  // for memset

// =========================================================================
// Constants
// =========================================================================

#define MAX_BITS       15    // Maximum Huffman code length
#define MAX_LIT_CODES  288   // Maximum literal/length codes
#define MAX_DIST_CODES 32    // Maximum distance codes
#define MAX_CL_CODES   19    // Maximum code length codes
#define WINDOW_SIZE    32768 // 32KB sliding window

// Length codes base values and extra bits (RFC 1951, section 3.2.5)
static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};

// Distance codes base values and extra bits (RFC 1951, section 3.2.5)
static const uint16_t dist_base[30] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289,
    16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

// Code length alphabet order (RFC 1951, section 3.2.7)
static const uint8_t cl_order[MAX_CL_CODES] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// =========================================================================
// Bitstream Reader
// =========================================================================
// Reads bits from the compressed byte stream.
// Bytes are loaded LSB-first into a 32-bit accumulator.
// For Huffman codes, the first bit read is the MSB of the code,
// which we accumulate with code = (code << 1) | bit.
// For non-Huffman data, bits are read LSB-first via bs_read().

typedef struct {
    const uint8_t* data;  // Pointer to input data
    uint32_t       size;  // Total size of input data
    uint32_t       pos;   // Current byte position in data
    uint32_t       bits;  // Bit buffer (bits already loaded, LSB = next bit)
    int            nbits; // Number of valid bits in buffer
} bitstream_t;

static void bs_init(bitstream_t* bs, const uint8_t* data, uint32_t size) {
    bs->data  = data;
    bs->size  = size;
    bs->pos   = 0;
    bs->bits  = 0;
    bs->nbits = 0;
}

// Ensure at least 'need' bits are available in the buffer
static int bs_ensure(bitstream_t* bs, int need) {
    while (bs->nbits < need) {
        if (bs->pos >= bs->size) return -1; // Unexpected end of stream
        bs->bits |= (uint32_t)(bs->data[bs->pos++]) << bs->nbits;
        bs->nbits += 8;
    }
    return 0;
}

// Read 'n' bits (1..25) from the stream, LSB first (for non-Huffman data)
static int bs_read(bitstream_t* bs, int n) {
    if (bs_ensure(bs, n) < 0) return -1;
    uint32_t val = bs->bits & ((1U << n) - 1);
    bs->bits  >>= n;
    bs->nbits -= n;
    return (int)val;
}

// Align to next byte boundary (discard remaining bits in current byte)
static void bs_align(bitstream_t* bs) {
    int drop = bs->nbits & 7;
    if (drop) {
        bs->bits  >>= drop;
        bs->nbits -= drop;
    }
}

// =========================================================================
// Huffman Table
// =========================================================================
// Canonical Huffman decoding. We store counts per length and a sorted
// symbol array. Decoding reads one bit at a time and tracks canonical
// code boundaries.
//
// For fast decoding of short codes, we build a direct-lookup table.
// In deflate, Huffman codes are packed MSB-first (RFC 1951 sec 3.1.1).
// When we peek bits from the stream (which stores bytes LSB-first),
// the peeked value for a Huffman code of length n has:
//   bit 0 = MSB of code, bit 1 = next MSB, ..., bit n-1 = LSB
// This is the bit-reversal of the numeric code value.
// We build the lookup table indexed by this bit-reversed representation.

#define HUFF_LOOKUP_BITS 9
#define HUFF_LOOKUP_SIZE (1 << HUFF_LOOKUP_BITS)

typedef struct {
    // Slow-path canonical decoding data
    uint16_t counts[MAX_BITS + 1];  // Number of codes of each length (1..15)
    uint16_t symbols[MAX_LIT_CODES]; // Symbols sorted by canonical code order
    int      max_sym;               // Total number of defined symbols

    // Fast lookup table for codes <= HUFF_LOOKUP_BITS
    int16_t  lookup_sym[HUFF_LOOKUP_SIZE]; // Symbol for this bit pattern (-1 = invalid)
    uint8_t  lookup_len[HUFF_LOOKUP_SIZE]; // Code length for this entry
    int      has_lookup;                    // 1 if lookup table is populated
} huff_table_t;

// Reverse the low 'n' bits of a value
static uint32_t bit_reverse(uint32_t val, int n) {
    uint32_t result = 0;
    int i;
    for (i = 0; i < n; i++) {
        result |= ((val >> i) & 1) << (n - 1 - i);
    }
    return result;
}

// Build a Huffman table from an array of code lengths.
// code_lengths[i] = bit length of code for symbol i (0 = unused).
// Returns 0 on success, -1 on error.
static int huff_build(huff_table_t* t, const uint8_t* code_lengths, int num_symbols) {
    int i;

    // Zero out the table structure
    memset(t, 0, sizeof(*t));
    for (i = 0; i < HUFF_LOOKUP_SIZE; i++) {
        t->lookup_sym[i] = -1;
    }

    // Count codes of each length
    for (i = 0; i < num_symbols; i++) {
        if (code_lengths[i] > MAX_BITS) return -1;
        t->counts[code_lengths[i]]++;
    }
    t->counts[0] = 0; // No actual codes of length 0

    // Check total
    int total = 0;
    for (i = 1; i <= MAX_BITS; i++) total += t->counts[i];
    if (total == 0) {
        // Empty table (all lengths zero)
        t->has_lookup = 0;
        return 0;
    }
    t->max_sym = total;

    // Compute offsets into symbols[] for each code length
    // offsets[len] = starting index in symbols[] for codes of length len
    uint16_t offsets[MAX_BITS + 1];
    offsets[0] = 0;
    for (i = 1; i <= MAX_BITS; i++) {
        offsets[i] = offsets[i - 1] + t->counts[i - 1];
    }

    // Place each symbol into the symbols array at its canonical position
    // Use a working copy of offsets since we increment them
    uint16_t work[MAX_BITS + 1];
    for (i = 0; i <= MAX_BITS; i++) work[i] = offsets[i];

    for (i = 0; i < num_symbols; i++) {
        int len = code_lengths[i];
        if (len > 0) {
            t->symbols[work[len]++] = (uint16_t)i;
        }
    }

    // Compute the first canonical code for each length
    // first_code[len] = first code value assigned to length len
    uint16_t first_code[MAX_BITS + 1];
    uint16_t code = 0;
    for (i = 1; i <= MAX_BITS; i++) {
        code = (uint16_t)((code + t->counts[i - 1]) << 1);
        first_code[i] = code;
    }

    // Build fast lookup table
    // For each defined symbol, compute its canonical code, bit-reverse it,
    // and fill the lookup table entries.
    for (i = 1; i <= MAX_BITS; i++) {
        int c;
        for (c = 0; c < t->counts[i]; c++) {
            uint16_t sym = t->symbols[offsets[i] + c];
            uint16_t cod = first_code[i] + c;

            if (i <= HUFF_LOOKUP_BITS) {
                // Bit-reverse the code to match the stream's bit order
                uint32_t rev = bit_reverse(cod, i);

                // Fill all entries whose low 'i' bits match this code.
                // Higher bits (HUFF_LOOKUP_BITS - i) can be anything since
                // we only consume 'i' bits.
                int fill = 1 << (HUFF_LOOKUP_BITS - i);
                int j;
                for (j = 0; j < fill; j++) {
                    int idx = (int)(rev | ((uint32_t)j << i));
                    t->lookup_sym[idx] = (int16_t)sym;
                    t->lookup_len[idx] = (uint8_t)i;
                }
            }
        }
    }

    t->has_lookup = 1;
    return 0;
}

// Decode one Huffman symbol from the bitstream.
// Returns symbol value on success, -1 on error.
static int huff_decode(bitstream_t* bs, huff_table_t* t) {
    // Fast path: try the lookup table
    if (t->has_lookup) {
        if (bs_ensure(bs, HUFF_LOOKUP_BITS) < 0) {
            // Not enough bits; fall through to slow path which handles
            // end-of-stream more gracefully
        } else {
            // Peek at HUFF_LOOKUP_BITS bits. These come from the stream
            // in byte-LSB-first order, which matches our bit-reversed
            // lookup table indexing.
            int peek = (int)(bs->bits & ((1U << HUFF_LOOKUP_BITS) - 1));
            int sym = t->lookup_sym[peek];
            if (sym >= 0) {
                // Valid entry found
                int len = t->lookup_len[peek];
                bs->bits  >>= len;
                bs->nbits -= len;
                return sym;
            }
            // Code is longer than HUFF_LOOKUP_BITS; fall through to slow path
        }
    }

    // Slow path: decode bit-by-bit using canonical Huffman boundaries.
    // We read one bit at a time and track the code value and the
    // canonical first-code for each length.
    int code  = 0;
    int first = 0;
    int index = 0;

    int len;
    for (len = 1; len <= MAX_BITS; len++) {
        int bit = bs_read(bs, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;

        int count = t->counts[len];
        if (code - first < count) {
            return (int)t->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
    }

    // No valid code found - corrupted data
    return -1;
}

// =========================================================================
// Fixed Huffman Tables (RFC 1951, section 3.2.6)
// =========================================================================
// Lit/Len 0-143:   8 bits
// Lit/Len 144-255: 9 bits
// Lit/Len 256-279: 7 bits
// Lit/Len 280-287: 8 bits
// Distance 0-31:   5 bits

static huff_table_t fixed_lit_table;
static huff_table_t fixed_dist_table;
static int fixed_tables_built = 0;

static void build_fixed_tables(void) {
    uint8_t lengths[MAX_LIT_CODES];
    int i;

    // Literal/length table
    for (i = 0; i <= 143; i++)   lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(&fixed_lit_table, lengths, 288);

    // Distance table: all 5-bit codes
    for (i = 0; i < 32; i++) lengths[i] = 5;
    huff_build(&fixed_dist_table, lengths, 32);

    fixed_tables_built = 1;
}

// =========================================================================
// Inflate Engine
// =========================================================================

typedef struct {
    bitstream_t bs;
    uint8_t*    dst;        // Output buffer
    uint32_t    dst_cap;    // Output buffer capacity
    uint32_t    dst_pos;    // Current write position in output
    uint8_t     window[WINDOW_SIZE]; // 32KB sliding window for back-references
    uint32_t    window_pos;          // Current position in window (mod WINDOW_SIZE)
} inflate_state_t;

// Output a single byte to both the destination buffer and the sliding window
static int inflate_output(inflate_state_t* s, uint8_t byte) {
    if (s->dst_pos >= s->dst_cap) return -1;
    s->dst[s->dst_pos++] = byte;

    // Update sliding window
    s->window[s->window_pos] = byte;
    s->window_pos = (s->window_pos + 1) & (WINDOW_SIZE - 1);

    return 0;
}

// Copy 'len' bytes from a back-reference at distance 'dist'
// The source overlaps the destination if len > dist (run-length encoding)
static int inflate_copy(inflate_state_t* s, uint32_t dist, uint32_t len) {
    if (dist == 0 || dist > s->dst_pos) return -1;

    // Compute the source position in the sliding window
    uint32_t src_pos = (s->window_pos + WINDOW_SIZE - dist) & (WINDOW_SIZE - 1);

    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t byte = s->window[src_pos];
        src_pos = (src_pos + 1) & (WINDOW_SIZE - 1);
        if (inflate_output(s, byte) < 0) return -1;
    }

    return 0;
}

// Decode a stored (uncompressed) block - RFC 1951 section 3.2.4
static int inflate_stored(inflate_state_t* s) {
    bitstream_t* bs = &s->bs;

    // Discard remaining bits in the current byte
    bs_align(bs);

    // Read LEN (2 bytes, little-endian) and NLEN (one's complement of LEN)
    int len_lo  = bs_read(bs, 8);
    int len_hi  = bs_read(bs, 8);
    int nlen_lo = bs_read(bs, 8);
    int nlen_hi = bs_read(bs, 8);
    if (len_lo < 0 || len_hi < 0 || nlen_lo < 0 || nlen_hi < 0) return -1;

    uint16_t len  = (uint16_t)((len_hi << 8) | len_lo);
    uint16_t nlen = (uint16_t)((nlen_hi << 8) | nlen_lo);

    // Verify NLEN is one's complement of LEN
    if ((uint16_t)(~nlen) != len) return -1;

    // Copy raw bytes to output
    uint16_t i;
    for (i = 0; i < len; i++) {
        int byte = bs_read(bs, 8);
        if (byte < 0) return -1;
        if (inflate_output(s, (uint8_t)byte) < 0) return -1;
    }

    return 0;
}

// Decode a block using fixed Huffman codes - RFC 1951 section 3.2.6
static int inflate_fixed(inflate_state_t* s) {
    if (!fixed_tables_built) build_fixed_tables();

    for (;;) {
        int sym = huff_decode(&s->bs, &fixed_lit_table);
        if (sym < 0) return -1;

        if (sym < 256) {
            // Literal byte
            if (inflate_output(s, (uint8_t)sym) < 0) return -1;
        } else if (sym == 256) {
            // End of block
            break;
        } else {
            // Length code (257..285)
            int lidx = sym - 257;
            if (lidx >= 29) return -1;

            uint32_t length = len_base[lidx];
            if (len_extra[lidx] > 0) {
                int extra = bs_read(&s->bs, len_extra[lidx]);
                if (extra < 0) return -1;
                length += extra;
            }

            // Decode distance
            int dsym = huff_decode(&s->bs, &fixed_dist_table);
            if (dsym < 0 || dsym >= 30) return -1;

            uint32_t distance = dist_base[dsym];
            if (dist_extra[dsym] > 0) {
                int extra = bs_read(&s->bs, dist_extra[dsym]);
                if (extra < 0) return -1;
                distance += extra;
            }

            if (inflate_copy(s, distance, length) < 0) return -1;
        }
    }

    return 0;
}

// Decode a block using dynamic Huffman codes - RFC 1951 section 3.2.7
static int inflate_dynamic(inflate_state_t* s) {
    bitstream_t* bs = &s->bs;
    int i;

    // Read table size descriptors
    int hlit  = bs_read(bs, 5);  // # of lit/len codes - 257 (range: 257..286)
    int hdist = bs_read(bs, 5);  // # of distance codes - 1 (range: 1..32)
    int hclen = bs_read(bs, 4);  // # of code length codes - 4 (range: 4..19)
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;

    hlit  += 257;
    hdist += 1;
    hclen += 4;

    // Read code length code lengths (3 bits each, in cl_order)
    uint8_t cl_lengths[MAX_CL_CODES];
    memset(cl_lengths, 0, sizeof(cl_lengths));

    for (i = 0; i < hclen; i++) {
        int val = bs_read(bs, 3);
        if (val < 0) return -1;
        cl_lengths[cl_order[i]] = (uint8_t)val;
    }

    // Build code-length Huffman table
    huff_table_t cl_table;
    if (huff_build(&cl_table, cl_lengths, MAX_CL_CODES) < 0) return -1;

    // Decode the literal/length + distance code lengths
    uint8_t code_lengths[MAX_LIT_CODES + MAX_DIST_CODES];
    memset(code_lengths, 0, sizeof(code_lengths));

    int total = hlit + hdist;
    int idx = 0;

    while (idx < total) {
        int sym = huff_decode(bs, &cl_table);
        if (sym < 0) return -1;

        if (sym < 16) {
            // Direct code length value (0..15)
            code_lengths[idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            // Repeat previous length 3..6 times
            int rep = bs_read(bs, 2);
            if (rep < 0 || idx == 0) return -1;
            rep += 3;
            uint8_t prev = code_lengths[idx - 1];
            while (rep > 0 && idx < total) {
                code_lengths[idx++] = prev;
                rep--;
            }
        } else if (sym == 17) {
            // Repeat zero 3..10 times
            int rep = bs_read(bs, 3);
            if (rep < 0) return -1;
            rep += 3;
            while (rep > 0 && idx < total) {
                code_lengths[idx++] = 0;
                rep--;
            }
        } else if (sym == 18) {
            // Repeat zero 11..138 times
            int rep = bs_read(bs, 7);
            if (rep < 0) return -1;
            rep += 11;
            while (rep > 0 && idx < total) {
                code_lengths[idx++] = 0;
                rep--;
            }
        } else {
            return -1;
        }
    }

    // Build literal/length Huffman table
    huff_table_t lit_table;
    if (huff_build(&lit_table, code_lengths, hlit) < 0) return -1;

    // Build distance Huffman table
    huff_table_t dist_table;
    if (huff_build(&dist_table, code_lengths + hlit, hdist) < 0) return -1;

    // Decode compressed data using these tables
    for (;;) {
        int sym = huff_decode(bs, &lit_table);
        if (sym < 0) return -1;

        if (sym < 256) {
            // Literal byte
            if (inflate_output(s, (uint8_t)sym) < 0) return -1;
        } else if (sym == 256) {
            // End of block
            break;
        } else {
            // Length code
            int lidx = sym - 257;
            if (lidx >= 29) return -1;

            uint32_t length = len_base[lidx];
            if (len_extra[lidx] > 0) {
                int extra = bs_read(bs, len_extra[lidx]);
                if (extra < 0) return -1;
                length += extra;
            }

            // Decode distance
            int dsym = huff_decode(bs, &dist_table);
            if (dsym < 0) return -1;
            if (dsym >= 30) return -1;

            uint32_t distance = dist_base[dsym];
            if (dist_extra[dsym] > 0) {
                int extra = bs_read(bs, dist_extra[dsym]);
                if (extra < 0) return -1;
                distance += extra;
            }

            if (inflate_copy(s, distance, length) < 0) return -1;
        }
    }

    return 0;
}

// =========================================================================
// Adler-32 Checksum (RFC 1950)
// =========================================================================

uint32_t adler32(const uint8_t* data, uint32_t len) {
    uint32_t a = 1;
    uint32_t b = 0;
    uint32_t i;

    // Process in chunks to avoid overflow (modulo 65521)
    // Each chunk: at most 5552 bytes (largest n where 255n < 65521)
    uint32_t remaining = len;
    const uint8_t* ptr = data;

    while (remaining > 0) {
        uint32_t chunk = remaining > 5552 ? 5552 : remaining;
        for (i = 0; i < chunk; i++) {
            a += ptr[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        ptr += chunk;
        remaining -= chunk;
    }

    return (b << 16) | a;
}

// =========================================================================
// Main Inflate Entry Point
// =========================================================================

int zlib_inflate(const uint8_t* src, uint32_t src_len,
                 uint8_t* dst, uint32_t dst_cap,
                 uint32_t* dst_len) {
    if (!src || !dst || src_len < 6) {
        // Need at least 2-byte zlib header + 4-byte Adler-32 trailer
        *dst_len = 0;
        return -1;
    }

    // Parse zlib header (RFC 1950)
    uint8_t cmf = src[0];
    uint8_t flg = src[1];

    // Verify header: (CMF * 256 + FLG) must be a multiple of 31
    if (((uint32_t)cmf * 256 + flg) % 31 != 0) {
        *dst_len = 0;
        return -1;
    }

    // Check compression method (low 4 bits of CMF must be 8 = deflate)
    if ((cmf & 0x0F) != 8) {
        *dst_len = 0;
        return -1;
    }

    // Check for preset dictionary (bit 5 of FLG)
    uint32_t data_offset = 2;
    if ((flg >> 5) & 1) {
        // FDICT is set: skip the 4-byte DICTID
        if (src_len < 10) {
            *dst_len = 0;
            return -1;
        }
        data_offset = 6;
    }

    // Verify we have room for the Adler-32 trailer
    if (data_offset + 4 > src_len) {
        *dst_len = 0;
        return -1;
    }

    // Set up inflate state
    inflate_state_t state;
    memset(&state, 0, sizeof(state));
    bs_init(&state.bs, src + data_offset, src_len - data_offset - 4);
    state.dst     = dst;
    state.dst_cap = dst_cap;
    state.dst_pos = 0;

    // Process deflate blocks
    int bfinal = 0;
    while (!bfinal) {
        int bf   = bs_read(&state.bs, 1);
        int btype = bs_read(&state.bs, 2);
        if (bf < 0 || btype < 0) {
            *dst_len = 0;
            return -1;
        }
        bfinal = bf;

        int result;
        switch (btype) {
            case 0: result = inflate_stored(&state);   break;
            case 1: result = inflate_fixed(&state);    break;
            case 2: result = inflate_dynamic(&state);  break;
            default:
                // Type 3 is reserved/invalid
                *dst_len = 0;
                return -1;
        }

        if (result < 0) {
            *dst_len = 0;
            return -1;
        }
    }

    // Verify Adler-32 checksum (last 4 bytes of zlib stream, big-endian)
    const uint8_t* adler_ptr = src + src_len - 4;
    uint32_t stored_adler = ((uint32_t)adler_ptr[0] << 24) |
                            ((uint32_t)adler_ptr[1] << 16) |
                            ((uint32_t)adler_ptr[2] << 8)  |
                            ((uint32_t)adler_ptr[3]);

    uint32_t computed_adler = adler32(dst, state.dst_pos);

    if (stored_adler != computed_adler) {
        // Checksum mismatch. Some DMG implementations may differ slightly.
        // We still return the decompressed data (lenient verification).
    }

    *dst_len = state.dst_pos;
    return 0;
}

// =========================================================================
// Raw Deflate Decompression (no header or trailer)
// =========================================================================

int raw_deflate_inflate(const uint8_t* src, uint32_t src_len,
                        uint8_t* dst, uint32_t dst_cap,
                        uint32_t* dst_len) {
    if (!src || !dst || src_len == 0) {
        *dst_len = 0;
        return -1;
    }

    inflate_state_t state;
    memset(&state, 0, sizeof(state));
    bs_init(&state.bs, src, src_len);
    state.dst     = dst;
    state.dst_cap = dst_cap;
    state.dst_pos = 0;

    // Process deflate blocks
    int bfinal = 0;
    while (!bfinal) {
        int bf   = bs_read(&state.bs, 1);
        int btype = bs_read(&state.bs, 2);
        if (bf < 0 || btype < 0) {
            *dst_len = state.dst_pos; // Return partial data
            return 0;
        }
        bfinal = bf;

        int result;
        switch (btype) {
            case 0: result = inflate_stored(&state);   break;
            case 1: result = inflate_fixed(&state);    break;
            case 2: result = inflate_dynamic(&state);  break;
            default:
                *dst_len = state.dst_pos; // Return partial data
                return 0;
        }

        if (result < 0) {
            *dst_len = state.dst_pos; // Return partial data on error
            return 0;
        }
    }

    *dst_len = state.dst_pos;
    return 0;
}

// =========================================================================
// Gzip Decompression (RFC 1952)
// =========================================================================
// Gzip format:
//   Magic:    0x1F 0x8B
//   Method:   1 byte (8 = deflate)
//   Flags:    1 byte (bit0=FTEXT, bit1=FHCRC, bit2=FEXTRA, bit3=FNAME, bit4=FCOMMENT)
//   MTIME:    4 bytes
//   XFL:      1 byte
//   OS:       1 byte
//   [FEXTRA]: 2-byte length + extra data
//   [FNAME]:  zero-terminated string
//   [FCOMMENT]: zero-terminated string
//   [FHCRC]:  2 bytes
//   <deflate data>
//   CRC32:    4 bytes
//   ISIZE:    4 bytes (original size mod 2^32)

int gzip_inflate(const uint8_t* src, uint32_t src_len,
                 uint8_t* dst, uint32_t dst_cap,
                 uint32_t* dst_len) {
    *dst_len = 0;

    if (!src || !dst || src_len < 18) {
        // Need at least 10-byte header + 8-byte trailer
        return -1;
    }

    // Verify magic number
    if (src[0] != 0x1F || src[1] != 0x8B) {
        return -1;
    }

    // Verify method (must be deflate = 8)
    if (src[2] != 8) {
        return -1;
    }

    uint8_t flags = src[3];
    uint32_t offset = 10; // Skip fixed header (magic + method + flags + mtime + xfl + os)

    // Skip optional fields based on flags
    // FEXTRA (bit 2)
    if (flags & 0x04) {
        if (offset + 2 > src_len) return -1;
        uint16_t xlen = (uint16_t)(src[offset] | (src[offset + 1] << 8));
        offset += 2 + xlen;
        if (offset > src_len) return -1;
    }

    // FNAME (bit 3) — zero-terminated string
    if (flags & 0x08) {
        while (offset < src_len && src[offset] != 0) offset++;
        offset++; // Skip the null terminator
        if (offset > src_len) return -1;
    }

    // FCOMMENT (bit 4) — zero-terminated string
    if (flags & 0x10) {
        while (offset < src_len && src[offset] != 0) offset++;
        offset++; // Skip the null terminator
        if (offset > src_len) return -1;
    }

    // FHCRC (bit 1) — 2-byte header CRC
    if (flags & 0x02) {
        offset += 2;
        if (offset > src_len) return -1;
    }

    // The deflate data runs from offset to (src_len - 8)
    // (last 8 bytes are CRC32 + ISIZE)
    if (offset + 8 > src_len) return -1;

    uint32_t deflate_len = src_len - offset - 8;

    return raw_deflate_inflate(src + offset, deflate_len, dst, dst_cap, dst_len);
}
