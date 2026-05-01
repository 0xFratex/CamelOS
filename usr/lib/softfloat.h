// usr/lib/softfloat.h - Proper soft-float implementation for CamelOS
// Implements GCC's libgcc soft-float functions using integer arithmetic
// IEEE 754 double-precision (64-bit) operations
//
// This replaces the broken stubs that were returning wrong values,
// which caused the Elk JS engine to fail on any floating-point operation.

#ifndef SOFTFLOAT_H
#define SOFTFLOAT_H

typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

// ============================================================================
// IEEE 754 double-precision helpers
// ============================================================================
// Double layout: sign(1) | exponent(11) | mantissa(52)
// Bias = 1023

// Union for type-punning between double and uint64_t
typedef union {
    double d;
    uint64_t u;
    struct {
        uint32_t lo;
        uint32_t hi;
    } parts;
} double_bits_t;

static inline int sf_is_nan(uint64_t bits) {
    uint32_t exp = (uint32_t)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0x000FFFFFFFFFFFFF;
    return (exp == 0x7FF) && (mant != 0);
}

static inline int sf_is_inf(uint64_t bits) {
    uint32_t exp = (uint32_t)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0x000FFFFFFFFFFFFF;
    return (exp == 0x7FF) && (mant == 0);
}

static inline int sf_is_zero(uint64_t bits) {
    return (bits & 0x7FFFFFFFFFFFFFFF) == 0;
}

static inline int sf_sign(uint64_t bits) {
    return (bits >> 63) & 1;
}

static inline int32_t sf_exp(uint64_t bits) {
    return (int32_t)((bits >> 52) & 0x7FF);
}

static inline uint64_t sf_mant(uint64_t bits) {
    return bits & 0x000FFFFFFFFFFFFF;
}

// Normalize a denormalized number: returns (mantissa_with_hidden_bit, exponent)
static inline void sf_normalize(uint64_t mant, int32_t exp, uint64_t* out_mant, int32_t* out_exp) {
    if (mant == 0) {
        *out_mant = 0;
        *out_exp = 0;
        return;
    }
    // Find the highest set bit
    int shift = 0;
    while (!(mant & 0x0010000000000000ULL) && shift < 53) {
        mant <<= 1;
        shift++;
    }
    *out_mant = mant;
    *out_exp = exp - shift;
}

// Pack a double from sign, exponent, and mantissa
static inline double sf_pack(int sign, int32_t exp, uint64_t mant) {
    // Handle overflow -> Infinity
    if (exp >= 0x7FF) {
        double_bits_t r;
        r.u = ((uint64_t)sign << 63) | ((uint64_t)0x7FF << 52);
        return r.d;
    }
    // Handle underflow -> Zero or denormal
    if (exp <= 0) {
        if (exp < -52) {
            double_bits_t r;
            r.u = ((uint64_t)sign << 63);
            return r.d;
        }
        // Denormalized number
        int shift = 1 - exp;
        mant >>= shift;
        double_bits_t r;
        r.u = ((uint64_t)sign << 63) | mant;
        return r.d;
    }
    // Normal number - round mantissa to 52 bits
    // mant has the hidden bit at bit 52
    double_bits_t r;
    r.u = ((uint64_t)sign << 63) | ((uint64_t)(exp & 0x7FF) << 52) | (mant & 0x000FFFFFFFFFFFFF);
    return r.d;
}

// Unpack a double into sign, exponent, and mantissa with hidden bit
static inline void sf_unpack(double d, int* sign, int32_t* exp, uint64_t* mant) {
    double_bits_t b;
    b.d = d;
    *sign = sf_sign(b.u);
    int32_t raw_exp = sf_exp(b.u);
    *mant = sf_mant(b.u);
    
    if (raw_exp == 0) {
        // Denormalized or zero
        if (*mant == 0) {
            *exp = 0;
        } else {
            // Normalize denormalized
            sf_normalize(*mant, 0, mant, exp);
            *exp = *exp + 1 - 1023; // Adjust for bias
        }
    } else if (raw_exp == 0x7FF) {
        // Inf or NaN - keep as is
        *exp = raw_exp;
        *mant |= 0x0010000000000000ULL; // Set hidden bit for convenience
    } else {
        // Normal number
        *exp = raw_exp - 1023;
        *mant |= 0x0010000000000000ULL; // Add hidden bit
    }
}

