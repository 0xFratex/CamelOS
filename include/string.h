#ifndef STRING_H
#define STRING_H

#include "types.h"

// GCC Builtins for varargs
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)
typedef __builtin_va_list va_list;

// Memory manipulation
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* destination, const void* source, size_t num);
void* memmove(void* dest, const void* src, size_t n);
int   memcmp(const void* ptr1, const void* ptr2, size_t num);

// String manipulation
size_t strlen(const char* str);
int    strcmp(const char* s1, const char* s2);
int    strncmp(const char* s1, const char* s2, size_t n);
char*  strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);
char*  strcat(char* dest, const char* src);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* haystack, const char* needle);

// Utility
void int_to_str(int num, char* str);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsprintf(char* buf, const char* fmt, va_list args);
void printk(const char* fmt, ...);

// Network utilities
void ip_to_str(uint32_t ip, char* str);
void mac_to_str(uint8_t* mac, char* str);

#endif