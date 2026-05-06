// jpeg_decoder.h - Minimal baseline JPEG decoder for CamelOS
// Self-contained, freestanding-capable decoder for baseline DCT JPEG (JFIF)
// Does NOT use stdlib, stdio, or any hosted library
// Uses extern kmalloc/kfree for memory, custom integer-only IDCT
//
// Supports: SOF0 (baseline DCT), DHT (Huffman tables), DQT (quantization tables),
//           SOS (start of scan), EOI, DRI (restart intervals), APP0 (JFIF)
// Output: ARGB pixel data (0xAARRGGBB)
#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H

#include "types.h"

// ============================================================================
// Error codes
// ============================================================================
#define JPEG_OK              0
#define JPEG_ERR_BAD_DATA   -1
#define JPEG_ERR_NO_MEMORY  -2
#define JPEG_ERR_NOT_JPEG   -3
#define JPEG_ERR_UNSUPPORTED -4

// ============================================================================
// Public API
// ============================================================================

// Initialize decoder with JPEG data buffer
// Returns JPEG_OK on success, negative error code on failure
int jpeg_decode_init(const uint8_t* data, int length);

// Decode to ARGB pixel buffer (caller provides buffer of width*height*4 bytes)
// out_width and out_height are set to the image dimensions
// Returns JPEG_OK on success
int jpeg_decode(uint32_t* output, int* out_width, int* out_height);

// Helper: decode JPEG from file path using sys_fs_read
// Returns allocated ARGB pixel buffer (caller must kfree), or NULL on error
// out_width and out_height are set to the image dimensions
uint32_t* jpeg_load_file(const char* path, int* out_width, int* out_height);

// ============================================================================
// Internal implementation (single-header style)
// ============================================================================

#ifdef JPEG_DECODER_IMPLEMENTATION

// Memory allocation - uses kernel allocator
extern void* kmalloc(unsigned int size);
extern void kfree(void* ptr);
extern int sys_fs_read(const char* path, char* buf, int max_len);

// Minimal memory functions for freestanding
static void _jpg_memset(void* dst, int val, unsigned int n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)val;
}

// ============================================================================
// JPEG constants
// ============================================================================
#define JPG_MAX_COMPS   4
#define JPG_MAX_HUFF    4
#define JPG_MAX_QUANT   4
#define JPG_MAX_W       4096
#define JPG_MAX_H       4096

// ============================================================================
// JPEG marker codes
// ============================================================================
#define MKR_SOI   0xD8
#define MKR_EOI   0xD9
#define MKR_SOF0  0xC0
#define MKR_SOF1  0xC1
#define MKR_SOF2  0xC2
#define MKR_DHT   0xC4
#define MKR_DQT   0xDB
#define MKR_DRI   0xDD
#define MKR_SOS   0xDA

// ============================================================================
// Internal types
// ============================================================================

typedef struct {
    uint8_t id;
    uint8_t h_samp, v_samp;
    uint8_t qt_id;
    uint8_t dc_ht, ac_ht;
} _jpg_comp;

typedef struct {
    uint8_t bits[17];     // bits[i] = count of codes of length i (1..16)
    uint8_t vals[256];    // symbol values in code-length order
    uint16_t maxcode[18]; // max code value for each length
    uint16_t mincode[18]; // min code value for each length
    uint8_t valoff[18];   // offset into vals[] for each length
} _jpg_huff;

typedef struct {
    uint8_t q[64];        // quantization values in zigzag order
} _jpg_quant;

typedef struct {
    const uint8_t* data;
    int len, pos;

    int width, height;
    int precision;
    int ncomp;
    _jpg_comp comp[JPG_MAX_COMPS];
    int max_h, max_v;     // max sampling factors
    int mcu_w, mcu_h;     // MCU size in pixels
    int mcus_x, mcus_y;

    _jpg_huff ht[JPG_MAX_HUFF];  // 0-1=DC, 2-3=AC
    _jpg_quant qt[JPG_MAX_QUANT];

    int restart_int;
    int restart_cnt;

    uint32_t bitbuf;
    int bitcnt;

    int prev_dc[JPG_MAX_COMPS];
} _jpg_dec;

static _jpg_dec _g_jpg;

// ============================================================================
// Zigzag table: natural order -> zigzag order
// Maps linear index (row-major) to zigzag position
// ============================================================================
static const uint8_t _zz[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// ============================================================================
// 1D IDCT basis matrix, scaled by 2^15 = 32768
// B[x][u] = C(u) * cos(pi*(2x+1)*u/16) * 32768
// where C(0) = 1/sqrt(2), C(u) = 1 for u > 0
//
// The 1D IDCT is: f(x) = (1/2) * sum_u C(u)*F(u)*cos(...)
// With this table: sum_u F(u)*B[x][u] = 2^16 * f(x)
// So after row+column passes: result = 2^32 * f(x,y)
// We use intermediate scaling: shift >> 16 after row pass, >> 16 after column pass
// ============================================================================
static const int16_t _idct_basis[8][8] = {
    { 23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170 },
    { 32138,  27245,  18204,   6392,  -6392, -18204, -27245, -32138 },
    { 30273,  12539, -12539, -30273, -30273, -12539,  12539,  30273 },
    { 27245,  -6392, -32138, -18204,  18204,  32138,   6392, -27245 },
    { 23170, -23170, -23170,  23170,  23170, -23170, -23170,  23170 },
    { 18204, -32138,   6392,  27245, -27245,  -6392,  32138, -18204 },
    { 12539, -30273,  30273, -12539, -12539,  30273, -30273,  12539 },
    {  6392, -18204,  27245, -32138,  32138, -27245,  18204,  -6392 },
};

// ============================================================================
// Bitstream reading
// ============================================================================

static int _jpg_next_byte(_jpg_dec* d) {
    if (d->pos >= d->len) return -1;
    int b = d->data[d->pos++];
    if (b == 0xFF) {
        if (d->pos >= d->len) return -1;
        int b2 = d->data[d->pos++];
        if (b2 == 0x00) return 0xFF;    // byte-stuffed FF
        if (b2 >= 0xD0 && b2 <= 0xD7) { // restart marker
            for (int i = 0; i < JPG_MAX_COMPS; i++) d->prev_dc[i] = 0;
            d->bitbuf = 0;
            d->bitcnt = 0;
            return _jpg_next_byte(d);
        }
        d->pos -= 2; // put back non-stuffed marker
        return -1;
    }
    return b;
}

static int _jpg_fill_bits(_jpg_dec* d, int n) {
    while (d->bitcnt < n) {
        int b = _jpg_next_byte(d);
        if (b < 0) return -1;
        d->bitbuf = (d->bitbuf << 8) | (uint32_t)b;
        d->bitcnt += 8;
    }
    return 0;
}

static int _jpg_get_bits(_jpg_dec* d, int n) {
    if (n == 0) return 0;
    if (_jpg_fill_bits(d, n) < 0) return 0;
    int val = (int)((d->bitbuf >> (d->bitcnt - n)) & ((1u << n) - 1));
    d->bitcnt -= n;
    d->bitbuf &= (1u << d->bitcnt) - 1;
    return val;
}

// ============================================================================
// Huffman decoding
// ============================================================================

static void _jpg_build_huff(_jpg_huff* h) {
    int code = 0, sym = 0;
    for (int i = 1; i <= 16; i++) {
        h->mincode[i] = (uint16_t)code;
        h->valoff[i] = (uint8_t)sym;
        h->maxcode[i] = (uint16_t)(code + h->bits[i] - 1);
        code += h->bits[i];
        sym += h->bits[i];
        code <<= 1;
    }
    h->mincode[17] = 0;
    h->maxcode[17] = 0xFFFF;
    h->valoff[17] = 0;
}

static int _jpg_huff_decode(_jpg_dec* d, _jpg_huff* h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int bit = _jpg_get_bits(d, 1);
        code = (code << 1) | bit;
        if (code <= h->maxcode[len]) {
            int idx = h->valoff[len] + (code - h->mincode[len]);
            if (idx < 0 || idx >= 256) return -1;
            return h->vals[idx];
        }
    }
    return -1;
}

