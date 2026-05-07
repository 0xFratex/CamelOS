// installer/soft_div.c - Software 64-bit division for installer (no libgcc)
// Provides __udivdi3, __umoddi3, __divdi3, __moddi3 used by 64-bit
// arithmetic in bare-metal 32-bit mode (e.g. stb_truetype needs signed)

typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

// Shared unsigned divmod core
static u64 udivmod(u64 num, u64 den, u64 *rem) {
    if (den == 0) { if (rem) *rem = 0; return 0; }
    u64 quot = 0;
    int shift = 0;
    while (den <= num && !(den & ((u64)1 << 63))) { den <<= 1; shift++; }
    while (shift >= 0) {
        if (num >= den) { num -= den; quot |= (u64)1 << shift; }
        den >>= 1; shift--;
    }
    if (rem) *rem = num;
    return quot;
}

// 64-bit unsigned division
u64 __udivdi3(u64 num, u64 den) {
    return udivmod(num, den, (u64*)0);
}

// 64-bit unsigned modulo
u64 __umoddi3(u64 num, u64 den) {
    u64 rem;
    udivmod(num, den, &rem);
    return rem;
}

// 64-bit signed division (needed by stb_truetype etc.)
i64 __divdi3(i64 num, i64 den) {
    int neg = 0;
    if (num < 0) { num = -num; neg = !neg; }
    if (den < 0) { den = -den; neg = !neg; }
    u64 result = udivmod((u64)num, (u64)den, (u64*)0);
    return neg ? -(i64)result : (i64)result;
}

// 64-bit signed modulo
i64 __moddi3(i64 num, i64 den) {
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    if (den < 0) { den = -den; }
    u64 rem;
    udivmod((u64)num, (u64)den, &rem);
    return neg ? -(i64)rem : (i64)rem;
}
