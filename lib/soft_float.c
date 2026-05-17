/*
 * lib/soft_float.c - Software floating-point emulation for CamelOS
 *
 * When compiled with -msoft-float, GCC generates calls to these routines
 * instead of x87/FPU instructions. On x86 Linux, libgcc.a does NOT contain
 * these soft-float routines because x86 always has hardware FPU support.
 *
 * This file provides all the soft-float routines needed by the kernel.
 * Implements IEEE 754 single and double precision arithmetic using
 * integer-only operations.
 *
 * Simplifications:
 *   - Denormals are flushed to zero (common in embedded soft-float libs)
 *   - Rounding mode: round-to-nearest-even (IEEE default)
 *   - NaN propagation: returns a quiet NaN
 *
 * Reference: IEEE 754-2008, GCC soft-float ABI
 */

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

/* ================================================================
 *  Helpers
 * ================================================================ */

static int clz32(u32 x) {
    if (x == 0) return 32;
    int n = 0;
    if (x <= 0x0000FFFFU) { n += 16; x <<= 16; }
    if (x <= 0x00FFFFFFU) { n +=  8; x <<=  8; }
    if (x <= 0x0FFFFFFFU) { n +=  4; x <<=  4; }
    if (x <= 0x3FFFFFFFU) { n +=  2; x <<=  2; }
    if (x <= 0x7FFFFFFFU) { n +=  1; }
    return n;
}

static int clz64(u64 x) {
    if (x == 0) return 64;
    u32 hi = (u32)(x >> 32);
    if (hi) return clz32(hi);
    return 32 + clz32((u32)x);
}

/* ================================================================
 *  IEEE 754 Single Precision (float)
 *  Layout: [S=1][E=8][M=23]  Bias = 127
 * ================================================================ */

#define SF_MANT_BITS   23
#define SF_EXP_BITS    8
#define SF_EXP_BIAS    127
#define SF_EXP_MASK    0x7F800000U
#define SF_MANT_MASK   0x007FFFFFU
#define SF_SIGN_MASK   0x80000000U
#define SF_QUIET_NAN   0x7FC00000U
#define SF_INF         0x7F800000U

typedef union { float f; u32 i; } sf_u;

/* Extract components */
static inline void sf_unpack(u32 v, u32 *sign, s32 *exp, u32 *mant) {
    *sign = v & SF_SIGN_MASK;
    *exp  = (s32)((v & SF_EXP_MASK) >> SF_MANT_BITS) - SF_EXP_BIAS;
    *mant = v & SF_MANT_MASK;
    if ((v & SF_EXP_MASK) != 0 && (v & SF_EXP_MASK) != SF_EXP_MASK)
        *mant |= 0x00800000U;  /* implicit 1 for normal numbers */
}

/* Pack components into a float bit pattern */
static u32 sf_pack(u32 sign, s32 exp, u64 mant) {
    /* Handle zero */
    if (mant == 0) return sign;

    /* Normalize: mant should have bit 23 set (the implicit 1 position) */
    if (mant & 0x01000000U) {
        /* 25-bit result from addition carry: shift right and round */
        int sticky = mant & 1;
        mant >>= 1;
        exp++;
        /* Round to nearest even */
        if (sticky && (mant & 1))
            mant++;
        if (mant & 0x01000000U) {
            mant >>= 1;
            exp++;
        }
    } else if (!(mant & 0x00800000U)) {
        int shift = clz32((u32)mant) - 8;
        if (shift > 0 && exp - shift > -SF_EXP_BIAS) {
            mant <<= shift;
            exp -= shift;
        } else {
            /* Flush denormal to zero */
            return sign;
        }
    }

    /* Check for overflow -> infinity */
    s32 biased_exp = exp + SF_EXP_BIAS;
    if (biased_exp >= 255) return sign | SF_INF;

    /* Check for underflow -> zero */
    if (biased_exp <= 0) return sign;

    return sign | ((u32)biased_exp << SF_MANT_BITS) | (mant & SF_MANT_MASK);
}

/* Is NaN? */
static inline int sf_is_nan(u32 v) {
    return ((v & SF_EXP_MASK) == SF_EXP_MASK) && (v & SF_MANT_MASK);
}

/* Is Infinity? */
static inline int sf_is_inf(u32 v) {
    return ((v & SF_EXP_MASK) == SF_EXP_MASK) && !(v & SF_MANT_MASK);
}

/* Is zero? (positive or negative) */
static inline int sf_is_zero(u32 v) {
    return (v & ~SF_SIGN_MASK) == 0;
}

