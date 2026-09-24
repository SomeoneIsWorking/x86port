/* See x87_ext80_narrow.h. */
#include "x87_ext80_narrow.h"

#define EXT80_EXP_MAX 0x7FFFu
#define EXT80_INTEGER_BIT 0x8000000000000000ull

int x86p_ext80_narrow_nearest(X86pExt80 value, unsigned width, uint64_t *out) {
  const X86pExt80Source target = x86p_ext80_source(width);
  const uint32_t exp = (uint32_t)(value.sign_exp & EXT80_EXP_MAX);
  const uint64_t sign = (uint64_t)(value.sign_exp >> 15) & 1u;
  /* ext80 keeps its leading one explicitly, so the target's implicit one is
     the bit already there and the fraction is what is below it. */
  const uint32_t drop = 63u - target.field;
  const uint64_t half = (uint64_t)1u << (drop - 1u);
  const uint64_t below = ((uint64_t)1u << drop) - 1u;
  uint64_t signif;
  int32_t biased;

  if (!out || target.field == 0u) {
    return 0;
  }
  /* A zero narrows to the zero of the same sign, exactly, in every rounding
     mode. It is the commonest value a game stores. */
  if (exp == 0u && value.signif == 0u) {
    *out = sign << (width * 8u - 1u);
    return 1;
  }
  /* A subnormal exponent, an all-ones one, and an unnormal -- a stored
     exponent with no explicit integer bit, which is an invalid encoding rather
     than a value -- are each somebody else's case. */
  if (exp == 0u || exp == EXT80_EXP_MAX || (value.signif & EXT80_INTEGER_BIT) == 0u) {
    return 0;
  }
  biased = (int32_t)exp - X86P_EXT80_BIAS + (int32_t)target.bias;
  /* Refused BEFORE rounding as well as after: a value one ulp below the
     smallest normal rounds up to it, and one at the top rounds to infinity,
     and both of those are the general conversion's to make. The bounds below
     are the normals only, and the carry test after rounding closes the top. */
  if (biased < 1 || biased > (int32_t)target.exp_max - 1) {
    return 0;
  }
  signif = value.signif;
  {
    /* Round to nearest, ties to even: add half an ulp, then clear the new
       least significant bit when the discarded part was EXACTLY half, which
       is the only case where "nearest" does not decide. */
    const uint64_t remainder = signif & below;
    const uint64_t rounded = signif + half;
    const int carried = rounded < signif;
    signif = rounded;
    if (remainder == half) {
      signif &= ~((uint64_t)1u << drop);
    }
    if (carried) {
      /* The significand became 2^64, which is 1.0 at one exponent higher and
         an empty fraction. That can leave the target's range, and when it
         does this is not the ordinary case after all. */
      biased++;
      signif = EXT80_INTEGER_BIT;
      if (biased > (int32_t)target.exp_max - 1) {
        return 0;
      }
    }
  }
  *out = (sign << (width * 8u - 1u)) | ((uint64_t)biased << target.field) |
         ((signif >> drop) & (((uint64_t)1u << target.field) - 1u));
  return 1;
}