static int _jpg_receive_extend(_jpg_dec* d, int n) {
    if (n == 0) return 0;
    int val = _jpg_get_bits(d, n);
    if (val < (1 << (n - 1)))
        val -= (1 << n) - 1;
    return val;
}

// ============================================================================
// 2D IDCT using separable 1D transforms with precomputed basis
// Input: block[64] in row-major order (de-zigzagged, dequantized)
// Output: uint8_t pixels at output[y*out_stride + x]
//
// Algorithm: two-pass separable IDCT
//   Row pass:  temp[x][v] = (sum_u F(u,v) * B[x][u] + 16384) >> 15
//   Col pass:  pixel = (sum_v temp[x][v] * B[y][v] + 16384) >> 15
//   Then add 128 for level shift and clamp to [0,255]
// ============================================================================
static void _jpg_idct(int32_t* blk, uint8_t* out, int stride) {
    int32_t tmp[64];
    int x, y, u, v;

    // Row pass: for each row v, compute 1D IDCT across columns
    for (v = 0; v < 8; v++) {
        // Fast path: if all AC coefficients are zero
        if (blk[v*8+1]==0 && blk[v*8+2]==0 && blk[v*8+3]==0 &&
            blk[v*8+4]==0 && blk[v*8+5]==0 && blk[v*8+6]==0 && blk[v*8+7]==0) {
            // DC only: f(x) = (1/2)*C(0)*F(0)*cos(0) for all x = F(0)/sqrt(2)/2 * ... 
            // With our basis: sum = F(0)*B[x][0], result = sum >> 15
            int32_t dc = blk[v*8];
            for (x = 0; x < 8; x++)
                tmp[v*8+x] = (dc * _idct_basis[x][0] + 16384) >> 15;
        } else {
            for (x = 0; x < 8; x++) {
                int32_t s = 0;
                for (u = 0; u < 8; u++)
                    s += blk[v*8+u] * _idct_basis[x][u];
                tmp[v*8+x] = (s + 16384) >> 15;
            }
        }
    }

    // Column pass: for each column x, compute 1D IDCT down rows
    for (x = 0; x < 8; x++) {
        if (tmp[8+x]==0 && tmp[16+x]==0 && tmp[24+x]==0 &&
            tmp[32+x]==0 && tmp[40+x]==0 && tmp[48+x]==0 && tmp[56+x]==0) {
            int32_t dc = tmp[x];
            for (y = 0; y < 8; y++) {
                int val = (dc * _idct_basis[y][0] + 16384) >> 15;
                val += 128;
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                out[y * stride + x] = (uint8_t)val;
            }
        } else {
            for (y = 0; y < 8; y++) {
                int32_t s = 0;
                for (v = 0; v < 8; v++)
                    s += tmp[v*8+x] * _idct_basis[y][v];
                int val = (s + 16384) >> 15;
                val += 128;
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                out[y * stride + x] = (uint8_t)val;
            }
        }
    }
}

// ============================================================================
// Marker parsing helpers
// ============================================================================

static uint16_t _rd16(_jpg_dec* d) {
    if (d->pos + 1 >= d->len) return 0;
    uint16_t v = ((uint16_t)d->data[d->pos] << 8) | d->data[d->pos+1];
    d->pos += 2;
    return v;
}

static void _skip_seg(_jpg_dec* d) {
    uint16_t len = _rd16(d);
    if (len >= 2) d->pos += len - 2;
}

// Parse DQT
static int _parse_dqt(_jpg_dec* d) {
    uint16_t len = _rd16(d);
    if (len < 2) return JPEG_ERR_BAD_DATA;
    len -= 2;
    while (len > 0) {
        if (d->pos >= d->len) return JPEG_ERR_BAD_DATA;
        uint8_t info = d->data[d->pos++];
        int prec = (info >> 4) & 0x0F;
        int tid = info & 0x0F;
        len--;
        if (tid >= JPG_MAX_QUANT) return JPEG_ERR_BAD_DATA;
        if (prec == 0) {
            if (len < 64) return JPEG_ERR_BAD_DATA;
            for (int i = 0; i < 64; i++)
                d->qt[tid].q[i] = d->data[d->pos++];
            len -= 64;
        } else {
            if (len < 128) return JPEG_ERR_BAD_DATA;
            for (int i = 0; i < 64; i++) {
                d->qt[tid].q[i] = d->data[d->pos+1]; // take low byte of 16-bit value
                d->pos += 2;
            }
            len -= 128;
        }
    }
    return JPEG_OK;
}

