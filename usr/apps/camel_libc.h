#ifndef CAMEL_LIBC_H
#define CAMEL_LIBC_H

#include "../../sys/cdl_defs.h"
extern kernel_api_t* sys;

#define NULL 0
typedef unsigned long size_t;
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef int int32_t;
typedef short int16_t;
typedef char int8_t;

#define true 1
#define false 0
typedef int bool;

#define assert(x)

static inline void *malloc(size_t size) { return sys->malloc(size); }
static inline void free(void *ptr) { sys->free(ptr); }
static inline void *memcpy(void *dest, const void *src, size_t n) { sys->memcpy(dest, src, n); return dest; }
static inline void *memset(void *s, int c, size_t n) { sys->memset(s, c, n); return s; }
static inline void *memmove(void *dest, const void *src, size_t n) { sys->memmove(dest, src, n); return dest; }
static inline int memcmp(const void *s1, const void *s2, size_t n) { return sys->strncmp((const char*)s1, (const char*)s2, n); }
static inline size_t strlen(const char *s) { return sys->strlen(s); }

static inline double strtod(const char *nptr, char **endptr) {
    double val = 0.0;
    while (*nptr >= '0' && *nptr <= '9') {
        val = val * 10.0 + (*nptr - '0');
        nptr++;
    }
    if (*nptr == '.') {
        nptr++;
        double frac = 1.0;
        while (*nptr >= '0' && *nptr <= '9') {
            frac /= 10.0;
            val += (*nptr - '0') * frac;
            nptr++;
        }
    }
    if (endptr) *endptr = (char*)nptr;
    return val;
}

static inline double modf(double x, double *iptr) {
    long i = (long)x;
    if (iptr) *iptr = (double)i;
    return x - i;
}

double __floatunsidf(unsigned int i);
double __adddf3(double a, double b);
double __subdf3(double a, double b);
double __muldf3(double a, double b);
double __divdf3(double a, double b);

int __ltdf2(double a, double b);
int __gedf2(double a, double b);
int __gtdf2(double a, double b);
int __ledf2(double a, double b);
int __eqdf2(double a, double b);
int __nedf2(double a, double b);
int __unorddf2(double a, double b);

int __fixdfsi(double a);
unsigned int __fixunsdfsi(double a);
double __floatsidf(int a);

#endif
