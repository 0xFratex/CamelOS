#ifndef _STDIO_H
#define _STDIO_H

#include "types.h"
#include "string.h"

// Minimal stdio compatibility for MuJS / third-party libs
// Maps to CamelOS kernel equivalents

#define EOF (-1)
#define BUFSIZ 256
#define FILE void

// stdout/stderr stubs (MuJS uses these for debug output)
#define stdout ((FILE*)1)
#define stderr ((FILE*)2)

// Core formatted output
int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int sscanf(const char* str, const char* fmt, ...);

// Character I/O (stubs - output to serial)
int putchar(int c);
int puts(const char* s);
int fputc(int c, FILE* f);
int fputs(const char* s, FILE* f);

// File stubs (MuJS doesn't really do file I/O in our embedded use)
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
size_t fread(void* ptr, size_t size, size_t count, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t count, FILE* f);
int fseek(FILE* f, long offset, int whence);
long ftell(FILE* f);
void rewind(FILE* f);
int feof(FILE* f);
int fflush(FILE* f);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