// Parse DHT
static int _parse_dht(_jpg_dec* d) {
    uint16_t len = _rd16(d);
    if (len < 2) return JPEG_ERR_BAD_DATA;
    len -= 2;
    while (len > 0) {
        if (d->pos >= d->len) return JPEG_ERR_BAD_DATA;
        uint8_t info = d->data[d->pos++];
        len--;
        int cls = (info >> 4) & 0x0F;  // 0=DC, 1=AC
        int tid = info & 0x0F;
        if (cls > 1 || tid > 1) {
            // Skip unsupported table
            int total = 0;
            for (int i = 0; i < 16; i++) { if (d->pos < d->len) total += d->data[d->pos+i]; }
            d->pos += 16 + total;
            len -= 16 + total;
            continue;
        }
        int idx = cls * 2 + tid;
        _jpg_huff* h = &d->ht[idx];
        _jpg_memset(h, 0, sizeof(_jpg_huff));
        if (len < 17) return JPEG_ERR_BAD_DATA;
        int total = 0;
        for (int i = 1; i <= 16; i++) {
            h->bits[i] = d->data[d->pos++];
            total += h->bits[i];
        }
        len -= 16;
        if (len < total) return JPEG_ERR_BAD_DATA;
        for (int i = 0; i < total; i++)
            h->vals[i] = d->data[d->pos++];
        len -= total;
        _jpg_build_huff(h);
    }
    return JPEG_OK;
}

// Parse SOF0
static int _parse_sof0(_jpg_dec* d) {
    uint16_t len = _rd16(d);
    if (len < 8) return JPEG_ERR_BAD_DATA;
    d->precision = d->data[d->pos++];
    if (d->precision != 8) return JPEG_ERR_UNSUPPORTED;
    d->height = (int)_rd16(d);
    d->width = (int)_rd16(d);
    d->ncomp = d->data[d->pos++];
    if (d->ncomp < 1 || d->ncomp > JPG_MAX_COMPS) return JPEG_ERR_UNSUPPORTED;
    if (d->width <= 0 || d->height <= 0 || d->width > JPG_MAX_W || d->height > JPG_MAX_H)
        return JPEG_ERR_BAD_DATA;

    d->max_h = 1; d->max_v = 1;
    for (int i = 0; i < d->ncomp; i++) {
        _jpg_comp* c = &d->comp[i];
        c->id = d->data[d->pos++];
        uint8_t sf = d->data[d->pos++];
        c->h_samp = (sf >> 4) & 0x0F;
        c->v_samp = sf & 0x0F;
        c->qt_id = d->data[d->pos++];
        if (c->h_samp > d->max_h) d->max_h = c->h_samp;
        if (c->v_samp > d->max_v) d->max_v = c->v_samp;
    }
    d->mcu_w = d->max_h * 8;
    d->mcu_h = d->max_v * 8;
    d->mcus_x = (d->width + d->mcu_w - 1) / d->mcu_w;
    d->mcus_y = (d->height + d->mcu_h - 1) / d->mcu_h;
    return JPEG_OK;
}

// Parse SOS
static int _parse_sos(_jpg_dec* d) {
    uint16_t len = _rd16(d);
    if (len < 6) return JPEG_ERR_BAD_DATA;
    int ns = d->data[d->pos++];
    if (ns < 1 || ns > JPG_MAX_COMPS) return JPEG_ERR_BAD_DATA;

    for (int i = 0; i < ns; i++) {
        int cid = d->data[d->pos++];
        uint8_t td = d->data[d->pos++];
        int dc_t = (td >> 4) & 0x0F;
        int ac_t = td & 0x0F;
        for (int j = 0; j < d->ncomp; j++) {
            if (d->comp[j].id == cid) {
                d->comp[j].dc_ht = dc_t;
                d->comp[j].ac_ht = ac_t;
                break;
            }
        }
    }
    // Skip Ss, Se, Ah, Al
    d->pos += 3;

    // Reset DC predictors and bitstream
    for (int i = 0; i < JPG_MAX_COMPS; i++) d->prev_dc[i] = 0;
    d->bitbuf = 0;
    d->bitcnt = 0;
    d->restart_cnt = d->restart_int;
    return JPEG_OK;
}