// ============================================================================
// Soft-float arithmetic operations
// ============================================================================

static double sf_add(double a, double b) {
    double_bits_t ba, bb;
    ba.d = a;
    bb.d = b;
    
    // Handle special cases
    if (sf_is_nan(ba.u) || sf_is_nan(bb.u)) {
        double_bits_t r;
        r.u = 0x7FF8000000000000ULL; // NaN
        return r.d;
    }
    if (sf_is_inf(ba.u)) {
        if (sf_is_inf(bb.u) && sf_sign(ba.u) != sf_sign(bb.u)) {
            double_bits_t r;
            r.u = 0x7FF8000000000000ULL; // Inf - Inf = NaN
            return r.d;
        }
        return a;
    }
    if (sf_is_inf(bb.u)) return b;
    if (sf_is_zero(ba.u)) return b;
    if (sf_is_zero(bb.u)) return a;
    
    int sign_a, sign_b;
    int32_t exp_a, exp_b;
    uint64_t mant_a, mant_b;
    
    sf_unpack(a, &sign_a, &exp_a, &mant_a);
    sf_unpack(b, &sign_b, &exp_b, &mant_b);
    
    // Align exponents
    int32_t exp_diff = exp_a - exp_b;
    int32_t result_exp;
    
    if (exp_diff > 0) {
        // A has larger exponent, shift B's mantissa right
        if (exp_diff < 64) {
            mant_b >>= exp_diff;
        } else {
            mant_b = 0;
        }
        result_exp = exp_a;
    } else if (exp_diff < 0) {
        // B has larger exponent, shift A's mantissa right
        int32_t shift = -exp_diff;
        if (shift < 64) {
            mant_a >>= shift;
        } else {
            mant_a = 0;
        }
        result_exp = exp_b;
    } else {
        result_exp = exp_a;
    }
    
    // Add or subtract mantissas
    int result_sign;
    uint64_t result_mant;
    
    if (sign_a == sign_b) {
        result_mant = mant_a + mant_b;
        result_sign = sign_a;
    } else {
        if (mant_a >= mant_b) {
            result_mant = mant_a - mant_b;
            result_sign = sign_a;
        } else {
            result_mant = mant_b - mant_a;
            result_sign = sign_b;
        }
    }
    
    // Handle zero result
    if (result_mant == 0) {
        double_bits_t r;
        r.u = 0; // +0.0
        return r.d;
    }
    
    // Normalize result
    if (result_mant & 0x0020000000000000ULL) {
        // Carry bit, shift right and increment exponent
        result_mant >>= 1;
        result_exp++;
    }
    
    // Shift to put hidden bit at position 52
    while (result_mant && !(result_mant & 0x0010000000000000ULL)) {
        result_mant <<= 1;
        result_exp--;
    }
    
    return sf_pack(result_sign, result_exp + 1023, result_mant);
}

static double sf_sub(double a, double b) {
    // Negate b and add
    double_bits_t bb;
    bb.d = b;
    bb.u ^= 0x8000000000000000ULL; // Flip sign
    return sf_add(a, bb.d);
}

