// utils/string.h
#ifndef STRING_H
#define STRING_H

// Define types manually since we're using -nostdinc
typedef unsigned int uint32_t;

static inline void *memset(void *dest, int val, uint32_t count) {
    char *temp = (char *)dest;
    for (; count != 0; count--) *temp++ = val;
    return dest;
}

static inline int strcmp(const char s1[], const char s2[]) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

static inline void strcpy(char *dest, const char *src) {
    int i = 0;
    while (src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

#endif