// Parse DRI
static int _parse_dri(_jpg_dec* d) {
    uint16_t len = _rd16(d);
    if (len < 4) return JPEG_ERR_BAD_DATA;
    d->restart_int = (int)_rd16(d);
    return JPEG_OK;
}

// Main marker parsing loop
static int _parse_markers(_jpg_dec* d) {
    if (d->pos + 1 >= d->len) return JPEG_ERR_BAD_DATA;
    if (d->data[d->pos] != 0xFF || d->data[d->pos+1] != MKR_SOI)
        return JPEG_ERR_NOT_JPEG;
    d->pos += 2;

    int sof_found = 0;
    while (d->pos + 1 < d->len) {
        // Find next marker
        while (d->pos < d->len && d->data[d->pos] != 0xFF) d->pos++;
        if (d->pos >= d->len) break;
        d->pos++; // skip 0xFF
        // Skip padding 0xFF bytes
        while (d->pos < d->len && d->data[d->pos] == 0xFF) d->pos++;
        if (d->pos >= d->len) break;

        uint8_t mkr = d->data[d->pos++];
        if (mkr == 0x00) continue; // not a marker

        switch (mkr) {
        case MKR_SOF0:
            if (_parse_sof0(d) != JPEG_OK) return JPEG_ERR_BAD_DATA;
            sof_found = 1;
            break;
        case MKR_DQT:
            if (_parse_dqt(d) != JPEG_OK) return JPEG_ERR_BAD_DATA;
            break;
        case MKR_DHT:
            if (_parse_dht(d) != JPEG_OK) return JPEG_ERR_BAD_DATA;
            break;
        case MKR_DRI:
            if (_parse_dri(d) != JPEG_OK) return JPEG_ERR_BAD_DATA;
            break;
        case MKR_SOS:
            if (!sof_found) return JPEG_ERR_BAD_DATA;
            return _parse_sos(d);
        case MKR_EOI:
            return JPEG_ERR_BAD_DATA;
        case MKR_SOF1: case MKR_SOF2:
            return JPEG_ERR_UNSUPPORTED;
        default:
            // APPn, COM, etc - skip
            _skip_seg(d);
            break;
        }
    }
    return JPEG_ERR_BAD_DATA;
}

// ============================================================================
// Block decoding: decode one 8x8 DCT block from bitstream
// Result is in row-major order (de-zigzagged), NOT yet dequantized
// ============================================================================
static int _decode_block(_jpg_dec* d, int ci, int32_t* blk) {
    _jpg_comp* c = &d->comp[ci];
    _jpg_huff* dc_h = &d->ht[c->dc_ht];
    _jpg_huff* ac_h = &d->ht[2 + c->ac_ht];

    _jpg_memset(blk, 0, 64 * sizeof(int32_t));

    // DC coefficient
    int dc_sz = _jpg_huff_decode(d, dc_h);
    if (dc_sz < 0) return JPEG_ERR_BAD_DATA;
    if (dc_sz > 11) return JPEG_ERR_BAD_DATA;
    int dc_diff = _jpg_receive_extend(d, dc_sz);
    d->prev_dc[ci] += dc_diff;
    blk[0] = d->prev_dc[ci];

    // AC coefficients
    int k = 1;
    while (k < 64) {
        int sym = _jpg_huff_decode(d, ac_h);
        if (sym < 0) return JPEG_ERR_BAD_DATA;
        if (sym == 0) break;  // EOB
        int run = (sym >> 4) & 0x0F;
        int sz = sym & 0x0F;
        if (sz == 0) {
            k += 16;  // ZRL
        } else {
            k += run;
            if (k >= 64) return JPEG_ERR_BAD_DATA;
            blk[_zz[k]] = _jpg_receive_extend(d, sz);
            k++;
        }
    }
    return JPEG_OK;
}

