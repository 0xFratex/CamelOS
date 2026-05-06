#ifndef RTC_H
#define RTC_H

// RTC time and date reading
void rtc_read_time(int* h, int* m, int* s);
void rtc_read_date(int* year, int* month, int* day);

// Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
// Reads both date and time from the RTC hardware
unsigned int get_unix_time(void);

// Leap year helper
int is_leap(int year);

#endif
