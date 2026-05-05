// core/png_decoder.c - PNG image decoder for CamelOS
// Implements decoding of 8-bit RGB/RGBA PNG images per the PNG specification.
// Uses zlib_inflate for decompression and supports all five standard row filters.

#include "png_decoder.h"
#include "zlib_inflate.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"

// =========================================================================
// Big-endian helpers
// =========================================================================

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// =========================================================================
// PNG signature
// =========================================================================

static const uint8_t png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

// =========================================================================
// Paeth predictor (PNG spec, section 9.4)
// =========================================================================

static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int32_t p = (int32_t)a + (int32_t)b - (int32_t)c;
    int32_t pa = p > (int32_t)a ? p - (int32_t)a : (int32_t)a - p;
    int32_t pb = p > (int32_t)b ? p - (int32_t)b : (int32_t)b - p;
    int32_t pc = p > (int32_t)c ? p - (int32_t)c : (int32_t)c - p;

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// =========================================================================
// PNG row filter application
// =========================================================================
// Each scanline starts with a filter byte (0-4) followed by the pixel bytes.
// We apply the inverse filter in-place on the decompressed data.
//
// row_bytes: number of bytes per row (excluding the filter byte)
// bpp: bytes per pixel (3 for RGB, 4 for RGBA)
// raw: decompressed data, organized as [filter_byte][row_bytes] per row

static int apply_filters(uint8_t* raw, uint32_t width, uint32_t height,
                         uint8_t bpp) {
    uint32_t row_bytes = width * bpp;
    uint32_t stride = 1 + row_bytes;  // filter byte + pixel bytes
    uint32_t y, x;

    uint8_t* prev_row = (uint8_t*)0;  // no previous row for the first row

    for (y = 0; y < height; y++) {
        uint8_t* row_start = raw + y * stride;
        uint8_t filter_type = row_start[0];
        uint8_t* pixel = row_start + 1;  // pixel data starts after filter byte

        switch (filter_type) {
        case 0:  // None
            // No filtering applied
            break;

        case 1:  // Sub: Pixel[x] += Pixel[x - bpp]
            for (x = bpp; x < row_bytes; x++) {
                pixel[x] = (uint8_t)(pixel[x] + pixel[x - bpp]);
            }
            break;

        case 2:  // Up: Pixel[x] += Prior[x]
            if (prev_row) {
                uint8_t* prev_pixel = prev_row + 1;
                for (x = 0; x < row_bytes; x++) {
                    pixel[x] = (uint8_t)(pixel[x] + prev_pixel[x]);
                }
            }
            break;

        case 3:  // Average: Pixel[x] += (Pixel[x - bpp] + Prior[x]) / 2
            for (x = 0; x < row_bytes; x++) {
                uint8_t left  = (x >= bpp) ? pixel[x - bpp] : 0;
                uint8_t above = (prev_row) ? (prev_row + 1)[x] : 0;
                pixel[x] = (uint8_t)(pixel[x] + ((uint16_t)left + above) / 2);
            }
            break;

        case 4:  // Paeth: Pixel[x] += PaethPredictor(left, above, above_left)
            for (x = 0; x < row_bytes; x++) {
                uint8_t left       = (x >= bpp) ? pixel[x - bpp] : 0;
                uint8_t above      = (prev_row) ? (prev_row + 1)[x] : 0;
                uint8_t above_left = (prev_row && x >= bpp) ? (prev_row + 1)[x - bpp] : 0;
                pixel[x] = (uint8_t)(pixel[x] + paeth_predictor(left, above, above_left));
            }
            break;

        default:
            s_printf("PNG: unknown row filter type %d\n", filter_type);
            return -1;
        }

        prev_row = row_start;
    }

    return 0;
}

// =========================================================================
// Main PNG decode entry point
// =========================================================================

