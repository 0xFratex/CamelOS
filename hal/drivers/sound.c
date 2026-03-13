// hal/drivers/sound.c
#include "../common/ports.h"
#include "../sys/api.h"

static void nosound() {
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

static void play_sound_raw(uint32_t nFrequence) {
    uint32_t Div;
    uint8_t tmp;
    Div = 1193180 / nFrequence;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t) (Div) );
    outb(0x42, (uint8_t) (Div >> 8));
    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

void beep(int duration_ms) {
    if (duration_ms <= 0) return;
    play_sound_raw(1000);
    sys_delay(duration_ms);
    nosound();
}

void play_startup_chime() {
    // Two short, distinct beeps indicating system ready
    // Much cleaner than raw PCM static
    play_sound_raw(880); // A5
    sys_delay(80);
    nosound();
    sys_delay(50);
    play_sound_raw(1100); // C#6
    sys_delay(120);
    nosound();
}