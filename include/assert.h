#ifndef _ASSERT_H
#define _ASSERT_H

// Minimal assert for MuJS compatibility
// In kernel mode, assert is a no-op or calls panic

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) do { if (!(x)) { extern void panic(const char*); panic("assertion failed: " #x); } } while(0)
#endif

#endif