int png_decode(const uint8_t* data, uint32_t data_len, png_image_t* out_image) {
    uint32_t offset;
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0;
    uint8_t compression, filter_method, interlace;

    // Accumulated IDAT data
    uint8_t* idat_buf = (uint8_t*)0;
    uint32_t idat_len = 0;
    uint32_t idat_cap = 0;

    // Decompressed image data
    uint8_t* raw = (uint8_t*)0;
    uint32_t raw_len = 0;

    // Output pixel data
    uint32_t* pixels = (uint32_t*)0;

    if (!data || data_len < 8 || !out_image) {
        return -1;
    }

    // -----------------------------------------------------------------
    // 1. Verify PNG signature (first 8 bytes)
    // -----------------------------------------------------------------
    for (int i = 0; i < 8; i++) {
        if (data[i] != png_signature[i]) {
            s_printf("PNG: invalid signature\n");
            return -1;
        }
    }

    // -----------------------------------------------------------------
    // 2. Parse chunks
    // -----------------------------------------------------------------
    offset = 8;  // skip signature

    while (offset + 12 <= data_len) {
        // Each chunk: 4-byte length (BE), 4-byte type, data[length], 4-byte CRC
        uint32_t chunk_len = be32(data + offset);
        const uint8_t* chunk_type = data + offset + 4;

        // Validate we can read the full chunk
        if (offset + 12 + chunk_len > data_len) {
            s_printf("PNG: chunk extends beyond data\n");
            goto error;
        }

        // Check for IHDR chunk
        if (chunk_type[0] == 'I' && chunk_type[1] == 'H' &&
            chunk_type[2] == 'D' && chunk_type[3] == 'R') {
            // IHDR is always 13 bytes
            if (chunk_len != 13) {
                s_printf("PNG: invalid IHDR length %d\n", chunk_len);
                goto error;
            }

            const uint8_t* ihdr = data + offset + 8;
            width       = be32(ihdr + 0);
            height      = be32(ihdr + 4);
            bit_depth   = ihdr[8];
            color_type  = ihdr[9];
            compression = ihdr[10];
            filter_method = ihdr[11];
            interlace   = ihdr[12];

            // Validate dimensions
            if (width == 0 || height == 0) {
                s_printf("PNG: invalid dimensions %dx%d\n", width, height);
                goto error;
            }

            // Only support 8-bit depth, RGB or RGBA
            if (bit_depth != 8) {
                s_printf("PNG: unsupported bit depth %d (only 8 supported)\n", bit_depth);
                goto error;
            }
            if (color_type != PNG_COLOR_TYPE_RGB &&
                color_type != PNG_COLOR_TYPE_RGBA) {
                s_printf("PNG: unsupported color type %d (only RGB=2, RGBA=6)\n", color_type);
                goto error;
            }

            // Only support standard compression and filtering
            if (compression != 0 || filter_method != 0) {
                s_printf("PNG: unsupported compression %d or filter %d\n",
                         compression, filter_method);
                goto error;
            }

            // No interlace support
            if (interlace != 0) {
                s_printf("PNG: interlacing not supported\n");
                goto error;
            }
        }

        // Check for IDAT chunk - accumulate compressed data
        else if (chunk_type[0] == 'I' && chunk_type[1] == 'D' &&
                 chunk_type[2] == 'A' && chunk_type[3] == 'T') {
            // Grow the IDAT buffer if needed
            if (idat_len + chunk_len > idat_cap) {
                uint32_t new_cap = idat_cap == 0 ? chunk_len : idat_cap * 2;
                if (new_cap < idat_len + chunk_len) {
                    new_cap = idat_len + chunk_len;
                }

                uint8_t* new_buf = (uint8_t*)krealloc(idat_buf, new_cap);
                if (!new_buf) {
                    s_printf("PNG: out of memory for IDAT buffer\n");
                    goto error;
                }
                idat_buf = new_buf;
                idat_cap = new_cap;
            }

            memcpy(idat_buf + idat_len, data + offset + 8, chunk_len);
            idat_len += chunk_len;
        }

        // Check for IEND chunk - stop parsing
        else if (chunk_type[0] == 'I' && chunk_type[1] == 'E' &&
                 chunk_type[2] == 'N' && chunk_type[3] == 'D') {
            break;
        }

        // Skip CRC (4 bytes) and advance to next chunk
        offset += 12 + chunk_len;
    }

    // Verify we got an IHDR
    if (width == 0 || height == 0) {
        s_printf("PNG: no valid IHDR chunk found\n");
        goto error;
    }

    // Verify we got IDAT data
    if (idat_len == 0) {
        s_printf("PNG: no IDAT data found\n");
        goto error;
    }

    // -----------------------------------------------------------------
    // 3. Decompress IDAT data using zlib_inflate
    // -----------------------------------------------------------------
    uint8_t bpp = (color_type == PNG_COLOR_TYPE_RGBA) ? 4 : 3;
    uint32_t row_bytes = width * bpp;
    uint32_t stride = 1 + row_bytes;  // filter byte + pixel bytes per row
    raw_len = stride * height;

    raw = (uint8_t*)kmalloc(raw_len);
    if (!raw) {
        s_printf("PNG: out of memory for decompressed data\n");
        goto error;
    }

    uint32_t decompressed_len = 0;
    int inflate_result = zlib_inflate(idat_buf, idat_len, raw, raw_len,
                                      &decompressed_len);
    if (inflate_result < 0 || decompressed_len != raw_len) {
        s_printf("PNG: decompression failed (result=%d, got %d, expected %d)\n",
                 inflate_result, decompressed_len, raw_len);
        goto error;
    }

    // Free IDAT buffer - no longer needed
    kfree(idat_buf);
    idat_buf = (uint8_t*)0;

    // -----------------------------------------------------------------
    // 4. Apply PNG row filters
    // -----------------------------------------------------------------
    if (apply_filters(raw, width, height, bpp) < 0) {
        goto error;
    }

    // -----------------------------------------------------------------
    // 5. Convert filtered bytes to ARGB pixel data (0xAARRGGBB)
    // -----------------------------------------------------------------
    pixels = (uint32_t*)kmalloc(width * height * sizeof(uint32_t));
    if (!pixels) {
        s_printf("PNG: out of memory for pixel data\n");
        goto error;
    }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row_start = raw + y * stride;
        uint8_t* pixel = row_start + 1;  // skip filter byte

        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b, a;

            if (color_type == PNG_COLOR_TYPE_RGB) {
                r = pixel[x * 3 + 0];
                g = pixel[x * 3 + 1];
                b = pixel[x * 3 + 2];
                a = 0xFF;  // fully opaque for RGB
            } else {
                // RGBA
                r = pixel[x * 4 + 0];
                g = pixel[x * 4 + 1];
                b = pixel[x * 4 + 2];
                a = pixel[x * 4 + 3];
            }

            pixels[y * width + x] = ((uint32_t)a << 24) |
                                     ((uint32_t)r << 16) |
                                     ((uint32_t)g << 8)  |
                                      (uint32_t)b;
        }
    }

    // Free the raw decompressed data
    kfree(raw);
    raw = (uint8_t*)0;

    // -----------------------------------------------------------------
    // 6. Populate output structure
    // -----------------------------------------------------------------
    out_image->width      = width;
    out_image->height     = height;
    out_image->color_type = color_type;
    out_image->bit_depth  = bit_depth;
    out_image->pixel_data = pixels;

    return 0;

error:
    // Clean up all allocated buffers
    if (idat_buf) kfree(idat_buf);
    if (raw)      kfree(raw);
    if (pixels)   kfree(pixels);
    return -1;
}

// =========================================================================
// Free PNG image pixel data
// =========================================================================

void png_image_free(png_image_t* image) {
    if (image && image->pixel_data) {
        kfree(image->pixel_data);
        image->pixel_data = (uint32_t*)0;
    }
    if (image) {
        image->width  = 0;
        image->height = 0;
    }
}