/* ---- Addition ---- */
float __addsf3(float a, float b) {
    sf_u ua = {.f = a}, ub = {.f = b};
    u32 ia = ua.i, ib = ub.i;

    /* NaN propagation */
    if (sf_is_nan(ia)) return a;
    if (sf_is_nan(ib)) return b;

    /* Infinity handling */
    if (sf_is_inf(ia)) {
        if (sf_is_inf(ib) && (ia & SF_SIGN_MASK) != (ib & SF_SIGN_MASK)) {
            /* inf + (-inf) = NaN */
            sf_u r; r.i = SF_QUIET_NAN; return r.f;
        }
        return a;
    }
    if (sf_is_inf(ib)) return b;

    /* Zero handling */
    if (sf_is_zero(ia)) return b;
    if (sf_is_zero(ib)) return a;

    u32 sa, sb;
    s32 ea, eb;
    u32 ma, mb;
    sf_unpack(ia, &sa, &ea, &ma);
    sf_unpack(ib, &sb, &eb, &mb);

    /* Align exponents */
    s32 exp_diff = ea - eb;
    u32 result_sign;
    s32 result_exp;
    u64 result_mant;

    if (exp_diff >= 0) {
        result_exp = ea;
        if (exp_diff > 0) {
            if (exp_diff < 24)
                mb >>= exp_diff;
            else
                mb = 0;
        }
    } else {
        result_exp = eb;
        exp_diff = -exp_diff;
        if (exp_diff > 0) {
            if (exp_diff < 24)
                ma >>= exp_diff;
            else
                ma = 0;
        }
        /* Swap so ma is the larger magnitude */
        u32 ts; s32 te; u32 tm;
        ts = sa; sa = sb; sb = ts;
        te = ea; ea = eb; eb = te;  /* not needed further but keep consistent */
        tm = ma; ma = mb; mb = tm;
    }

    /* Add or subtract mantissas */
    if (sa == sb) {
        result_mant = (u64)ma + mb;
        result_sign = sa;
    } else {
        if (ma >= mb) {
            result_mant = (u64)ma - mb;
            result_sign = sa;
        } else {
            result_mant = (u64)mb - ma;
            result_sign = sb;
        }
    }

    if (result_mant == 0) {
        sf_u r; r.i = 0; return r.f;  /* positive zero */
    }

    sf_u r;
    r.i = sf_pack(result_sign, result_exp, result_mant);
    return r.f;
}

/* ---- Subtraction ---- */
float __subsf3(float a, float b) {
    sf_u ub = {.f = b};
    ub.i ^= SF_SIGN_MASK;  /* flip sign of b */
    return __addsf3(a, ub.f);
}

/* ---- Multiplication ---- */
float __mulsf3(float a, float b) {
    sf_u ua = {.f = a}, ub = {.f = b};
    u32 ia = ua.i, ib = ub.i;

    if (sf_is_nan(ia) || sf_is_nan(ib)) {
        sf_u r; r.i = SF_QUIET_NAN; return r.f;
    }

    u32 result_sign = (ia ^ ib) & SF_SIGN_MASK;

    if (sf_is_inf(ia)) {
        if (sf_is_zero(ib)) { sf_u r; r.i = SF_QUIET_NAN; return r.f; }
        sf_u r; r.i = result_sign | SF_INF; return r.f;
    }
    if (sf_is_inf(ib)) {
        if (sf_is_zero(ia)) { sf_u r; r.i = SF_QUIET_NAN; return r.f; }
        sf_u r; r.i = result_sign | SF_INF; return r.f;
    }
    if (sf_is_zero(ia) || sf_is_zero(ib)) {
        sf_u r; r.i = result_sign; return r.f;
    }

    u32 sa, sb;
    s32 ea, eb;
    u32 ma, mb;
    sf_unpack(ia, &sa, &ea, &ma);
    sf_unpack(ib, &sb, &eb, &mb);

    s32 result_exp = ea + eb - SF_EXP_BIAS + SF_EXP_BIAS;  /* remove double bias */
    /* Actually: ea and eb are already biased-removed. Real exponent = ea + eb. */
    /* But sf_pack adds the bias back, so we pass ea + eb directly. */
    result_exp = ea + eb;

    u64 result_mant = (u64)ma * mb;
    /* ma and mb are 24-bit values (bit 23 = implicit 1), product is up to 48 bits */
    /* We need to normalize: find the top 24 bits of the product */

    /* The product of two 24-bit numbers with bit 23 set gives a 47 or 48-bit result */
    /* Bit 47 or 46 will be the new implicit 1 position */

    /* Normalize: shift so that bit 47 is set (if it isn't, bit 46 is) */
    int shift;
    if (result_mant & 0x800000000000ULL) {
        /* 48-bit result, implicit 1 at bit 47 */
        /* We need to shift right by 24 to get mantissa in [bit 23] position */
        /* But first, we need to handle rounding of the bits we'll discard */
        u64 round_bits = result_mant & ((1ULL << 24) - 1);
        result_mant >>= 24;
        result_exp++;  /* adjust for the extra bit */
        /* Round to nearest even */
        if (round_bits > (1ULL << 23) ||
            (round_bits == (1ULL << 23) && (result_mant & 1))) {
            result_mant++;
        }
    } else {
        /* 47-bit result, implicit 1 at bit 46 */
        u64 round_bits = result_mant & ((1ULL << 23) - 1);
        result_mant >>= 23;
        /* Round to nearest even */
        if (round_bits > (1ULL << 22) ||
            (round_bits == (1ULL << 22) && (result_mant & 1))) {
            result_mant++;
        }
    }

    sf_u r;
    r.i = sf_pack(result_sign, result_exp, result_mant);
    return r.f;
}

