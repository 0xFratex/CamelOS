// lib/libc_compat.c - Minimal C library compatibility for third-party code (MuJS etc.)
// Provides stubs and mappings from standard C functions to CamelOS kernel APIs
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/math.h"
#include "../include/time.h"
#include "../include/errno.h"
#include "../include/sys/time.h"
#include "../sys/api.h"

extern void panic(const char* msg);

int errno = 0;

// strerror stub
char* strerror(int errnum) {
    static char buf[32];
    if (errnum == EDOM) return "Domain error";
    if (errnum == ERANGE) return "Range error";
    if (errnum == EINVAL) return "Invalid argument";
    if (errnum == ENOENT) return "No such file or directory";
    if (errnum == ENOMEM) return "Out of memory";
    buf[0] = 'E'; buf[1] = 'r'; buf[2] = 'r'; buf[3] = 'n'; buf[4] = 'o'; buf[5] = ' '; buf[6] = 0;
    int n = errnum;
    char tmp[12]; int i = 0;
    if (n < 0) n = -n;
    if (n == 0) tmp[i++] = '0';
    else while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int pos = 6;
    while (i > 0 && pos < 31) buf[pos++] = tmp[--i];
    buf[pos] = 0;
    return buf;
}

// ---- stdio stubs ----

// printf family - maps to CamelOS printk/serial
int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_print(buf);
    return n;
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_print(buf);
    return n;
}

int vprintf(const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    sys_print(buf);
    return n;
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    sys_print(buf);
    return n;
}

int putchar(int c) {
    char buf[2] = { (char)c, 0 };
    sys_print(buf);
    return c;
}

int puts(const char* s) {
    sys_print(s);
    sys_print("\n");
    return 0;
}

int fputc(int c, FILE* f) {
    return putchar(c);
}

int fputs(const char* s, FILE* f) {
    sys_print(s);
    return 0;
}

// File stubs - MuJS uses these for js_dofile which we don't support
FILE* fopen(const char* path, const char* mode) { return NULL; }
int fclose(FILE* f) { return EOF; }
size_t fread(void* ptr, size_t size, size_t count, FILE* f) { return 0; }
size_t fwrite(const void* ptr, size_t size, size_t count, FILE* f) { return 0; }
int fseek(FILE* f, long offset, int whence) { return -1; }
long ftell(FILE* f) { return -1; }
void rewind(FILE* f) { }
int feof(FILE* f) { return 1; }
int fflush(FILE* f) { return 0; }

int sscanf(const char* str, const char* fmt, ...) {
    // Minimal sscanf - just return 0 (no items matched)
    return 0;
}

// ---- stdlib stubs ----

void* malloc(size_t size) {
    return kmalloc(size);
}

void* calloc(size_t num, size_t size) {
    void* p = kmalloc(num * size);
    if (p) memset(p, 0, num * size);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    // Simple realloc: allocate new, copy, free old
    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        // We don't know old size, so copy 'size' bytes (may overread, but safer than nothing)
        memcpy(new_ptr, ptr, size);
        kfree(ptr);
    }
    return new_ptr;
}

void free(void* ptr) {
    kfree(ptr);
}

// atoi is provided by core/string.c, don't redefine here
#if 0
int atoi(const char* str) {
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return sign * result;
}
#endif

long atol(const char* str) {
    return (long)atoi(str);
}

long strtol(const char* str, char** endptr, int base) {
    long result = 0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    if (base == 0) {
        if (*str == '0' && (*(str+1) == 'x' || *(str+1) == 'X')) { base = 16; str += 2; }
        else if (*str == '0') base = 8;
        else base = 10;
    }
    while (1) {
        int digit;
        if (*str >= '0' && *str <= '9') digit = *str - '0';
        else if (*str >= 'a' && *str <= 'f') digit = *str - 'a' + 10;
        else if (*str >= 'A' && *str <= 'F') digit = *str - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        str++;
    }
    if (endptr) *endptr = (char*)str;
    return sign * result;
}

unsigned long strtoul(const char* str, char** endptr, int base) {
    return (unsigned long)strtol(str, endptr, base);
}

double strtod(const char* str, char** endptr) {
    double result = 0.0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10.0 + (*str - '0');
        str++;
    }
    if (*str == '.') {
        str++;
        double frac = 0.1;
        while (*str >= '0' && *str <= '9') {
            result += (*str - '0') * frac;
            frac *= 0.1;
            str++;
        }
    }
    if (*str == 'e' || *str == 'E') {
        str++;
        int exp_sign = 1;
        int exp = 0;
        if (*str == '-') { exp_sign = -1; str++; }
        else if (*str == '+') str++;
        while (*str >= '0' && *str <= '9') {
            exp = exp * 10 + (*str - '0');
            str++;
        }
        double exp_mul = 1.0;
        for (int i = 0; i < exp; i++) exp_mul *= 10.0;
        if (exp_sign < 0) result /= exp_mul;
        else result *= exp_mul;
    }
    if (endptr) *endptr = (char*)str;
    return sign * result;
}

double atof(const char* str) {
    return strtod(str, NULL);
}

static unsigned long next_rand = 1;

int rand(void) {
    next_rand = next_rand * 1103515245 + 12345;
    return (unsigned int)(next_rand / 65536) % (RAND_MAX + 1);
}

void srand(unsigned int seed) {
    next_rand = seed;
}

void abort(void) {
    panic("abort() called");
    while(1);
}

