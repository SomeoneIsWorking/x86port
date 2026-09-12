/*
 * Bit-level binary128 operations for the x87 register file.
 *
 * A host whose `long double` is IEEE binary128 -- AArch64 Linux and Android --
 * has no hardware for it. Every `==`, `isnan`, `<` and every widening from
 * float or double becomes a compiler-rt call (__cmptf2, __unordtf2,
 * __extendsftf2, __extenddftf2). Measured on a Huawei BKY-W09 running the
 * X-Men 2 port: the tag classifier alone was 9.5% of the frame, __cmptf2 a
 * further 7.7%, and the two widenings 5.2%.
 *
 * Every operation here is exact and answers the same question the arithmetic
 * form answers -- it reads the fields instead of calling a soft-float library
 * to derive them. Widening a float or a double to binary128 is exact for every
 * input including subnormals and NaN payloads, and ordering is the IEEE total
 * order with the two zeros made equal, so no result changes.
 *
 * The bit-pattern core takes and returns X86pF128, so it is testable on every
 * host. The `long double` wrappers exist only where that type IS binary128.
 */
#ifndef X86PORT_X87_BINARY128_H
#define X86PORT_X87_BINARY128_H

#include <float.h>
#include <stdint.h>

/* The `long double` wrappers copy bytes into the halves below, so they need
   the host's own little-endian layout as well as the format. */
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384 && defined(__BYTE_ORDER__) &&                                        \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define X86P_X87_BINARY128 1
#endif

/* One binary128 value, little-endian halves: `hi` carries the sign, the
   15-bit exponent and the top 48 mantissa bits. */
typedef struct X86pF128 {
  uint64_t lo;
  uint64_t hi;
} X86pF128;

int x86p_f128_is_zero(X86pF128 v);
int x86p_f128_is_nan(X86pF128 v);
int x86p_f128_is_inf(X86pF128 v);

X86pF128 x86p_f128_from_f32(uint32_t bits);
X86pF128 x86p_f128_from_f64(uint64_t bits);

/* Ordered comparison for two non-NaN values: -1, 0 or 1. The zeros compare
   equal, as IEEE comparison requires and a bare bit compare would not. */
int x86p_f128_compare(X86pF128 a, X86pF128 b);

#ifdef X86P_X87_BINARY128
X86pF128 x86p_f128_of(long double v);
long double x86p_f128_value(X86pF128 v);
#endif

#endif /* X86PORT_X87_BINARY128_H */
