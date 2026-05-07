#ifndef _TIME_H
#define _TIME_H

#include "types.h"

// Minimal time.h for MuJS compatibility

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCKS_PER_SEC 50

time_t time(time_t* t);
clock_t clock(void);
struct tm* localtime(const time_t* timer);
struct tm* gmtime(const time_t* timer);
time_t mktime(struct tm* t);
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* t);
double difftime(time_t t1, time_t t0);

#endif
