#ifndef TYPES_H
#define TYPES_H

// Standard unsigned integer types
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

// Standard signed integer types
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

// Size types
typedef unsigned int       size_t;
typedef int                ssize_t;

// Pointer-sized integer types (for pointer <-> integer casts)
typedef unsigned int       uintptr_t;
typedef int                intptr_t;

// Null pointer definition
#ifndef NULL
#define NULL ((void*)0)
#endif


#endif