/* ---- Division ---- */
float __divsf3(float a, float b) {
    sf_u ua = {.f = a}, ub = {.f = b};
    u32 ia = ua.i, ib = ub.i;

    if (sf_is_nan(ia) || sf_is_nan(ib)) {
        sf_u r; r.i = SF_QUIET_NAN; return r.f;
    }

    u32 result_sign = (ia ^ ib) & SF_SIGN_MASK;

    if (sf_is_inf(ia)) {
        sf_u r; r.i = result_sign | SF_INF; return r.f;
    }
    if (sf_is_inf(ib)) {
        sf_u r; r.i = result_sign; return r.f;  /* finite / inf = 0 */
    }
    if (sf_is_zero(ib)) {
        if (sf_is_zero(ia)) {
            sf_u r; r.i = SF_QUIET_NAN; return r.f;  /* 0/0 = NaN */
        }
        sf_u r; r.i = result_sign | SF_INF; return r.f;  /* x/0 = inf */
    }
    if (sf_is_zero(ia)) {
        sf_u r; r.i = result_sign; return r.f;
    }

    u32 sa, sb;
    s32 ea, eb;
    u32 ma, mb;
    sf_unpack(ia, &sa, &ea, &ma);
    sf_unpack(ib, &sb, &eb, &mb);

    s32 result_exp = ea - eb;
    u32 result_mant;

    /* Bit-by-bit division of 24-bit mantissas (avoids 64-bit division)
     * This avoids __udivmoddi4 which isn't available in freestanding.
     * We need 24 mantissa bits + 1 guard bit = 25 bits of quotient. */
    result_mant = 0;

    /* First quotient bit: if ma >= mb, quotient bit is 1 */
    if (ma >= mb) {
        ma -= mb;
        result_mant = 1;
    } else {
        result_exp--;
    }

    /* Generate remaining 24 bits */
    for (int i = 0; i < 24; i++) {
        result_mant <<= 1;
        ma <<= 1;
        if (ma >= mb) {
            ma -= mb;
            result_mant |= 1;
        }
    }

    /* result_mant has 25 bits: 24 mantissa bits + 1 guard bit for rounding */
    /* Round to nearest even using the guard bit */
    if (result_mant & 1) {
        result_mant >>= 1;
        result_mant++;
    } else {
        result_mant >>= 1;
    }

    sf_u r;
    r.i = sf_pack(result_sign, result_exp, result_mant);
    return r.f;
}

/* ---- float -> signed int ---- */
s32 __fixsfsi(float a) {
    sf_u ua = {.f = a};
    u32 ia = ua.i;

    if (sf_is_nan(ia)) return 0;

    u32 sign = ia & SF_SIGN_MASK;
    s32 exp = (s32)((ia & SF_EXP_MASK) >> SF_MANT_BITS) - SF_EXP_BIAS;
    u32 mant = (ia & SF_MANT_MASK);

    if (exp < 0) return 0;  /* |a| < 1 */

    if ((ia & SF_EXP_MASK) != 0)
        mant |= 0x00800000U;

    if (exp <= 23) {
        mant >>= (23 - exp);
    } else if (exp <= 30) {
        mant <<= (exp - 23);
    } else {
        /* Overflow: saturate */
        return sign ? (s32)0x80000000 : (s32)0x7FFFFFFF;
    }

    return sign ? -(s32)mant : (s32)mant;
}

