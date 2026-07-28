#include "ports.h"

#define PORT 0x3f8 // COM1

// Variadic support for s_printf
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)
typedef __builtin_va_list va_list;

// Forward declaration of vsnprintf from core/string.c (bounds-checked)
extern int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);

int init_serial() {
   outb(PORT + 1, 0x00);    // Disable all interrupts
   outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
   outb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
   outb(PORT + 1, 0x00);    //                  (hi byte)
   outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
   outb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
   outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
   return 0;
}

int is_transmit_empty() {
   return inb(PORT + 5) & 0x20;
}

void write_serial(char a) {
   while (is_transmit_empty() == 0);
   outb(PORT, a);
}

void s_printf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    for (int i = 0; buf[i] != 0; i++) {
        write_serial(buf[i]);
    }
}


void serial_write_string(const char* str) {
    // Pass str as DATA, not as a format string — prevents %s/%d etc.
    // in `str` from being interpreted and reading random stack values.
    s_printf("%s", str);
}
