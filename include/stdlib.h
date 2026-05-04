#ifndef _STDLIB_H
#define _STDLIB_H

#include "types.h"
#include "string.h"

// Minimal stdlib compatibility for MuJS / third-party libs

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

// Memory allocation (maps to CamelOS kmalloc/kfree)
void* malloc(size_t size);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

// String to number conversion
int atoi(const char* str);
long atol(const char* str);
long strtol(const char* str, char** endptr, int base);
unsigned long strtoul(const char* str, char** endptr, int base);
double strtod(const char* str, char** endptr);
double atof(const char* str);

// Pseudo-random
int rand(void);
void srand(unsigned int seed);

// Abort (calls kernel panic)
void abort(void);
void exit(int status);

// Absolute value
int abs(int x);
long labs(long x);

// Div
typedef struct { int quot; int rem; } div_t;
div_t div(int num, int denom);

#endif