static double sf_mul(double a, double b) {
    double_bits_t ba, bb;
    ba.d = a;
    bb.d = b;
    
    // Handle special cases
    if (sf_is_nan(ba.u) || sf_is_nan(bb.u)) {
        double_bits_t r;
        r.u = 0x7FF8000000000000ULL;
        return r.d;
    }
    if (sf_is_inf(ba.u)) {
        if (sf_is_zero(bb.u)) {
            double_bits_t r;
            r.u = 0x7FF8000000000000ULL; // Inf * 0 = NaN
            return r.d;
        }
        double_bits_t r;
        r.u = ((uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63) | ((uint64_t)0x7FF << 52);
        return r.d;
    }
    if (sf_is_inf(bb.u)) {
        if (sf_is_zero(ba.u)) {
            double_bits_t r;
            r.u = 0x7FF8000000000000ULL;
            return r.d;
        }
        double_bits_t r;
        r.u = ((uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63) | ((uint64_t)0x7FF << 52);
        return r.d;
    }
    if (sf_is_zero(ba.u) || sf_is_zero(bb.u)) {
        double_bits_t r;
        r.u = (uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63;
        return r.d;
    }
    
    int sign_a, sign_b;
    int32_t exp_a, exp_b;
    uint64_t mant_a, mant_b;
    
    sf_unpack(a, &sign_a, &exp_a, &mant_a);
    sf_unpack(b, &sign_b, &exp_b, &mant_b);
    
    int result_sign = sign_a ^ sign_b;
    int32_t result_exp = exp_a + exp_b;
    
    // Multiply mantissas (53-bit * 53-bit -> 106-bit result)
    // We use the upper 64 bits of the 128-bit product
    // Split mantissa into high and low 32-bit parts
    uint32_t a_hi = (uint32_t)(mant_a >> 21);
    uint32_t a_lo = (uint32_t)(mant_a & 0x1FFFFF);
    uint32_t b_hi = (uint32_t)(mant_b >> 21);
    uint32_t b_lo = (uint32_t)(mant_b & 0x1FFFFF);
    
    uint64_t product = (uint64_t)a_hi * b_hi;
    uint64_t cross1 = (uint64_t)a_hi * b_lo;
    uint64_t cross2 = (uint64_t)a_lo * b_hi;
    
    // Add cross products (shifted appropriately)
    product += (cross1 >> 21) + (cross2 >> 21);
    
    // The product now has the hidden bit at position ~42
    // We need it at position 52
    // Shift left by 10 to get to position 52
    if (product) {
        // Normalize: put the top bit at position 52
        while (product && !(product & 0x0010000000000000ULL)) {
            product <<= 1;
            result_exp--;
        }
        while (product & 0x0020000000000000ULL) {
            product >>= 1;
            result_exp++;
        }
    }
    
    if (product == 0) {
        double_bits_t r;
        r.u = (uint64_t)result_sign << 63;
        return r.d;
    }
    
    return sf_pack(result_sign, result_exp + 1023, product);
}

static double sf_div(double a, double b) {
    double_bits_t ba, bb;
    ba.d = a;
    bb.d = b;
    
    // Handle special cases
    if (sf_is_nan(ba.u) || sf_is_nan(bb.u)) {
        double_bits_t r;
        r.u = 0x7FF8000000000000ULL;
        return r.d;
    }
    if (sf_is_inf(ba.u)) {
        if (sf_is_inf(bb.u)) {
            double_bits_t r;
            r.u = 0x7FF8000000000000ULL; // Inf / Inf = NaN
            return r.d;
        }
        double_bits_t r;
        r.u = ((uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63) | ((uint64_t)0x7FF << 52);
        return r.d;
    }
    if (sf_is_inf(bb.u)) {
        double_bits_t r;
        r.u = (uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63;
        return r.d;
    }
    if (sf_is_zero(bb.u)) {
        if (sf_is_zero(ba.u)) {
            double_bits_t r;
            r.u = 0x7FF8000000000000ULL; // 0/0 = NaN
            return r.d;
        }
        double_bits_t r;
        r.u = ((uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63) | ((uint64_t)0x7FF << 52);
        return r.d;
    }
    if (sf_is_zero(ba.u)) {
        double_bits_t r;
        r.u = (uint64_t)(sf_sign(ba.u) ^ sf_sign(bb.u)) << 63;
        return r.d;
    }
    
    int sign_a, sign_b;
    int32_t exp_a, exp_b;
    uint64_t mant_a, mant_b;
    
    sf_unpack(a, &sign_a, &exp_a, &mant_a);
    sf_unpack(b, &sign_b, &exp_b, &mant_b);
    
    int result_sign = sign_a ^ sign_b;
    int32_t result_exp = exp_a - exp_b;
    
    // Division: mant_a / mant_b
    // Both have the hidden bit at position 52
    // Shift dividend left by 52 to get fractional bits
    uint64_t dividend = mant_a;
    uint64_t divisor = mant_b;
    uint64_t quotient = 0;
    
    // Simple long division
    // Shift dividend to get more precision
    for (int i = 0; i < 53; i++) {
        quotient <<= 1;
        if (dividend >= divisor) {
            dividend -= divisor;
            quotient |= 1;
        }
        // Shift dividend left safely
        if (dividend & 0x8000000000000000ULL) {
            // Would overflow on shift; adjust
            dividend = (dividend >> 1);
            divisor = (divisor >> 1);
        } else {
            dividend <<= 1;
        }
    }
    
    // Normalize
    while (quotient && !(quotient & 0x0010000000000000ULL)) {
        quotient <<= 1;
        result_exp--;
    }
    while (quotient & 0x0020000000000000ULL) {
        quotient >>= 1;
        result_exp++;
    }
    
    if (quotient == 0) {
        double_bits_t r;
        r.u = (uint64_t)result_sign << 63;
        return r.d;
    }
    
    return sf_pack(result_sign, result_exp + 1023, quotient);
}

// Comparison: returns -1 if a < b, 0 if a == b, 1 if a > b, 2 if unordered
static int sf_cmp(double a, double b) {
    double_bits_t ba, bb;
    ba.d = a;
    bb.d = b;
    
    if (sf_is_nan(ba.u) || sf_is_nan(bb.u)) return 2; // unordered
    
    int sign_a = sf_sign(ba.u);
    int sign_b = sf_sign(bb.u);
    
    if (sf_is_zero(ba.u) && sf_is_zero(bb.u)) return 0;
    if (sf_is_inf(ba.u)) {
        if (sign_a == 0) return 1;  // +Inf > anything (except NaN)
        return -1; // -Inf < anything
    }
    if (sf_is_inf(bb.u)) {
        if (sign_b == 0) return -1;
        return 1;
    }
    
    // Same sign: compare bits directly (works for IEEE 754)
    if (sign_a == sign_b) {
        // Both positive: compare magnitudes
        if (sign_a == 0) {
            if (ba.u < bb.u) return -1;
            if (ba.u > bb.u) return 1;
            return 0;
        } else {
            // Both negative: reverse comparison
            if (ba.u > bb.u) return -1;
            if (ba.u < bb.u) return 1;
            return 0;
        }
    }
    
    // Different signs
    if (sign_a == 0) return 1;  // positive > negative
    return -1; // negative < positive
}

// Integer to double conversion
static double sf_int_to_double(int32_t i) {
    if (i == 0) {
        double_bits_t r;
        r.u = 0;
        return r.d;
    }
    
    int sign = 0;
    int32_t val = i;
    if (val < 0) {
        sign = 1;
        val = -val;
    }
    
    // Find the highest set bit
    int32_t exp = 31;
    uint32_t mask = 0x80000000;
    while (!(val & mask) && exp > 0) {
        exp--;
        mask >>= 1;
    }
    
    // Shift val so the MSB is at bit 52 (hidden bit position)
    uint64_t mant = (uint64_t)val;
    int shift = 52 - exp;
    if (shift > 0) {
        mant <<= shift;
    } else if (shift < 0) {
        mant >>= -shift;
    }
    
    return sf_pack(sign, exp + 1023, mant);
}

// Unsigned int to double
static double sf_uint_to_double(uint32_t u) {
    if (u == 0) {
        double_bits_t r;
        r.u = 0;
        return r.d;
    }
    
    // Find the highest set bit
    int32_t exp = 31;
    uint32_t mask = 0x80000000;
    while (!(u & mask) && exp > 0) {
        exp--;
        mask >>= 1;
    }
    
    uint64_t mant = (uint64_t)u;
    int shift = 52 - exp;
    if (shift > 0) {
        mant <<= shift;
    } else if (shift < 0) {
        mant >>= -shift;
    }
    
    return sf_pack(0, exp + 1023, mant);
}

// Double to int conversion (truncate toward zero)
static int32_t sf_double_to_int(double d) {
    double_bits_t b;
    b.d = d;
    
    if (sf_is_nan(b.u) || sf_is_inf(b.u)) return 0;
    if (sf_is_zero(b.u)) return 0;
    
    int sign = sf_sign(b.u);
    int32_t exp = sf_exp(b.u) - 1023;
    uint64_t mant = sf_mant(b.u);
    
    if (exp < 0) return 0; // |d| < 1
    if (exp >= 31) {
        // Overflow
        return sign ? (int32_t)0x80000000 : (int32_t)0x7FFFFFFF;
    }
    
    // Add hidden bit
    if (sf_exp(b.u) != 0) {
        mant |= 0x0010000000000000ULL;
    }
    
    // Shift to get integer part
    int shift = 52 - exp;
    if (shift > 0) {
        mant >>= shift;
    } else {
        mant <<= -shift;
    }
    
    int32_t result = (int32_t)(mant & 0xFFFFFFFF);
    return sign ? -result : result;
}

// Double to unsigned int conversion
static uint32_t sf_double_to_uint(double d) {
    double_bits_t b;
    b.d = d;
    
    if (sf_is_nan(b.u) || sf_is_inf(b.u) || sf_sign(b.u)) return 0;
    if (sf_is_zero(b.u)) return 0;
    
    int32_t exp = sf_exp(b.u) - 1023;
    uint64_t mant = sf_mant(b.u);
    
    if (exp < 0) return 0;
    if (exp >= 32) return 0xFFFFFFFF;
    
    if (sf_exp(b.u) != 0) {
        mant |= 0x0010000000000000ULL;
    }
    
    int shift = 52 - exp;
    if (shift > 0) {
        mant >>= shift;
    } else {
        mant <<= -shift;
    }
    
    return (uint32_t)(mant & 0xFFFFFFFF);
}

// ============================================================================
// GCC soft-float API implementations
// These replace the broken stubs in browser_cdl.c
// ============================================================================

// Convert unsigned int to double
double __floatunsidf(unsigned int i) {
    return sf_uint_to_double(i);
}

// Convert signed int to double
double __floatsidf(int a) {
    return sf_int_to_double(a);
}

// Double addition
double __adddf3(double a, double b) {
    return sf_add(a, b);
}

// Double subtraction
double __subdf3(double a, double b) {
    return sf_sub(a, b);
}

// Double multiplication
double __muldf3(double a, double b) {
    return sf_mul(a, b);
}

// Double division
double __divdf3(double a, double b) {
    return sf_div(a, b);
}

// Double comparison: a < b -> return -1, a == b -> 0, a > b -> 1
// GCC's convention: returns negative if a < b, 0 if equal, positive if a > b
int __ltdf2(double a, double b) {
    int c = sf_cmp(a, b);
    if (c == 2) return 1; // unordered is treated as less than for LT
    return (c < 0) ? -1 : ((c > 0) ? 1 : 1); // LT: return -1 if true, 1 if false
}

int __ledf2(double a, double b) {
    int c = sf_cmp(a, b);
    if (c == 2) return 1;
    return (c <= 0) ? -1 : 1;
}

int __gtdf2(double a, double b) {
    int c = sf_cmp(a, b);
    if (c == 2) return -1;
    return (c > 0) ? 1 : -1;
}

int __gedf2(double a, double b) {
    int c = sf_cmp(a, b);
    if (c == 2) return -1;
    return (c >= 0) ? 1 : -1;
}

int __eqdf2(double a, double b) {
    int c = sf_cmp(a, b);
    if (c == 2) return 1; // unordered != equal
    return (c == 0) ? 0 : 1;
}

int __nedf2(double a, double b) {
    int c = sf_cmp(a, b);
    if (c == 2) return 1;
    return (c != 0) ? 1 : 0;
}

int __unorddf2(double a, double b) {
    double_bits_t ba, bb;
    ba.d = a;
    bb.d = b;
    return (sf_is_nan(ba.u) || sf_is_nan(bb.u)) ? 1 : 0;
}

// Double to signed int
int __fixdfsi(double a) {
    return sf_double_to_int(a);
}

// Double to unsigned int
unsigned int __fixunsdfsi(double a) {
    return sf_double_to_uint(a);
}

#endif // SOFTFLOAT_H
