/* See x87_ext80_arith.h. */
#include "x87_ext80_arith.h"

/* x87's status bits, as the softfloat adapter reports them: these are the
   architectural values, and the adapter passes Bochs's exception flags
   through unchanged, so one spelling serves both. */
enum {
  kInexact = 0x20u,   /* PE */
  kRoundedUp = 0x200u /* C1, set when the rounding went away from zero */
};

enum { kExt80MaxExp = 0x7FFFu, kExplicitOne = 1 };

int x86p_ext80_is_normal(X86pExt80 v) {
  const unsigned exponent = (unsigned)(v.sign_exp & 0x7FFFu);
  return exponent - 1u < (unsigned)(kExt80MaxExp - 1u) && (v.signif >> 63) != 0u;
}

int x86p_ext80_is_zero(X86pExt80 v) {
  return (v.sign_exp & 0x7FFFu) == 0u && v.signif == 0u;
}

int x86p_ext80_control_is_ordinary(uint16_t control) {
  return (control & X86P_X87_RC_MASK) == X86P_X87_RC_NEAREST && (control & X86P_X87_PC_MASK) == X86P_X87_PC_EXTENDED;
}

/*
 * The 64x64 to 128 product, spelled out.
 *
 * WebAssembly has no multiply-high, so this is what the one instruction an
 * x87 unit spends on a significand becomes; writing it here rather than
 * reaching for a 128-bit type keeps the cost visible and keeps this file free
 * of a compiler extension.
 */
static void mul64_to_128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
  const uint64_t a0 = a & 0xFFFFFFFFull;
  const uint64_t a1 = a >> 32;
  const uint64_t b0 = b & 0xFFFFFFFFull;
  const uint64_t b1 = b >> 32;
  const uint64_t p00 = a0 * b0;
  const uint64_t p01 = a0 * b1;
  const uint64_t p10 = a1 * b0;
  const uint64_t p11 = a1 * b1;
  const uint64_t middle = p10 + (p00 >> 32) + (p01 & 0xFFFFFFFFull);
  *lo = (middle << 32) | (p00 & 0xFFFFFFFFull);
  *hi = p11 + (middle >> 32) + (p01 >> 32);
}

int x86p_ext80_mul_ordinary(uint16_t control, X86pExt80 x, X86pExt80 y, X86pExt80 *out, uint16_t *flags) {
  int exponent;
  uint64_t hi;
  uint64_t lo;
  uint16_t sign;
  /* Accumulated locally and only published on success: a refusal must leave
     the caller's status word exactly as it found it, because the softfloat
     that then runs will report the whole operation itself. */
  uint16_t raised = 0u;
  if (!out || !flags) {
    return 0;
  }
  if (!x86p_ext80_control_is_ordinary(control)) {
    return 0;
  }
  if (!x86p_ext80_is_normal(x) || !x86p_ext80_is_normal(y)) {
    return 0;
  }
  sign = (uint16_t)((x.sign_exp ^ y.sign_exp) & 0x8000u);
  /* Two biased exponents carry the bias twice, and the product's leading one
     lands one place above the operands', which is the -0x3FFE rather than the
     -0x3FFF a plain rebias would give. */
  exponent = (int)(x.sign_exp & 0x7FFFu) + (int)(y.sign_exp & 0x7FFFu) - (X86P_EXT80_BIAS - 1);
  mul64_to_128(x.signif, y.signif, &hi, &lo);
  /* Two significands in [1, 2) multiply to [1, 4): the top bit is set unless
     both were close to one, and then the product is normalised by doubling. */
  if ((hi >> 63) == 0u) {
    hi = (hi << 1) | (lo >> 63);
    lo <<= 1;
    exponent--;
  }
  /*
   * Round to nearest, ties to even, on the 64 bits below the significand.
   * Bochs increments on a tie and clears the low bit afterwards; the test is
   * written out here because "greater than half, or exactly half with an odd
   * significand" is the rule, and the two spellings must agree.
   */
  if (lo > 0x8000000000000000ull || (lo == 0x8000000000000000ull && (hi & 1u) != 0u)) {
    hi++;
    if (hi == 0u) {
      /* The rounding carried out of the significand: the result is the next
         power of two. */
      hi = 0x8000000000000000ull;
      exponent++;
    }
    raised |= kRoundedUp;
  }
  if (lo != 0u) {
    raised |= kInexact;
  }
  /* An exponent at either edge is an overflow or a subnormal, and both raise
     flags and produce encodings this does not make. */
  if (exponent <= 0 || exponent >= kExt80MaxExp) {
    return 0;
  }
  out->signif = hi;
  out->sign_exp = (uint16_t)(sign | (unsigned)exponent);
  *flags |= raised;
  return 1;
}