/* ---- float -> unsigned int ---- */
u32 __fixunssfsi(float a) {
    sf_u ua = {.f = a};
    u32 ia = ua.i;

    if (sf_is_nan(ia)) return 0;
    if (ia & SF_SIGN_MASK) return 0;  /* negative */

    s32 exp = (s32)((ia & SF_EXP_MASK) >> SF_MANT_BITS) - SF_EXP_BIAS;
    u32 mant = (ia & SF_MANT_MASK);

    if (exp < 0) return 0;

    if ((ia & SF_EXP_MASK) != 0)
        mant |= 0x00800000U;

    if (exp <= 23) {
        mant >>= (23 - exp);
    } else if (exp <= 31) {
        mant <<= (exp - 23);
    } else {
        return 0xFFFFFFFFU;  /* overflow: saturate */
    }

    return mant;
}

/* ---- signed int -> float ---- */
float __floatsisf(s32 i) {
    if (i == 0) { sf_u r; r.i = 0; return r.f; }

    u32 sign = 0;
    u32 v;
    if (i < 0) {
        sign = SF_SIGN_MASK;
        v = (u32)(-(s64)i);  /* use 64-bit to handle INT_MIN */
    } else {
        v = (u32)i;
    }

    int lz = clz32(v);
    s32 exp = 31 - lz;
    u32 mant = v << (lz + 1);  /* shift so bit 31 is the implicit 1, bits 30:8 are mantissa */
    /* mant now has implicit 1 at bit 31, we need it at bit 23 */
    mant >>= 8;  /* shift right to get bits [30:8] -> [22:0] */

    sf_u r;
    r.i = sf_pack(sign, exp, mant);
    return r.f;
}

/* ---- unsigned int -> float ---- */
float __floatunsisf(u32 i) {
    if (i == 0) { sf_u r; r.i = 0; return r.f; }

    int lz = clz32(i);
    s32 exp = 31 - lz;
    u32 mant = i << (lz + 1);
    mant >>= 8;

    sf_u r;
    r.i = sf_pack(0, exp, mant);
    return r.f;
}

/* ---- Comparisons ---- */
/* GCC soft-float comparison ABI:
 *   __cmpsf2 returns: -1 if a<b, 0 if a==b, +1 if a>b
 *   __unordsf2 returns: 1 if either is NaN, 0 otherwise
 *   The individual __ltsf2, __gtsf2 etc. all just call the same comparison
 */

static s32 sf_cmp(float a, float b) {
    sf_u ua = {.f = a}, ub = {.f = b};
    u32 ia = ua.i, ib = ub.i;

    /* Handle NaN */
    if (sf_is_nan(ia) || sf_is_nan(ib)) return 2;  /* unordered */

    /* Handle zeros: -0 == +0 */
    if (sf_is_zero(ia) && sf_is_zero(ib)) return 0;

    /* Different signs? */
    u32 sa = ia & SF_SIGN_MASK;
    u32 sb = ib & SF_SIGN_MASK;
    if (sa != sb) return sa ? -1 : 1;

    /* Same sign: compare magnitudes */
    u32 abs_a = ia & ~SF_SIGN_MASK;
    u32 abs_b = ib & ~SF_SIGN_MASK;

    if (sa) {
        /* Both negative: larger magnitude = smaller value */
        if (abs_a > abs_b) return -1;
        if (abs_a < abs_b) return 1;
        return 0;
    } else {
        /* Both positive */
        if (abs_a < abs_b) return -1;
        if (abs_a > abs_b) return 1;
        return 0;
    }
}

s32 __eqsf2(float a, float b)   { return sf_cmp(a, b); }
s32 __nesf2(float a, float b)   { return sf_cmp(a, b); }
s32 __ltsf2(float a, float b)   { return sf_cmp(a, b); }
s32 __lesf2(float a, float b)   { return sf_cmp(a, b); }
s32 __gtsf2(float a, float b)   { return sf_cmp(a, b); }
s32 __gesf2(float a, float b)   { return sf_cmp(a, b); }
s32 __unordsf2(float a, float b) {
    sf_u ua = {.f = a}, ub = {.f = b};
    return (sf_is_nan(ua.i) || sf_is_nan(ub.i)) ? 1 : 0;
}


/* ================================================================
 *  IEEE 754 Double Precision (double)
 *  Layout: [S=1][E=11][M=52]  Bias = 1023
 * ================================================================ */

#define DF_MANT_BITS   52
#define DF_EXP_BITS    11
#define DF_EXP_BIAS    1023
#define DF_EXP_MASK    0x7FF0000000000000ULL
#define DF_MANT_MASK   0x000FFFFFFFFFFFFFULL
#define DF_SIGN_MASK   0x8000000000000000ULL
#define DF_QUIET_NAN   0x7FF8000000000000ULL
#define DF_INF         0x7FF0000000000000ULL

