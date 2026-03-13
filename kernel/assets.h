#ifndef ASSETS_H
#define ASSETS_H

// Use basic types instead of stdint.h for bare-metal kernel
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

typedef struct {
    const char* name;
    const uint32_t* data;
    uint32_t width;
    uint32_t height;
} embedded_image_t;

// The function provided by the generated .c file
const embedded_image_t* get_embedded_images(uint32_t* count);

#endif
