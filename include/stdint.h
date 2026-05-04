#ifndef _STDINT_H
#define _STDINT_H

#include "types.h"

// Standard integer types - already defined in types.h
// This header exists for compatibility with third-party code

#define INT8_MIN    (-128)
#define INT16_MIN   (-32768)
#define INT32_MIN   (-2147483647-1)
#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define UINT8_MAX   255
#define UINT16_MAX  65535
#define UINT32_MAX  4294967295U

#define SIZE_MAX    ((size_t)-1)

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#define PTRDIFF_MIN INT32_MIN
#define PTRDIFF_MAX INT32_MAX

#endif
