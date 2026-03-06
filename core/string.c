// core/string.c
#include "../include/string.h"

// GCC Builtins for varargs
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)
typedef __builtin_va_list va_list;

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char* strcpy(char* dest, const char* src) {
    char* tmp = dest;
    while ((*dest++ = *src++));
    return tmp;
}

// FIXED: Proper strncpy that zero-pads the destination
char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* tmp = dest;
    while (*tmp) tmp++;
    while ((*tmp++ = *src++));
    return dest;
}

char* strchr(const char* s, int c) {
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char*)s;
}

char* strrchr(const char* s, int c) {
    const char* res = NULL;
    do {
        if (*s == (char)c) res = s;
    } while (*s++);
    return (char*)res;
}

char* strstr(const char* haystack, const char* needle) {
    if (*needle == '\0') return (char*)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

int memcmp(const void* ptr1, const void* ptr2, size_t num) {
    const unsigned char* p1 = (const unsigned char*)ptr1;
    const unsigned char* p2 = (const unsigned char*)ptr2;
    
    while (num--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    
    return 0;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    
    if (d < s) {
        // Forward copy
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        // Backward copy to avoid overlap
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    
    return dest;
}



void int_to_str(int num, char* str) {
    int i = 0;
    int is_neg = 0;
    if(num == 0) { str[0]='0'; str[1]=0; return; }
    if(num < 0) { is_neg=1; num=-num; }

    while(num != 0) {
        str[i++] = '0' + (num % 10);
        num /= 10;
    }
    if(is_neg) str[i++] = '-';
    str[i] = 0;

    // Reverse
    for(int j=0; j<i/2; j++) {
        char t = str[j];
        str[j] = str[i-1-j];
        str[i-1-j] = t;
    }
}

// vsprintf
int vsprintf(char* buf, const char* fmt, va_list args) {
    char* orig_buf = buf;
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                fmt++;
                const char* str = va_arg(args, const char*);
                if(!str) str = "(null)";
                while (*str) *buf++ = *str++;
            } else if (*fmt == 'd') {
                fmt++;
                int val = va_arg(args, int);
                char tmp[32];
                int_to_str(val, tmp);
                char* p = tmp;
                while(*p) *buf++ = *p++;
            } else if (*fmt == '0' && *(fmt+1) == '2' && *(fmt+2) == 'X') {
                fmt += 3;
                int val = va_arg(args, int);
                char hex[] = "0123456789ABCDEF";
                *buf++ = hex[(val >> 4) & 0xF];
                *buf++ = hex[val & 0xF];
            } else if (*fmt == 'c') {
                fmt++;
                char c = (char)va_arg(args, int);
                *buf++ = c;
            } else {
                *buf++ = *fmt++;
            }
        } else {
            *buf++ = *fmt++;
        }
    }
    *buf = 0;
    return buf - orig_buf;
}

// Simple sprintf implementation
int sprintf(char* buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsprintf(buf, fmt, args);
    va_end(args);
    return len;
}

// Simple snprintf implementation
int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    // For simplicity, ignore size and use vsprintf
    int len = vsprintf(buf, fmt, args);
    va_end(args);
    return len;
}

// Simple atoi implementation
int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return sign * result;
}

// Simple printk implementation
void printk(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    extern void s_printf(const char*); // From serial
    s_printf(buf);
}
