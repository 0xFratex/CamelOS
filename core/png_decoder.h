// core/png_decoder.h - PNG image decoder for CamelOS
// Supports 8-bit RGB and RGBA PNG images with standard row filters.

#ifndef PNG_DECODER_H
#define PNG_DECODER_H

#include "../include/types.h"

// Supported PNG color types
#define PNG_COLOR_TYPE_RGB   2
#define PNG_COLOR_TYPE_RGBA  6

// Decoded PNG image structure
// pixel_data is stored in ARGB format: 0xAARRGGBB
typedef struct {
    uint32_t  width;
    uint32_t  height;
    uint8_t   color_type;
    uint8_t   bit_depth;
    uint32_t* pixel_data;  // ARGB format: 0xAARRGGBB
} png_image_t;

// Decode a PNG image from raw data
// data:     pointer to raw PNG file data
// data_len: length of the PNG data in bytes
// out_image: pointer to a png_image_t to populate
// Returns: 0 on success, -1 on error
int png_decode(const uint8_t* data, uint32_t data_len, png_image_t* out_image);

// Free the pixel data allocated by png_decode
// image: pointer to a png_image_t whose pixel_data should be freed
void png_image_free(png_image_t* image);

#endif
