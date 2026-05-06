#include "../../common/ports.h"
#include "../../sys/api.h"
#include "rtc.h"

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

void rtc_read_date(int* year, int* month, int* day) {
    unsigned char century, registerB;

    // Wait until update is not in progress
    while (rtc_get_update_in_progress_flag());

    *day   = rtc_get_register(0x07);
    *month = rtc_get_register(0x08);
    *year  = rtc_get_register(0x09);
    century = rtc_get_register(0x32);

    registerB = rtc_get_register(0x0B);

    // Convert BCD to binary values if necessary
    if (!(registerB & 0x04)) {
        *day   = bcd2bin(*day);
        *month = bcd2bin(*month);
        *year  = bcd2bin(*year);
        century = bcd2bin(century);
    }

    // Compute full year
    if (century > 0) {
        *year = century * 100 + *year;
    } else {
        *year = 2000 + *year;
    }
}

// ============================================================================
// Unix Timestamp Conversion
// ============================================================================

#define SECS_PER_MIN  60
#define SECS_PER_HOUR 3600
#define SECS_PER_DAY  86400

static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

int is_leap(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

unsigned int get_unix_time(void) {
    int h, m, s;
    int year, month, day;

    // Read actual date and time from the RTC hardware
    rtc_read_date(&year, &month, &day);
    rtc_read_time(&h, &m, &s);

    // Validate: if date is unreasonable, fall back to a safe default
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
        year = 2025; month = 1; day = 1;
    }

    // Count days from 1970-01-01 to the beginning of the given year
    unsigned int days = 0;
    for (int y = 1970; y < year; y++) {
        days += 365 + is_leap(y);
    }

    // Add days for completed months in the current year
    for (int mo = 0; mo < month - 1; mo++) {
        if (mo == 1 && is_leap(year))
            days += 29;
        else
            days += days_in_month[mo];
    }

    // Add days in the current month (day-1 because day 1 = 0 completed days)
    days += day - 1;

    return (days * SECS_PER_DAY) + (h * SECS_PER_HOUR) + (m * SECS_PER_MIN) + s;
}