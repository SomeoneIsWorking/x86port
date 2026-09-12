/* See x87_binary128.h. */
#include "x87_binary128.h"

#include <string.h>

#define F128_EXP_MASK 0x7FFFu
#define F128_SIGN_BIT 0x8000000000000000ull
#define F128_HI_MANT_MASK 0x0000FFFFFFFFFFFFull

static uint32_t f128_exponent(X86pF128 v) { return (uint32_t)((v.hi >> 48) & F128_EXP_MASK); }

static int f128_mantissa_is_zero(X86pF128 v) { return v.lo == 0u && (v.hi & F128_HI_MANT_MASK) == 0u; }

int x86p_f128_is_zero(X86pF128 v) { return f128_exponent(v) == 0u && f128_mantissa_is_zero(v); }

int x86p_f128_is_nan(X86pF128 v) { return f128_exponent(v) == F128_EXP_MASK && !f128_mantissa_is_zero(v); }

int x86p_f128_is_inf(X86pF128 v) { return f128_exponent(v) == F128_EXP_MASK && f128_mantissa_is_zero(v); }

/* Assemble from a sign, an UNBIASED exponent and a mantissa already aligned to
   binary128's 112-bit field. */
static X86pF128 f128_make(uint64_t sign, uint32_t biased_exp, uint64_t mant_hi, uint64_t mant_lo) {
  X86pF128 out;
  out.lo = mant_lo;
  out.hi = sign | ((uint64_t)(biased_exp & F128_EXP_MASK) << 48) | (mant_hi & F128_HI_MANT_MASK);
  return out;
}

/*
 * Widen a narrower IEEE format. The stored mantissa moves left by the width
 * difference and the exponent is rebiased; both widths and both biases are
 * exact in binary128, so nothing rounds. A subnormal has no implicit leading
 * one, so it is normalised first -- binary128's exponent range is wide enough
 * that every float and double subnormal becomes a normal number.
 *
 * `precision` counts the implicit bit (24 for float, 53 for double), so the
 * stored field is `precision - 1` bits wide.
 */
static X86pF128 widen(uint64_t sign, uint32_t exp, uint64_t mant, uint32_t precision, uint32_t exp_max,
                      uint32_t bias) {
  const uint32_t field = precision - 1u;
  const uint32_t shift = 112u - field;
  const uint64_t implicit = (uint64_t)1u << field;
  int32_t biased;
  uint64_t hi, lo;

  if (exp == 0u && mant == 0u) {
    return f128_make(sign, 0u, 0u, 0u); /* signed zero */
  }
  if (exp == exp_max) {
    /* Infinity keeps its empty mantissa; a NaN keeps its payload in the same
       relative position, so the quiet bit stays the quiet bit. A SIGNALLING
       NaN is quieted, which is what the C conversion this replaces does and
       what the guest sees today. */
    biased = (int32_t)F128_EXP_MASK;
    if (mant != 0u) {
      mant |= implicit >> 1;
    }
  } else if (exp == 0u) {
    uint32_t norm = 0u;
    while ((mant & implicit) == 0u) {
      mant <<= 1;
      norm++;
    }
    mant &= implicit - 1u;
    biased = 16383 + 1 - (int32_t)bias - (int32_t)norm;
  } else {
    biased = (int32_t)exp - (int32_t)bias + 16383;
  }

  if (shift >= 64u) {
    hi = mant << (shift - 64u);
    lo = 0u;
  } else {
    hi = mant >> (64u - shift);
    lo = mant << shift;
  }
  return f128_make(sign, (uint32_t)biased, hi, lo);
}

X86pF128 x86p_f128_from_f32(uint32_t bits) {
  return widen((uint64_t)(bits >> 31) << 63, (bits >> 23) & 0xFFu, bits & 0x7FFFFFu, 24u, 0xFFu, 127u);
}

X86pF128 x86p_f128_from_f64(uint64_t bits) {
  return widen(bits & F128_SIGN_BIT, (uint32_t)((bits >> 52) & 0x7FFu), bits & 0xFFFFFFFFFFFFFull, 53u, 0x7FFu,
               1023u);
}

int x86p_f128_compare(X86pF128 a, X86pF128 b) {
  const int a_zero = x86p_f128_is_zero(a), b_zero = x86p_f128_is_zero(b);
  const int a_neg = (a.hi & F128_SIGN_BIT) != 0u, b_neg = (b.hi & F128_SIGN_BIT) != 0u;

  if (a_zero && b_zero) {
    return 0; /* -0.0 == +0.0 */
  }
  if (a_neg != b_neg) {
    /* A zero of either sign is still on the side its sign says, once the
       both-zero case above is out of the way. */
    return a_neg ? -1 : 1;
  }
  if (a.hi != b.hi) {
    return (a.hi > b.hi) != (a_neg != 0) ? 1 : -1;
  }
  if (a.lo == b.lo) {
    return 0;
  }
  return (a.lo > b.lo) != (a_neg != 0) ? 1 : -1;
}

#ifdef X86P_X87_BINARY128
X86pF128 x86p_f128_of(long double v) {
  X86pF128 out;
  memcpy(&out, &v, sizeof out);
  return out;
}

long double x86p_f128_value(X86pF128 v) {
  long double out;
  memcpy(&out, &v, sizeof out);
  return out;
}
#endif