// ============================================================================
// YCbCr -> ARGB conversion (integer fixed-point, 2^16 scale)
// ============================================================================
static uint32_t _ycbcr_to_argb(int y, int cb, int cr) {
    int32_t cb_off = cb - 128;
    int32_t cr_off = cr - 128;

    // R = Y + 1.402*(Cr-128)
    int32_t r = y + ((91881 * cr_off + 32768) >> 16);
    // G = Y - 0.344136*(Cb-128) - 0.714136*(Cr-128)
    int32_t g = y - ((22554 * cb_off + 46802 * cr_off + 32768) >> 16);
    // B = Y + 1.772*(Cb-128)
    int32_t b = y + ((116130 * cb_off + 32768) >> 16);

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ============================================================================
// Decode scan data and produce ARGB output
// ============================================================================
static int _decode_scan(_jpg_dec* d, uint32_t* output) {
    int32_t blk[64];
    int mcu_px = d->mcu_w * d->mcu_h;

    // Allocate per-component MCU buffers
    uint8_t* cbuf[JPG_MAX_COMPS];
    for (int c = 0; c < d->ncomp; c++) {
        cbuf[c] = (uint8_t*)kmalloc(mcu_px);
        if (!cbuf[c]) {
            for (int j = 0; j < c; j++) kfree(cbuf[j]);
            return JPEG_ERR_NO_MEMORY;
        }
    }

    for (int my = 0; my < d->mcus_y; my++) {
        for (int mx = 0; mx < d->mcus_x; mx++) {
            // Handle restart intervals
            if (d->restart_int > 0 && d->restart_cnt == 0) {
                d->restart_cnt = d->restart_int;
                for (int i = 0; i < JPG_MAX_COMPS; i++) d->prev_dc[i] = 0;
                d->bitbuf = 0;
                d->bitcnt = 0;
                // Skip to next restart marker
                while (d->pos + 1 < d->len) {
                    if (d->data[d->pos] == 0xFF) {
                        int m = d->data[d->pos+1];
                        if (m >= 0xD0 && m <= 0xD7) {
                            d->pos += 2;
                            break;
                        }
                    }
                    d->pos++;
                }
            }
            if (d->restart_int > 0) d->restart_cnt--;

            // Decode each component's blocks
            for (int c = 0; c < d->ncomp; c++) {
                _jpg_comp* cp = &d->comp[c];
                int hb = cp->h_samp, vb = cp->v_samp;

                for (int vy = 0; vy < vb; vy++) {
                    for (int hx = 0; hx < hb; hx++) {
                        if (_decode_block(d, c, blk) != JPEG_OK)
                            _jpg_memset(blk, 0, 64 * sizeof(int32_t));

                        // Dequantize (multiply by quant table values)
                        uint8_t* q = d->qt[cp->qt_id].q;
                        for (int i = 0; i < 64; i++)
                            blk[i] *= (int32_t)q[i];

                        // IDCT: output 8x8 block into component buffer
                        int bx = hx * 8, by = vy * 8;
                        int stride = hb * 8;
                        _jpg_idct(blk, cbuf[c] + by * stride + bx, stride);
                    }
                }

                // Upsample chroma if needed (nearest-neighbor)
                if (hb < d->max_h || vb < d->max_v) {
                    int sw = hb * 8, sh = vb * 8;
                    int dw = d->max_h * 8, dh = d->max_v * 8;
                    // Use block as temp (it's 256 bytes, enough for 64x64)
                    // Actually we need a separate temp buffer
                    uint8_t* tmp = (uint8_t*)kmalloc(dw * dh);
                    if (tmp) {
                        for (int y = 0; y < dh; y++) {
                            int sy = y * sh / dh;
                            for (int x = 0; x < dw; x++) {
                                int sx = x * sw / dw;
                                tmp[y * dw + x] = cbuf[c][sy * sw + sx];
                            }
                        }
                        // Copy back to cbuf
                        for (int y = 0; y < dh; y++)
                            for (int x = 0; x < dw; x++)
                                cbuf[c][y * dw + x] = tmp[y * dw + x];
                        kfree(tmp);
                    }
                }
            }

            // Convert to ARGB pixels
            int px0 = mx * d->mcu_w, py0 = my * d->mcu_h;
            for (int py = 0; py < d->mcu_h; py++) {
                int oy = py0 + py;
                if (oy >= d->height) break;
                for (int px = 0; px < d->mcu_w; px++) {
                    int ox = px0 + px;
                    if (ox >= d->width) break;

                    uint32_t argb;
                    if (d->ncomp == 1) {
                        int g = cbuf[0][py * d->mcu_w + px];
                        argb = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
                    } else if (d->ncomp == 3) {
                        argb = _ycbcr_to_argb(
                            cbuf[0][py * d->mcu_w + px],
                            cbuf[1][py * d->mcu_w + px],
                            cbuf[2][py * d->mcu_w + px]);
                    } else {
                        // 4-component: assume CMYK
                        int cv = cbuf[0][py * d->mcu_w + px];
                        int mv = cbuf[1][py * d->mcu_w + px];
                        int yv = cbuf[2][py * d->mcu_w + px];
                        int kv = cbuf[3][py * d->mcu_w + px];
                        int r = 255 - ((cv * (255 - kv) + 127) / 255 + kv);
                        int g2 = 255 - ((mv * (255 - kv) + 127) / 255 + kv);
                        int b = 255 - ((yv * (255 - kv) + 127) / 255 + kv);
                        if (r < 0) r = 0; if (r > 255) r = 255;
                        if (g2 < 0) g2 = 0; if (g2 > 255) g2 = 255;
                        if (b < 0) b = 0; if (b > 255) b = 255;
                        argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g2 << 8) | (uint32_t)b;
                    }
                    output[oy * d->width + ox] = argb;
                }
            }
        }
    }

    for (int c = 0; c < d->ncomp; c++) kfree(cbuf[c]);
    return JPEG_OK;
}

