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

int x86p_ext80_is_normal_or_zero(X86pExt80 v) {
  return x86p_ext80_is_normal(v) || x86p_ext80_is_zero(v);
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
  sign = (uint16_t)((x.sign_exp ^ y.sign_exp) & 0x8000u);
  /*
   * A zero operand is 44.6% of what the game's route performs -- measured, and
   * the reason this is here rather than left to the softfloat. Times anything
   * finite the answer is a zero of the combined sign, exactly, raising nothing.
   */
  if (x86p_ext80_is_zero(x) || x86p_ext80_is_zero(y)) {
    if (!x86p_ext80_is_normal_or_zero(x) || !x86p_ext80_is_normal_or_zero(y)) {
      return 0;
    }
    out->signif = 0u;
    out->sign_exp = sign;
    return 1;
  }
  if (!x86p_ext80_is_normal(x) || !x86p_ext80_is_normal(y)) {
    return 0;
  }
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

/*
 * The count of leading zeroes, spelled out for the same reason the 128-bit
 * product is: this is the reference the WebAssembly backend's i64.clz has to
 * agree with, and a compiler builtin here would hide which one is authority.
 * Never called with zero -- an exactly cancelled result is refused above it.
 */
static int clz64(uint64_t v) {
  int n = 0;
  while ((v >> 63) == 0u) {
    v <<= 1;
    n++;
  }
  return n;
}

/*
 * The smaller operand's significand, aligned `shift` places below the larger's,
 * as a 128-bit pair plus whether anything fell off the bottom.
 *
 * The sticky bit is not an optimisation: two operands whose exponents differ by
 * more than 128 still decide a rounding, and dropping the fact that something
 * non-zero was there turns a round-up into a tie.
 */
static void align_smaller(uint64_t signif, unsigned shift, uint64_t *hi, uint64_t *lo, int *sticky) {
  if (shift == 0u) {
    *hi = signif;
    *lo = 0u;
    *sticky = 0;
  } else if (shift < 64u) {
    *hi = signif >> shift;
    *lo = signif << (64u - shift);
    *sticky = 0;
  } else if (shift == 64u) {
    *hi = 0u;
    *lo = signif;
    *sticky = 0;
  } else if (shift < 128u) {
    *hi = 0u;
    *lo = signif >> (shift - 64u);
    *sticky = (signif << (128u - shift)) != 0u;
  } else {
    /* Every bit is below the pair, and a normal's significand is never zero. */
    *hi = 0u;
    *lo = 0u;
    *sticky = 1;
  }
}

int x86p_ext80_add_ordinary(uint16_t control, X86pExt80 x, X86pExt80 y, int subtract, X86pExt80 *out, uint16_t *flags) {
  X86pExt80 big;
  X86pExt80 small;
  unsigned small_sign;
  unsigned big_exp;
  uint64_t hi;
  uint64_t lo;
  uint64_t shi;
  uint64_t slo;
  int sticky = 0;
  int exponent;
  unsigned sign;
  uint16_t raised = 0u;
  if (!out || !flags) {
    return 0;
  }
  if (!x86p_ext80_control_is_ordinary(control)) {
    return 0;
  }
  if (!x86p_ext80_is_normal_or_zero(x) || !x86p_ext80_is_normal_or_zero(y)) {
    return 0;
  }
  /* FSUB is FADD with the subtrahend's sign flipped, and nothing else. */
  small_sign = (unsigned)(y.sign_exp & 0x8000u);
  if (subtract) {
    small_sign ^= 0x8000u;
  }
  /*
   * A zero operand, which is most of what the route's additions have: the sum
   * is the other operand exactly, and two zeros give their common sign or, when
   * they disagree, the positive one that round-to-nearest specifies.
   */
  if (x86p_ext80_is_zero(x) || x86p_ext80_is_zero(y)) {
    if (!x86p_ext80_is_normal_or_zero(x) || !x86p_ext80_is_normal_or_zero(y)) {
      return 0;
    }
    if (!x86p_ext80_is_zero(y)) {
      out->signif = y.signif;
      out->sign_exp = (uint16_t)((unsigned)(y.sign_exp & 0x7FFFu) | small_sign);
      return 1;
    }
    if (!x86p_ext80_is_zero(x)) {
      *out = x;
      return 1;
    }
    out->signif = 0u;
    out->sign_exp = (uint16_t)((x.sign_exp & 0x8000u) == small_sign ? small_sign : 0u);
    return 1;
  }

  /*
   * The larger magnitude first, so the alignment shift is never negative and
   * the result's sign is always the larger operand's. `small_sign` ends up
   * holding whichever operand did NOT win, which is what the same-sign test
   * below needs.
   */
  {
    const unsigned ex = (unsigned)(x.sign_exp & 0x7FFFu);
    const unsigned ey = (unsigned)(y.sign_exp & 0x7FFFu);
    if (ey > ex || (ey == ex && y.signif > x.signif)) {
      big = y;
      small = x;
      sign = small_sign;
      small_sign = (unsigned)(x.sign_exp & 0x8000u);
    } else {
      big = x;
      small = y;
      sign = (unsigned)(x.sign_exp & 0x8000u);
    }
  }
  big_exp = (unsigned)(big.sign_exp & 0x7FFFu);
  exponent = (int)big_exp;
  align_smaller(small.signif, big_exp - (unsigned)(small.sign_exp & 0x7FFFu), &shi, &slo, &sticky);

  if (sign == small_sign) {
    /*
     * Same sign: a magnitude add. Two significands below 2^64 sum below 2^65,
     * so the only normalisation is one place right when the sum carried out,
     * and the bit that falls off the pair is sticky rather than lost.
     */
    lo = slo;
    hi = big.signif + shi;
    if (hi < big.signif) {
      sticky |= (int)(lo & 1u);
      lo = (lo >> 1) | (hi << 63);
      hi = (hi >> 1) | 0x8000000000000000ull;
      exponent++;
    }
  } else {
    /*
     * Opposite signs: a magnitude subtract. The bits that fell below the pair
     * are a positive remainder still to be taken off, so one is borrowed from
     * the bottom and what is left over is non-zero -- which is exactly what a
     * sticky bit means to the rounding below.
     */
    lo = 0u - slo;
    hi = big.signif - shi - (slo != 0u ? 1u : 0u);
    if (sticky) {
      if (lo == 0u) {
        lo = ~(uint64_t)0u;
        hi--;
      } else {
        lo--;
      }
    }
    if (hi == 0u && lo == 0u) {
      /* An exact cancellation: the zero's sign is the rounding mode's rule
         and not this one's. */
      return 0;
    }
    if ((hi >> 63) == 0u) {
      int shift;
      if (hi == 0u) {
        hi = lo;
        lo = 0u;
        exponent -= 64;
      }
      shift = clz64(hi);
      if (shift != 0) {
        hi = (hi << shift) | (lo >> (64 - shift));
        lo <<= shift;
        exponent -= shift;
      }
    }
  }

  /* Round to nearest, ties to even, on the 64 bits below the significand. */
  {
    const uint64_t half = 0x8000000000000000ull;
    if (lo != 0u || sticky) {
      raised |= kInexact;
    }
    if (lo > half || (lo == half && (sticky || (hi & 1u) != 0u))) {
      hi++;
      if (hi == 0u) {
        hi = half;
        exponent++;
      }
      raised |= kRoundedUp;
    }
  }
  if (exponent <= 0 || exponent >= kExt80MaxExp) {
    return 0;
  }
  out->signif = hi;
  out->sign_exp = (uint16_t)(sign | (unsigned)exponent);
  *flags |= raised;
  return 1;
}
