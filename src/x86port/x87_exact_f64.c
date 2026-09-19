/* See x87_exact_f64.h. */
#include "x87_exact_f64.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* binary64's fields, named once. */
enum { kF64SignifBits = 53, kF64StoredBits = 52, kF64Bias = 1023, kF64MinExp = -1022, kF64MaxExp = 1023 };

/* The ext80 exponents whose binary64 counterpart is a normal. */
enum { kExt80MinExp = X86P_EXT80_BIAS + kF64MinExp, kExt80MaxExp = X86P_EXT80_BIAS + kF64MaxExp };

static double f64_from_bits(uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof value);
  return value;
}

static uint64_t f64_to_bits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof bits);
  return bits;
}

/*
 * Trailing zeros of a nonzero 64-bit word, without a compiler builtin: this
 * file is C11 and builds under MSVC, clang and emcc alike.
 *
 * Halving rather than stepping, because the FIRST version of this stepped one
 * bit at a time and the values this module accepts are precisely the ones with
 * a long run of trailing zeros -- 1.0 has 52 of them. It made the fast path
 * slower than the softfloat it replaces: x86p_x87_arith_raw went from 16.80%
 * of the browser's guest worker to 28.83% while the softfloat frames left the
 * profile entirely, which is what "taking every operation and costing more"
 * looks like.
 */
static unsigned trailing_zeros(uint64_t v) {
  unsigned n = 0u;
  if ((v & 0xFFFFFFFFull) == 0u) {
    n += 32u;
    v >>= 32;
  }
  if ((v & 0xFFFFull) == 0u) {
    n += 16u;
    v >>= 16;
  }
  if ((v & 0xFFull) == 0u) {
    n += 8u;
    v >>= 8;
  }
  if ((v & 0xFull) == 0u) {
    n += 4u;
    v >>= 4;
  }
  if ((v & 0x3ull) == 0u) {
    n += 2u;
    v >>= 2;
  }
  if ((v & 0x1ull) == 0u) {
    n += 1u;
  }
  return n;
}

int x86p_x87_exact_f64_from_ext80(X86pExt80 v, double *out) {
  const unsigned exponent = (unsigned)(v.sign_exp & 0x7FFFu);
  const uint64_t sign = (uint64_t)(v.sign_exp >> 15) << 63;
  if (!out) {
    return 0;
  }
  if (exponent == 0u) {
    /* A zero converts exactly and keeps its sign; a pseudo-denormal does not
       and is not this. */
    if (v.signif != 0u) {
      return 0;
    }
    *out = f64_from_bits(sign);
    return 1;
  }
  if (exponent == 0x7FFFu) {
    return 0; /* infinity or NaN */
  }
  if ((v.signif >> 63) == 0u) {
    return 0; /* an unnormal: normal exponent without the integer bit */
  }
  if (exponent < (unsigned)kExt80MinExp || exponent > (unsigned)kExt80MaxExp) {
    return 0;
  }
  /* The 64-bit significand's low eleven bits are the ones binary64 has no
     room for. */
  if ((v.signif & 0x7FFu) != 0u) {
    return 0;
  }
  {
    const uint64_t biased = (uint64_t)(exponent - X86P_EXT80_BIAS + kF64Bias) << kF64StoredBits;
    const uint64_t stored = (v.signif >> 11) & 0x000FFFFFFFFFFFFFull;
    *out = f64_from_bits(sign | biased | stored);
  }
  return 1;
}

/* How many significant bits a nonzero finite binary64 carries. */
static unsigned f64_signif_bits(double value) {
  const uint64_t stored = f64_to_bits(value) & 0x000FFFFFFFFFFFFFull;
  const uint64_t significand = stored | (1ull << kF64StoredBits);
  return (unsigned)kF64SignifBits - trailing_zeros(significand);
}

/* A result this module may return: finite, normal, and not a zero. */
static int result_is_ordinary(double r) {
  if (!(r == r) || r > DBL_MAX || r < -DBL_MAX) {
    return 0;
  }
  return fabs(r) >= DBL_MIN;
}

/*
 * Knuth's two-sum. `s` is the rounded sum and the value returned is the error
 * it dropped, EXACTLY, for any two finite operands whose sum does not
 * overflow. Zero error is therefore a proof of exactness rather than evidence
 * of it.
 */
static double two_sum_error(double x, double y, double s) {
  const double bv = s - x;
  return (x - (s - bv)) + (y - bv);
}

int x86p_x87_exact_f64_arith(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *out) {
  double a;
  double b;
  double r;
  if (!out) {
    return 0;
  }
  /* The guest must be asking for the precision this answers. */
  if ((control & X86P_X87_PC_MASK) != X86P_X87_PC_EXTENDED) {
    return 0;
  }
  if (!x86p_x87_exact_f64_from_ext80(x, &a) || !x86p_x87_exact_f64_from_ext80(y, &b)) {
    return 0;
  }
  switch (op) {
  case kX86pX87Add:
    r = a + b;
    if (!result_is_ordinary(r) || two_sum_error(a, b, r) != 0.0) {
      return 0;
    }
    break;
  case kX86pX87Sub:
    r = a - b;
    if (!result_is_ordinary(r) || two_sum_error(a, -b, r) != 0.0) {
      return 0;
    }
    break;
  case kX86pX87Mul:
    /* Exact when the two significands' product fits in binary64's. Both
       operands are normals or zeros here, and a zero product is refused as a
       zero result below. */
    if (a == 0.0 || b == 0.0) {
      return 0;
    }
    if (f64_signif_bits(a) + f64_signif_bits(b) > (unsigned)kF64SignifBits) {
      return 0;
    }
    r = a * b;
    if (!result_is_ordinary(r)) {
      return 0;
    }
    break;
  case kX86pX87Div:
    if (a == 0.0 || b == 0.0) {
      return 0;
    }
    r = a / b;
    if (!result_is_ordinary(r)) {
      return 0;
    }
    /* r is exact iff r * b is exactly a, and r * b is itself exact iff the
       significands fit -- so both tests are needed and neither implies the
       other. */
    if (f64_signif_bits(r) + f64_signif_bits(b) > (unsigned)kF64SignifBits) {
      return 0;
    }
    if (r * b != a) {
      return 0;
    }
    break;
  case kX86pX87OpCount:
  default:
    return 0;
  }
  *out = x86p_ext80_from_f64_bits(f64_to_bits(r));
  return 1;
}
