#ifndef _MATH_H
#define _MATH_H

// Minimal math functions for MuJS
// MuJS needs: floor, ceil, fmod, pow, sqrt, log, log10, sin, cos, tan, etc.

#define HUGE_VAL    (__builtin_huge_val())
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))
#define M_PI        3.14159265358979323846
#define M_E         2.7182818284590452354
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_SQRT2     1.41421356237309504880

double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double fabs(double x);
double sqrt(double x);
double pow(double x, double y);
double log(double x);
double log10(double x);
double log2(double x);
double exp(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double round(double x);
double trunc(double x);
double copysign(double x, double y);
int isnan(double x);
int isinf(double x);
int isfinite(double x);
int signbit(double x);

double frexp(double x, int* exp);
double ldexp(double x, int exp);
double modf(double x, double* iptr);

#define fpclassify(x) \
    (__builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x))

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#endif