typedef union { double d; u64 i; } df_u;

static inline void df_unpack(u64 v, u64 *sign, s32 *exp, u64 *mant) {
    *sign = v & DF_SIGN_MASK;
    *exp  = (s32)((v & DF_EXP_MASK) >> DF_MANT_BITS) - DF_EXP_BIAS;
    *mant = v & DF_MANT_MASK;
    if ((v & DF_EXP_MASK) != 0 && (v & DF_EXP_MASK) != DF_EXP_MASK)
        *mant |= 0x0010000000000000ULL;  /* implicit 1 */
}

static u64 df_pack(u64 sign, s32 exp, u64 mant) {
    if (mant == 0) return sign;

    /* Normalize: mant should have bit 52 set */
    if (mant & 0x0020000000000000ULL) {
        /* 54-bit result from addition carry */
        int sticky = mant & 1;
        mant >>= 1;
        exp++;
        if (sticky && (mant & 1)) mant++;
        if (mant & 0x0020000000000000ULL) {
            mant >>= 1;
            exp++;
        }
    } else if (!(mant & 0x0010000000000000ULL)) {
        int shift = clz64(mant) - 11;
        if (shift > 0 && exp - shift > -DF_EXP_BIAS) {
            mant <<= shift;
            exp -= shift;
        } else {
            return sign;  /* flush denormal to zero */
        }
    }

    s32 biased_exp = exp + DF_EXP_BIAS;
    if (biased_exp >= 2047) return sign | DF_INF;
    if (biased_exp <= 0) return sign;

    return sign | ((u64)biased_exp << DF_MANT_BITS) | (mant & DF_MANT_MASK);
}

static inline int df_is_nan(u64 v) {
    return ((v & DF_EXP_MASK) == DF_EXP_MASK) && (v & DF_MANT_MASK);
}

static inline int df_is_inf(u64 v) {
    return ((v & DF_EXP_MASK) == DF_EXP_MASK) && !(v & DF_MANT_MASK);
}

static inline int df_is_zero(u64 v) {
    return (v & ~DF_SIGN_MASK) == 0;
}

/* ---- Double Addition ---- */
double __adddf3(double a, double b) {
    df_u ua = {.d = a}, ub = {.d = b};
    u64 ia = ua.i, ib = ub.i;

    if (df_is_nan(ia)) return a;
    if (df_is_nan(ib)) return b;

    if (df_is_inf(ia)) {
        if (df_is_inf(ib) && (ia & DF_SIGN_MASK) != (ib & DF_SIGN_MASK)) {
            df_u r; r.i = DF_QUIET_NAN; return r.d;
        }
        return a;
    }
    if (df_is_inf(ib)) return b;

    if (df_is_zero(ia)) return b;
    if (df_is_zero(ib)) return a;

    u64 sa, sb;
    s32 ea, eb;
    u64 ma, mb;
    df_unpack(ia, &sa, &ea, &ma);
    df_unpack(ib, &sb, &eb, &mb);

    s32 exp_diff = ea - eb;
    u64 result_sign;
    s32 result_exp;
    u64 result_mant;

    if (exp_diff >= 0) {
        result_exp = ea;
        if (exp_diff > 0) {
            if (exp_diff < 53)
                mb >>= exp_diff;
            else
                mb = 0;
        }
    } else {
        result_exp = eb;
        exp_diff = -exp_diff;
        if (exp_diff > 0) {
            if (exp_diff < 53)
                ma >>= exp_diff;
            else
                ma = 0;
        }
        u64 ts; s32 te; u64 tm;
        ts = sa; sa = sb; sb = ts;
        te = ea; ea = eb; eb = te;
        tm = ma; ma = mb; mb = tm;
    }

    if (sa == sb) {
        result_mant = ma + mb;
        result_sign = sa;
    } else {
        if (ma >= mb) {
            result_mant = ma - mb;
            result_sign = sa;
        } else {
            result_mant = mb - ma;
            result_sign = sb;
        }
    }

    if (result_mant == 0) { df_u r; r.i = 0; return r.d; }

    df_u r;
    r.i = df_pack(result_sign, result_exp, result_mant);
    return r.d;
}

/* ---- Double Subtraction ---- */
double __subdf3(double a, double b) {
    df_u ub = {.d = b};
    ub.i ^= DF_SIGN_MASK;
    return __adddf3(a, ub.d);
}

