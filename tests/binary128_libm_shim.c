/*
 * Test-only (cmake/binary128_model.cmake): the `long double` libm calls of a
 * -mlong-double-128 build, bound to glibc's _Float128 implementations. glibc's
 * own `*l` symbols take ext80, so without this every call would read a
 * binary128 argument as ten ext80 bytes -- the model's artefact, not Bionic's.
 * Extend it when the build starts calling another `*l` function.
 */
#define __STDC_WANT_IEC_60559_TYPES_EXT__ 1
#include <math.h>

_Static_assert(sizeof(long double) == 16 && __LDBL_MANT_DIG__ == 113, "needs -mlong-double-128");

long double fmodl(long double x, long double y) {
  return fmodf128(x, y);
}

long double frexpl(long double x, int *exponent) {
  return frexpf128(x, exponent);
}

long double scalbnl(long double x, int n) {
  return scalbnf128(x, n);
}

long double ldexpl(long double x, int n) {
  return ldexpf128(x, n);
}

long double cosl(long double x) {
  return cosf128(x);
}

long double sinl(long double x) {
  return sinf128(x);
}

long double sqrtl(long double x) {
  return sqrtf128(x);
}
