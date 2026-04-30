// installer/soft_div.c - Software 64-bit division for installer (no libgcc)
// Provides __udivdi3, __umoddi3 used by 64-bit arithmetic in bare-metal 32-bit mode

typedef unsigned int u32;
typedef unsigned long long u64;

// 64-bit unsigned division
u64 __udivdi3(u64 num, u64 den) {
    if (den == 0) return 0;
    u64 quot = 0;
    int shift = 0;
    while (den <= num && !(den & ((u64)1 << 63))) { den <<= 1; shift++; }
    while (shift >= 0) {
        if (num >= den) { num -= den; quot |= (u64)1 << shift; }
        den >>= 1; shift--;
    }
    return quot;
}

// 64-bit unsigned modulo
u64 __umoddi3(u64 num, u64 den) {
    if (den == 0) return 0;
    int shift = 0;
    while (den <= num && !(den & ((u64)1 << 63))) { den <<= 1; shift++; }
    while (shift >= 0) {
        if (num >= den) { num -= den; }
        den >>= 1; shift--;
    }
    return num;
}
