/*
 * x87_ext80_integer.h -- integers to and from the ten-byte encoding, with no
 * host float in between.
 *
 * FILD and FIST convert between a guest integer and an x87 register, and every
 * integer up to 64 bits is exact in ext80's 64-bit significand. Routing them
 * through the host's `long double` cost nothing on a host with a real x87 and
 * two binary128 softfloat conversions on one without -- `__floatsitf` and
 * `__fixtfdi` were 3% of the ARM64 phone's port library. These work on the
 * encoding's fields and are the one implementation on every host.
 */
#ifndef X86PORT_X87_EXT80_INTEGER_H
#define X86PORT_X87_EXT80_INTEGER_H

#include "x87_ext80_widen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The integer exactly: a zero is +0, anything else a normal. */
X86pExt80 x86p_ext80_from_int(int64_t value);

/*
 * The value rounded to an integer by the control word's RC field, as FIST
 * does, into `width` bytes (2, 4 or 8) as a signed integer. Returns 0, leaving
 * `out` alone, when the result does not fit or the encoding is a NaN, an
 * infinity or an unnormal: the invalid operation a caller reports.
 */
int x86p_ext80_to_int(uint16_t control, X86pExt80 value, int width, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_X87_EXT80_INTEGER_H */
