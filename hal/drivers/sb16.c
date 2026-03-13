#include "../../common/ports.h"
#include "../../core/memory.h"
#include "../../hal/drivers/serial.h"

#define DSP_RESET 0x226
#define DSP_READ  0x22A
#define DSP_WRITE 0x22C
#define DSP_DATA_AVAIL 0x22E

// Fixed: Shorter timeout to prevent kernel freeze
int sb16_wait_write() {
    int timeout = 1000;
    while ((inb(DSP_WRITE) & 0x80) != 0) {
        if (--timeout == 0) return 0;
    }
    return 1;
}

void sb16_write(uint8_t val) {
    if(sb16_wait_write()) outb(DSP_WRITE, val);
}

int sb16_init() {
    outb(DSP_RESET, 1);
    for(int i=0; i<100; i++) asm volatile("nop"); // Short delay
    outb(DSP_RESET, 0);

    int timeout = 2000;
    while (!(inb(DSP_DATA_AVAIL) & 0x80)) {
        if (--timeout == 0) return 0;
    }

    if (inb(DSP_READ) == 0xAA) {
        s_printf("[AUDIO] SB16 Found.\n");
        return 1;
    }
    return 0;
}

void sb16_play_direct(uint8_t* data, uint32_t len) {
    if(!sb16_init()) return;

    // Turn speaker on
    sb16_write(0xD1);

    // Clamp size for safety
    if (len > 0xFFFF) len = 0xFFFF;

    for(uint32_t i=0; i<len; i++) {
        sb16_write(0x10); // Direct 8-bit DAC
        sb16_write(data[i]);
        
        // Audio Timing Delay
        // 22kHz = ~45 microseconds per sample
        // On fast QEMU, we need a decent spin.
        // On bare metal, we should use the PIT, but that blocks interrupts.
        // This spin is tuned for emulation speed.
        for(volatile int k=0; k<1500; k++);
    }

    sb16_write(0xD3); // Speaker off
}