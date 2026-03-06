// utils/net.h - CORRECT byte-swapping implementations
#ifndef NET_H
#define NET_H

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;

// CORRECT byte-swapping implementations
static inline uint32_t swap32(uint32_t val) {
    return ((val & 0xFF) << 24) |
           ((val & 0xFF00) << 8) |
           ((val & 0xFF0000) >> 8) |
           ((val & 0xFF000000) >> 24);
}

static inline uint16_t swap16(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val & 0xFF00) >> 8);
}

// Network to Host Long (Convert Big Endian to Little Endian)
static inline uint32_t ntohl(uint32_t netlong) {
    // Since x86 is Little Endian, we must swap Network (Big Endian) data
    return swap32(netlong);
}

// Host to Network Long (Convert Little Endian to Big Endian)
static inline uint32_t htonl(uint32_t hostlong) {
    return swap32(hostlong);
}

// Network to Host Short (Convert Big Endian to Little Endian)
static inline uint16_t ntohs(uint16_t netshort) {
    return swap16(netshort);
}

// Host to Network Short (Convert Little Endian to Big Endian)
static inline uint16_t htons(uint16_t hostshort) {
    return swap16(hostshort);
}

#endif