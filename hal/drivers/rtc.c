#include "../../common/ports.h"
#include "../../sys/api.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

int rtc_get_update_in_progress_flag() {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

unsigned char rtc_get_register(int reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

// BCD to Binary conversion
unsigned char bcd2bin(unsigned char bcd) {
    return ((bcd & 0xF0) >> 1) + ( (bcd & 0xF0) >> 3) + (bcd & 0x0f);
}

void rtc_read_time(int* h, int* m, int* s) {
    unsigned char second, minute, hour, registerB;

    // Wait until update is not in progress
    while (rtc_get_update_in_progress_flag());

    second = rtc_get_register(0x00);
    minute = rtc_get_register(0x02);
    hour   = rtc_get_register(0x04);

    registerB = rtc_get_register(0x0B);

    // Convert BCD to binary values if necessary
    if (!(registerB & 0x04)) {
        second = bcd2bin(second);
        minute = bcd2bin(minute);
        hour   = bcd2bin(hour);
    }

    // Convert 12 hour clock to 24 hour clock if necessary
    if (!(registerB & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    *s = (int)second;
    *m = (int)minute;
    *h = (int)hour;
}