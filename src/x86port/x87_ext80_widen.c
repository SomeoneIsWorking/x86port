/* See x87_ext80_widen.h. */
#include "x87_ext80_widen.h"

/* ext80's exponent bias, and the all-ones exponent shared by infinity and NaN. */
#define EXT80_BIAS 16383
#define EXT80_EXP_MAX 0x7FFFu
#define EXT80_INTEGER_BIT 0x8000000000000000ull
/* The significand's most significant fraction bit: an ext80 NaN is quiet when
   this is set, and quieting a signalling one means setting it. */
#define EXT80_QUIET_BIT 0x4000000000000000ull

/*
 * The one shape both widths share.
 *
 * `mant` is the stored fraction, `exp` the stored exponent, `field` the width
 * of the fraction in bits and `bias`/`exp_max` the source format's. The
 * significand always ends up left-justified with its leading one at bit 63,
 * which is where ext80 keeps it explicitly rather than implying it.
 */
static X86pExt80 widen(uint64_t sign, uint32_t exp, uint64_t mant, uint32_t field, uint32_t exp_max, uint32_t bias) {
  const uint32_t shift = 63u - field;
  X86pExt80 out;
  out.signif = 0u;
  out.sign_exp = (uint16_t)(sign << 15);

  if (exp == exp_max) {
    /* Infinity has an empty fraction; a NaN keeps its payload in the same
       relative position, so a quiet one stays quiet and a signalling one is
       quieted by the bit that distinguishes them. */
    out.sign_exp |= (uint16_t)EXT80_EXP_MAX;
    out.signif = EXT80_INTEGER_BIT;
    if (mant != 0u) {
      out.signif |= (mant << shift) | EXT80_QUIET_BIT;
    }
    return out;
  }
  if (exp != 0u) {
    out.signif = (mant | ((uint64_t)1u << field)) << shift;
    out.sign_exp |= (uint16_t)(exp - bias + EXT80_BIAS);
    return out;
  }
  if (mant == 0u) {
    return out; /* a signed zero, whose ext80 form is an empty significand */
  }
  /*
   * A subnormal has no leading one to make explicit, so it is normalised into
   * one. ext80's exponent range is far wider than either source format's, so
   * every binary32 and binary64 subnormal is an ordinary normal number here
   * and nothing is lost. The shift loop only ever runs for subnormals, and a
   * builtin is deliberately not used: this stays plain C for every compiler
   * the project supports, and the path is cold.
   */
  {
    /*
     * Normalising k places left makes the value 1.f * 2^(1 - bias - k), so the
     * exponent starts where the format's smallest normal sits and drops by one
     * per shift. The fraction width does not appear: the shifts are what carry
     * it, and subtracting it here as well was wrong by exactly `field`.
     */
    int32_t biased = (int32_t)EXT80_BIAS + 1 - (int32_t)bias;
    while ((mant & ((uint64_t)1u << field)) == 0u) {
      mant <<= 1;
      biased--;
    }
    out.signif = mant << shift;
    out.sign_exp |= (uint16_t)biased;
  }
  return out;
}

X86pExt80 x86p_ext80_from_f32_bits(uint32_t bits) {
  return widen((uint64_t)(bits >> 31), (bits >> 23) & 0xFFu, bits & 0x7FFFFFu, 23u, 0xFFu, 127u);
}

X86pExt80 x86p_ext80_from_f64_bits(uint64_t bits) {
  return widen(bits >> 63, (uint32_t)((bits >> 52) & 0x7FFu), bits & 0xFFFFFFFFFFFFFull, 52u, 0x7FFu, 1023u);
}
