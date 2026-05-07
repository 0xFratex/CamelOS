// core/zlib_inflate.h - Minimal zlib/deflate decompression for CamelOS
// Supports the zlib format used in macOS DMG files:
//   2-byte header (CMF + FLG) + deflate data + 4-byte Adler-32 checksum

#ifndef ZLIB_INFLATE_H
#define ZLIB_INFLATE_H

#include "../include/types.h"

// Decompress zlib-format data (as used in DMG files)
// src: pointer to compressed data
// src_len: length of compressed data
// dst: pointer to output buffer
// dst_cap: capacity of output buffer
// dst_len: [out] actual decompressed size
// Returns: 0 on success, -1 on error
int zlib_inflate(const uint8_t* src, uint32_t src_len,
                 uint8_t* dst, uint32_t dst_cap,
                 uint32_t* dst_len);

// Decompress gzip-format data (RFC 1952, as used in HTTP Content-Encoding)
// Strips the gzip header and trailer, then runs raw deflate decompression.
// src: pointer to compressed data
// src_len: length of compressed data
// dst: pointer to output buffer
// dst_cap: capacity of output buffer
// dst_len: [out] actual decompressed size
// Returns: 0 on success, -1 on error
int gzip_inflate(const uint8_t* src, uint32_t src_len,
                 uint8_t* dst, uint32_t dst_cap,
                 uint32_t* dst_len);

// Decompress raw deflate data (no header/trailer)
// src: pointer to compressed data
// src_len: length of compressed data
// dst: pointer to output buffer
// dst_cap: capacity of output buffer
// dst_len: [out] actual decompressed size
// Returns: 0 on success, -1 on error
int raw_deflate_inflate(const uint8_t* src, uint32_t src_len,
                        uint8_t* dst, uint32_t dst_cap,
                        uint32_t* dst_len);

// Compute Adler-32 checksum (for zlib verification)
uint32_t adler32(const uint8_t* data, uint32_t len);

#endif