/* ---- Double Multiplication ---- */
double __muldf3(double a, double b) {
    df_u ua = {.d = a}, ub = {.d = b};
    u64 ia = ua.i, ib = ub.i;

    if (df_is_nan(ia) || df_is_nan(ib)) {
        df_u r; r.i = DF_QUIET_NAN; return r.d;
    }

    u64 result_sign = (ia ^ ib) & DF_SIGN_MASK;

    if (df_is_inf(ia)) {
        if (df_is_zero(ib)) { df_u r; r.i = DF_QUIET_NAN; return r.d; }
        df_u r; r.i = result_sign | DF_INF; return r.d;
    }
    if (df_is_inf(ib)) {
        if (df_is_zero(ia)) { df_u r; r.i = DF_QUIET_NAN; return r.d; }
        df_u r; r.i = result_sign | DF_INF; return r.d;
    }
    if (df_is_zero(ia) || df_is_zero(ib)) {
        df_u r; r.i = result_sign; return r.d;
    }

    u64 sa, sb;
    s32 ea, eb;
    u64 ma, mb;
    df_unpack(ia, &sa, &ea, &ma);
    df_unpack(ib, &sb, &eb, &mb);

    s32 result_exp = ea + eb;

    /* Multiply two 53-bit mantissas. Result is up to 106 bits.
     * We only need the top 54 bits for the result. */
    u64 a_hi = ma >> 21;
    u64 a_lo = ma & 0x1FFFFFULL;
    u64 b_hi = mb >> 21;
    u64 b_lo = mb & 0x1FFFFFULL;

    u64 p0 = a_lo * b_lo;
    u64 p1 = a_hi * b_lo;
    u64 p2 = a_lo * b_hi;
    u64 p3 = a_hi * b_hi;

    u64 mid = p1 + (p0 >> 21) + p2;
    u64 result_mant = p3 + (mid >> 21);

    /* result_mant now has the top bits, with implicit 1 at a variable position */
    /* The product of two numbers with bit 52 set will have bit 104 or 105 set */

    if (result_mant & 0x0020000000000000ULL) {
        /* 54-bit result (bit 53 set) */
        u64 round_bits = mid & 0x1FFFFFULL;
        result_exp++;
        /* Shift right by 1 to get 53-bit mantissa */
        int sticky = result_mant & 1;
        result_mant >>= 1;
        if ((round_bits > (1ULL << 20)) ||
            (round_bits == (1ULL << 20) && (sticky || (result_mant & 1)))) {
            result_mant++;
        }
    } else {
        /* 53-bit result (bit 52 set) */
        u64 round_bits = mid & 0x1FFFFFULL;
        if ((round_bits > (1ULL << 20)) ||
            (round_bits == (1ULL << 20) && (result_mant & 1))) {
            result_mant++;
            if (result_mant & 0x0020000000000000ULL) {
                result_mant >>= 1;
                result_exp++;
            }
        }
    }

    df_u r;
    r.i = df_pack(result_sign, result_exp, result_mant);
    return r.d;
}

/* ---- Double Division ---- */
double __divdf3(double a, double b) {
    df_u ua = {.d = a}, ub = {.d = b};
    u64 ia = ua.i, ib = ub.i;

    if (df_is_nan(ia) || df_is_nan(ib)) {
        df_u r; r.i = DF_QUIET_NAN; return r.d;
    }

    u64 result_sign = (ia ^ ib) & DF_SIGN_MASK;

    if (df_is_inf(ia)) {
        if (df_is_inf(ib)) { df_u r; r.i = DF_QUIET_NAN; return r.d; }
        df_u r; r.i = result_sign | DF_INF; return r.d;
    }
    if (df_is_inf(ib)) {
        df_u r; r.i = result_sign; return r.d;
    }
    if (df_is_zero(ib)) {
        if (df_is_zero(ia)) { df_u r; r.i = DF_QUIET_NAN; return r.d; }
        df_u r; r.i = result_sign | DF_INF; return r.d;
    }
    if (df_is_zero(ia)) {
        df_u r; r.i = result_sign; return r.d;
    }

    u64 sa, sb;
    s32 ea, eb;
    u64 ma, mb;
    df_unpack(ia, &sa, &ea, &ma);
    df_unpack(ib, &sb, &eb, &mb);

    s32 result_exp = ea - eb;

    /* Long division of 53-bit mantissas */
    /* We need to compute (ma << N) / mb to get enough precision */
    /* Shift dividend left to get at least 53 bits of quotient */

    u64 result_mant = 0;
    int shift = 53;

    if (ma < mb) {
        /* Result will be < 1 in mantissa space, need to shift */
        ma <<= 1;
        result_exp--;
    }

    /* Simple bit-by-bit division */
    for (int i = 0; i <= shift; i++) {
        result_mant <<= 1;
        if (ma >= mb) {
            ma -= mb;
            result_mant |= 1;
        }
        ma <<= 1;
    }

    /* result_mant now has 54 bits (53 + 1 guard bit) */
    /* Round */
    if (result_mant & 1) {
        result_mant >>= 1;
        result_mant++;
        if (result_mant & 0x0020000000000000ULL) {
            result_mant >>= 1;
            result_exp++;
        }
    } else {
        result_mant >>= 1;
    }

    df_u r;
    r.i = df_pack(result_sign, result_exp, result_mant);
    return r.d;
}

