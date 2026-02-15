#include "../sys/api.h"

// Unix Timestamp conversion (Simple)
#define SECS_PER_MIN  60
#define SECS_PER_HOUR 3600
#define SECS_PER_DAY  86400

static int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

int is_leap(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

unsigned int get_unix_time() {
    int h, m, s;
    // Basic RTC read from kernel
    sys_get_time(&h, &m, &s);
    
    // Mock Date (Since basic RTC driver in prompt doesn't return Y/M/D)
    // In a real implementation, you'd read ports 0x09, 0x08, 0x07 from CMOS
    int year = 2025;
    int month = 1;
    int day = 1;
    
    unsigned int days = 0;
    for (int y = 1970; y < year; y++) {
        days += 365 + is_leap(y);
    }
    for (int mo = 0; mo < month - 1; mo++) {
        if (mo == 1 && is_leap(year)) days += 29;
        else days += days_in_month[mo];
    }
    days += day - 1;
    
    return (days * SECS_PER_DAY) + (h * SECS_PER_HOUR) + (m * SECS_PER_MIN) + s;
}