// common/ports.h
#ifndef PORTS_H
#define PORTS_H

#include "../include/types.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "d"(port));
    return result;
}

static inline void outb(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "d"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    asm volatile("inw %1, %0" : "=a"(result) : "d"(port));
    return result;
}

static inline void outw(uint16_t port, uint16_t data) {
    asm volatile("outw %0, %1" : : "a"(data), "d"(port));
}

static inline uint32_t inl(uint32_t port) {
    uint32_t result;
    asm volatile("inl %1, %0" : "=a"(result) : "d"(port));
    return result;
}

static inline void outl(uint32_t port, uint32_t data) {
    asm volatile("outl %0, %1" : : "a"(data), "d"(port));
}

#endif
