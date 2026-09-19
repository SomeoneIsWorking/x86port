/*
 * Exact widening of an IEEE binary32 or binary64 operand into the x87 ext80
 * encoding.
 *
 * FLD m32 and FLD m64 cannot round. Every binary32 and binary64 value --
 * normal, subnormal, zero, infinity and NaN alike -- is exactly representable
 * in ext80's 64-bit explicit significand and 15-bit exponent, so the whole
 * operation is a shift and a rebias. It raises no exception and consults no
 * rounding mode, which is why it takes no control word and returns no status.
 *
 * It had been going through the general softfloat conversion, which exists to
 * make rounding decisions this direction never has: 4.95% of the browser's
 * guest worker was spent there, second only to the arithmetic itself. The
 * conversion this replaces is the one the guest's loads hit on every frame.
 *
 * Kept apart from the register file and from the softfloat adapter because it
 * is neither: no host float type appears here, nothing about it depends on
 * what the host's `long double` happens to be, and its correctness is decided
 * entirely by the two encodings. That also lets the differential test below
 * run on a host whose x87 is REAL, and check this against the hardware the
 * format is named after.
 *
 * A signalling NaN is quieted and its payload preserved, matching both the
 * softfloat conversion this replaces and an x87 unit. Neither this function
 * nor the one it replaces reports the invalid-operation that a real FLD of a
 * signalling NaN raises; that gap is unchanged and is not this module's.
 */
#ifndef X86PORT_X87_EXT80_WIDEN_H
#define X86PORT_X87_EXT80_WIDEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The architectural pair, in the layout the guest stores and the softfloat
 * computes in: an explicit 64-bit significand, and the sign bit above a
 * 15-bit biased exponent. Deliberately not X86pX87Reg -- that type is the
 * host's `long double` where the host has a real x87, and this module is
 * about the encoding rather than about any host's storage of it.
 */
typedef struct X86pExt80 {
  uint64_t signif;
  uint16_t sign_exp;
} X86pExt80;

/* ext80's exponent bias. Published because the WebAssembly backend emits the
   normal case of this widening inline, and a second spelling of the rebias
   would be free to drift from the one below by a power of two. */
#define X86P_EXT80_BIAS 16383

/*
 * What a `width`-byte source operand's bits mean: the stored fraction's width,
 * the all-ones exponent that marks an infinity or a NaN, and the format's
 * bias. The sign is always the top bit and so is not described.
 *
 * A DESCRIPTOR AND NOT TWO LITERAL CALL SITES. These six numbers decide where
 * every field of an operand is, and the WebAssembly backend now emits the
 * shift-and-rebias for the normal case rather than calling it -- so they have
 * two consumers, in two languages, and a disagreement between them would
 * surface as a load that is wrong by a factor of two rather than as anything
 * that looks like a mistake.
 *
 * `field` is zero for a width that is not 4 or 8, which is the only refusal
 * this can have and is what a caller must test before using the rest.
 */
typedef struct X86pExt80Source {
  uint32_t field;
  uint32_t exp_max;
  uint32_t bias;
} X86pExt80Source;

X86pExt80Source x86p_ext80_source(unsigned width);

X86pExt80 x86p_ext80_from_f32_bits(uint32_t bits);
X86pExt80 x86p_ext80_from_f64_bits(uint64_t bits);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_EXT80_WIDEN_H */
