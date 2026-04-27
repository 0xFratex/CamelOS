#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

typedef __builtin_va_list va_list;

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int atoi(const char *nptr);
long atol(const char *nptr);

#endif