// ============================================================================
// Public API
// ============================================================================

int jpeg_decode_init(const uint8_t* data, int length) {
    if (!data || length < 4) return JPEG_ERR_BAD_DATA;
    _jpg_dec* d = &_g_jpg;
    _jpg_memset(d, 0, sizeof(_jpg_dec));
    d->data = data;
    d->len = length;
    d->pos = 0;
    d->restart_int = 0;
    return _parse_markers(d);
}

int jpeg_decode(uint32_t* output, int* out_width, int* out_height) {
    _jpg_dec* d = &_g_jpg;
    if (!d->data || d->width <= 0 || d->height <= 0) return JPEG_ERR_BAD_DATA;
    *out_width = d->width;
    *out_height = d->height;
    _jpg_memset(output, 0, d->width * d->height * 4);
    return _decode_scan(d, output);
}

uint32_t* jpeg_load_file(const char* path, int* out_width, int* out_height) {
    if (!path || !out_width || !out_height) return NULL;
    *out_width = 0;
    *out_height = 0;

    int bufsz = 1024 * 1024;
    uint8_t* buf = (uint8_t*)kmalloc(bufsz);
    if (!buf) return NULL;

    int len = sys_fs_read(path, (char*)buf, bufsz);
    if (len <= 0 || len < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        kfree(buf);
        return NULL;
    }

    if (jpeg_decode_init(buf, len) != JPEG_OK) {
        kfree(buf);
        return NULL;
    }

    _jpg_dec* d = &_g_jpg;
    int w = d->width, h = d->height;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        kfree(buf);
        return NULL;
    }

    uint32_t* px = (uint32_t*)kmalloc(w * h * 4);
    if (!px) { kfree(buf); return NULL; }

    int ow, oh;
    if (jpeg_decode(px, &ow, &oh) != JPEG_OK) {
        kfree(px);
        kfree(buf);
        return NULL;
    }

    *out_width = ow;
    *out_height = oh;
    kfree(buf);
    return px;
}

#endif // JPEG_DECODER_IMPLEMENTATION

#endif // JPEG_DECODER_H