void exit(int status) {
    panic("exit() called");
    while(1);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

div_t div(int num, int denom) {
    div_t d;
    d.quot = num / denom;
    d.rem = num % denom;
    return d;
}

// ---- math stubs ----
// Use GCC builtins where possible

double fabs(double x) {
    return __builtin_fabs(x);
}

double floor(double x) {
    return __builtin_floor(x);
}

double ceil(double x) {
    return __builtin_ceil(x);
}

double fmod(double x, double y) {
    return __builtin_fmod(x, y);
}

double sqrt(double x) {
    return __builtin_sqrt(x);
}

double pow(double x, double y) {
    return __builtin_pow(x, y);
}

double log(double x) {
    return __builtin_log(x);
}

double log10(double x) {
    return __builtin_log10(x);
}

double log2(double x) {
    return __builtin_log2(x);
}

double exp(double x) {
    return __builtin_exp(x);
}

double sin(double x) {
    return __builtin_sin(x);
}

double cos(double x) {
    return __builtin_cos(x);
}

double tan(double x) {
    return __builtin_tan(x);
}

double asin(double x) {
    return __builtin_asin(x);
}

double acos(double x) {
    return __builtin_acos(x);
}

double atan(double x) {
    return __builtin_atan(x);
}

double atan2(double y, double x) {
    return __builtin_atan2(y, x);
}

double round(double x) {
    return __builtin_round(x);
}

double trunc(double x) {
    return __builtin_trunc(x);
}

double copysign(double x, double y) {
    return __builtin_copysign(x, y);
}

int isnan(double x) {
    return __builtin_isnan(x);
}

int isinf(double x) {
    return __builtin_isinf(x);
}

int isfinite(double x) {
    return __builtin_isfinite(x);
}

int signbit(double x) {
    return __builtin_signbit(x);
}

double frexp(double x, int* exp) {
    return __builtin_frexp(x, exp);
}

double ldexp(double x, int exp) {
    return __builtin_ldexp(x, exp);
}

double modf(double x, double* iptr) {
    return __builtin_modf(x, iptr);
}

// ---- time stubs ----

time_t time(time_t* t) {
    int h, m, s;
    sys_get_time(&h, &m, &s);
    time_t val = h * 3600 + m * 60 + s;
    if (t) *t = val;
    return val;
}

clock_t clock(void) {
    static clock_t ticks = 0;
    return ticks++;
}

static struct tm tm_buf;
struct tm* localtime(const time_t* timer) {
    memset(&tm_buf, 0, sizeof(tm_buf));
    if (timer) {
        time_t t = *timer;
        tm_buf.tm_hour = (int)(t / 3600) % 24;
        tm_buf.tm_min = (int)(t / 60) % 60;
        tm_buf.tm_sec = (int)(t) % 60;
    }
    return &tm_buf;
}

struct tm* gmtime(const time_t* timer) {
    return localtime(timer);
}

time_t mktime(struct tm* t) {
    if (!t) return 0;
    return t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

size_t strftime(char* s, size_t max, const char* fmt, const struct tm* t) {
    if (!s || !t || max < 2) return 0;
    int h = t->tm_hour, m = t->tm_min, sec = t->tm_sec;
    char buf[32];
    buf[0] = '0' + h/10; buf[1] = '0' + h%10; buf[2] = ':';
    buf[3] = '0' + m/10; buf[4] = '0' + m%10; buf[5] = ':';
    buf[6] = '0' + sec/10; buf[7] = '0' + sec%10; buf[8] = 0;
    size_t len = strlen(buf);
    if (len >= max) len = max - 1;
    memcpy(s, buf, len);
    s[len] = 0;
    return len;
}

double difftime(time_t t1, time_t t0) {
    return (double)(t1 - t0);
}

// gettimeofday - needed by MuJS jsdate.c
int gettimeofday(struct timeval* tv, struct timezone* tz) {
    if (tv) {
        time_t t = time(NULL);
        tv->tv_sec = t;
        tv->tv_usec = 0;
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

// ---- 64-bit arithmetic helpers ----
// These are needed because the kernel is compiled for i386 without SSE/MMX
// and the compiler generates calls to these soft-division functions

typedef unsigned long long u64_local;
typedef long long i64_local;

// __udivmoddi4 - unsigned 64-bit division with remainder
u64_local __udivmoddi4(u64_local num, u64_local den, u64_local *rem) {
    u64_local quot = 0;
    u64_local bit = 1;

    if (den == 0) {
        if (rem) *rem = 0;
        return 0;
    }

    // Align den with num
    while (den <= num && !(den & ((u64_local)1 << 63))) {
        den <<= 1;
        bit <<= 1;
    }

    while (bit) {
        if (num >= den) {
            num -= den;
            quot |= bit;
        }
        den >>= 1;
        bit >>= 1;
    }

    if (rem) *rem = num;
    return quot;
}

// __udivdi3 - unsigned 64-bit division
u64_local __udivdi3(u64_local num, u64_local den) {
    return __udivmoddi4(num, den, NULL);
}

// __umoddi3 - unsigned 64-bit modulo
u64_local __umoddi3(u64_local num, u64_local den) {
    u64_local rem;
    __udivmoddi4(num, den, &rem);
    return rem;
}

// __divdi3 - signed 64-bit division
i64_local __divdi3(i64_local num, i64_local den) {
    int neg = 0;
    if (num < 0) { num = -num; neg = !neg; }
    if (den < 0) { den = -den; neg = !neg; }
    u64_local result = __udivmoddi4((u64_local)num, (u64_local)den, NULL);
    return neg ? -(i64_local)result : (i64_local)result;
}

// __moddi3 - signed 64-bit modulo
i64_local __moddi3(i64_local num, i64_local den) {
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    if (den < 0) { den = -den; }
    u64_local rem;
    __udivmoddi4((u64_local)num, (u64_local)den, &rem);
    return neg ? -(i64_local)rem : (i64_local)rem;
}