/* ---- double -> signed int ---- */
s32 __fixdfsi(double a) {
    df_u ua = {.d = a};
    u64 ia = ua.i;

    if (df_is_nan(ia)) return 0;

    u64 sign = ia & DF_SIGN_MASK;
    s32 exp = (s32)((ia & DF_EXP_MASK) >> DF_MANT_BITS) - DF_EXP_BIAS;
    u64 mant = ia & DF_MANT_MASK;

    if (exp < 0) return 0;

    if ((ia & DF_EXP_MASK) != 0)
        mant |= 0x0010000000000000ULL;

    if (exp <= 52) {
        mant >>= (52 - exp);
    } else if (exp <= 62) {
        mant <<= (exp - 52);
    } else {
        return sign ? (s32)0x80000000 : (s32)0x7FFFFFFF;
    }

    return sign ? -(s32)(u32)mant : (s32)(u32)mant;
}

/* ---- double -> unsigned int ---- */
u32 __fixunsdfsi(double a) {
    df_u ua = {.d = a};
    u64 ia = ua.i;

    if (df_is_nan(ia)) return 0;
    if (ia & DF_SIGN_MASK) return 0;

    s32 exp = (s32)((ia & DF_EXP_MASK) >> DF_MANT_BITS) - DF_EXP_BIAS;
    u64 mant = ia & DF_MANT_MASK;

    if (exp < 0) return 0;

    if ((ia & DF_EXP_MASK) != 0)
        mant |= 0x0010000000000000ULL;

    if (exp <= 52) {
        mant >>= (52 - exp);
    } else if (exp <= 63) {
        mant <<= (exp - 52);
    } else {
        return 0xFFFFFFFFU;
    }

    return (u32)mant;
}

/* ---- double -> unsigned long long ---- */
u64 __fixunsdfdi(double a) {
    df_u ua = {.d = a};
    u64 ia = ua.i;

    if (df_is_nan(ia)) return 0;
    if (ia & DF_SIGN_MASK) return 0;

    s32 exp = (s32)((ia & DF_EXP_MASK) >> DF_MANT_BITS) - DF_EXP_BIAS;
    u64 mant = ia & DF_MANT_MASK;

    if (exp < 0) return 0;

    if ((ia & DF_EXP_MASK) != 0)
        mant |= 0x0010000000000000ULL;

    if (exp <= 52) {
        mant >>= (52 - exp);
    } else if (exp <= 63) {
        mant <<= (exp - 52);
    } else {
        return 0xFFFFFFFFFFFFFFFFULL;
    }

    return mant;
}

/* ---- signed int -> double ---- */
double __floatsidf(s32 i) {
    if (i == 0) { df_u r; r.i = 0; return r.d; }

    u64 sign = 0;
    u32 v;
    if (i < 0) {
        sign = DF_SIGN_MASK;
        v = (u32)(-(s64)i);
    } else {
        v = (u32)i;
    }

    int lz = clz32(v);
    s32 exp = 31 - lz;
    u64 mant = (u64)v << (lz + 1);
    mant >>= (32 - 53);  /* shift to get bits [52:0] */

    df_u r;
    r.i = df_pack(sign, exp, mant);
    return r.d;
}

/* ---- unsigned int -> double ---- */
double __floatunsidf(u32 i) {
    if (i == 0) { df_u r; r.i = 0; return r.d; }

    int lz = clz32(i);
    s32 exp = 31 - lz;
    u64 mant = (u64)i << (lz + 1);
    mant >>= (32 - 53);

    df_u r;
    r.i = df_pack(0, exp, mant);
    return r.d;
}

