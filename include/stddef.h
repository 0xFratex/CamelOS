#ifndef _STDDEF_H
#define _STDDEF_H

#include "types.h"

// Standard definitions
#define offsetof(type, member) ((size_t)&((type*)0)->member)

typedef int ptrdiff_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif
