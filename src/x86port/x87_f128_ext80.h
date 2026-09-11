/*
 * Exact representation conversion between IEEE binary128 and the x87 ext80
 * format, for hosts whose `long double` is binary128 (AArch64 Linux/Android,
 * Emscripten). Every ext80 value is exactly representable in binary128, and a
 * binary128 value that came from ext80 carries no significand bits below the
 * 64-bit boundary -- so both directions are pure bit reassembly for the values
 * the guest FPU actually produces, with no rounding decision to make.
 *
 * Each function REFUSES (returns 0) for anything that is not a finite normal
 * with an exactly representable significand: zero, subnormal, infinity, NaN,
 * and ext80 unnormals. The caller falls back to the general softfloat
 * conversion there, which stays the single authority on rounding and status.
 */
#ifndef X86PORT_X87_F128_EXT80_H
#define X86PORT_X87_F128_EXT80_H

#include "softfloat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `bits` is sixteen bytes of little-endian binary128 storage. */
int x86p_x87_f128_to_ext80_exact(const void *bits, floatx80 *out);
int x86p_x87_ext80_to_f128_exact(floatx80 value, void *bits);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_X87_F128_EXT80_H */