/* ---- double -> float (truncation) ---- */
float __truncdfsf2(double a) {
    df_u ua = {.d = a};
    u64 ia = ua.i;

    if (df_is_nan(ia)) { sf_u r; r.i = SF_QUIET_NAN; return r.f; }

    u64 sign = ia & DF_SIGN_MASK ? SF_SIGN_MASK : 0;

    if (df_is_inf(ia)) { sf_u r; r.i = sign | SF_INF; return r.f; }

    if (df_is_zero(ia)) { sf_u r; r.i = (u32)sign; return r.f; }

    s32 exp = (s32)((ia & DF_EXP_MASK) >> DF_MANT_BITS) - DF_EXP_BIAS;
    u64 mant = ia & DF_MANT_MASK;
    if ((ia & DF_EXP_MASK) != 0)
        mant |= 0x0010000000000000ULL;

    /* Convert double mantissa (53 bits) to float mantissa (24 bits) */
    u32 sf_mant;
    if (exp > 127) {
        /* Overflow to infinity */
        sf_u r; r.i = sign | SF_INF; return r.f;
    }
    if (exp < -126) {
        /* Underflow to zero */
        sf_u r; r.i = (u32)sign; return r.f;
    }

    /* Shift 53-bit mantissa to 24-bit */
    if (53 > 24) {
        int shift = 53 - 24;  /* 29 bits to discard */
        u64 round_bits = mant & ((1ULL << shift) - 1);
        mant >>= shift;
        /* Round to nearest even */
        if (round_bits > (1ULL << (shift - 1)) ||
            (round_bits == (1ULL << (shift - 1)) && (mant & 1))) {
            mant++;
            if (mant & 0x01000000U) {
                mant >>= 1;
                exp++;
                if (exp > 127) {
                    sf_u r; r.i = sign | SF_INF; return r.f;
                }
            }
        }
    }
    sf_mant = (u32)mant;

    sf_u r;
    r.i = sf_pack(sign, exp, sf_mant);
    return r.f;
}

/* ---- Double Comparisons ---- */
static s32 df_cmp(double a, double b) {
    df_u ua = {.d = a}, ub = {.d = b};
    u64 ia = ua.i, ib = ub.i;

    if (df_is_nan(ia) || df_is_nan(ib)) return 2;

    if (df_is_zero(ia) && df_is_zero(ib)) return 0;

    u64 sa = ia & DF_SIGN_MASK;
    u64 sb = ib & DF_SIGN_MASK;
    if (sa != sb) return sa ? -1 : 1;

    u64 abs_a = ia & ~DF_SIGN_MASK;
    u64 abs_b = ib & ~DF_SIGN_MASK;

    if (sa) {
        if (abs_a > abs_b) return -1;
        if (abs_a < abs_b) return 1;
        return 0;
    } else {
        if (abs_a < abs_b) return -1;
        if (abs_a > abs_b) return 1;
        return 0;
    }
}

s32 __eqdf2(double a, double b)   { return df_cmp(a, b); }
s32 __nedf2(double a, double b)   { return df_cmp(a, b); }
s32 __ltdf2(double a, double b)   { return df_cmp(a, b); }
s32 __ledf2(double a, double b)   { return df_cmp(a, b); }
s32 __gtdf2(double a, double b)   { return df_cmp(a, b); }
s32 __gedf2(double a, double b)   { return df_cmp(a, b); }
s32 __unorddf2(double a, double b) {
    df_u ua = {.d = a}, ub = {.d = b};
    return (df_is_nan(ua.i) || df_is_nan(ub.i)) ? 1 : 0;
}

/* ================================================================
 *  64-bit Integer Division Helpers
 *  Required on 32-bit targets when code uses 64-bit arithmetic.
 *  Normally provided by libgcc, but we're freestanding.
 * ================================================================ */

/* Combined unsigned divmod: returns quotient, stores remainder via pointer */
u64 __udivmoddi4(u64 num, u64 den, u64 *rem) {
    if (den == 0) {
        if (rem) *rem = 0;
        return 0;
    }
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

/* 64-bit unsigned division */
u64 __udivdi3(u64 num, u64 den) {
    return __udivmoddi4(num, den, (u64*)0);
}

/* 64-bit unsigned modulo */
u64 __umoddi3(u64 num, u64 den) {
    u64 rem;
    __udivmoddi4(num, den, &rem);
    return rem;
}

/* 64-bit signed division */
s64 __divdi3(s64 num, s64 den) {
    int neg = 0;
    if (num < 0) { num = -num; neg = !neg; }
    if (den < 0) { den = -den; neg = !neg; }
    u64 result = __udivmoddi4((u64)num, (u64)den, (u64*)0);
    return neg ? -(s64)result : (s64)result;
}

/* 64-bit signed modulo */
s64 __moddi3(s64 num, s64 den) {
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    if (den < 0) { den = -den; }
    u64 rem;
    __udivmoddi4((u64)num, (u64)den, &rem);
    return neg ? -(s64)rem : (s64)rem;
}
