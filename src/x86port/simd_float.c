/*
 * simd_float.c -- SSE single precision.
 *
 * IEEE-754 binary32 with round-to-nearest-even, which is exactly what the host
 * does with `float`, so these are not approximations of the guest's arithmetic
 * -- they are the same arithmetic. That is the whole reason the packed
 * operations can be a lane loop over host floats rather than a software
 * float library.
 *
 * WHERE THAT STOPS BEING TRUE, AND WHAT IS DONE ABOUT IT:
 *
 *   - MXCSR's flush-to-zero and denormals-are-zero bits change the result for
 *     denormal inputs. This framework does not model them; every process it
 *     targets runs at the default, where both are clear, and the host default
 *     matches. Stated here rather than assumed silently.
 *   - MXCSR's rounding-control field is likewise not honoured. The same
 *     applies, and CVTTSS2SI -- which truncates regardless of it -- is the one
 *     conversion that is right either way.
 *   - RCPPS, RCPSS, RSQRTPS and RSQRTSS are NOT here. They produce results
 *     from a hardware table to about twelve mantissa bits, and `1.0f / x`
 *     agrees to six digits and differs below that. They are refused by name in
 *     simd.c, which is the honest answer: a wrong reciprocal surfaces as
 *     geometry drift a thousand frames later, and the test that would catch it
 *     is the one nobody writes.
 *
 * MIN AND MAX ARE NOT COMMUTATIVE, and that is architectural rather than an
 * implementation detail: MINPS returns the SECOND operand when the two are
 * equal or when either is a NaN. Writing `a < b ? a : b` is right for ordinary
 * values and wrong for every NaN, which is precisely the case a shader-adjacent
 * math library produces.
 */
#include "simd_internal.h"
#include "simd_packed.h"

#include "flags.h"

#include <math.h>

static float min_ss(float a, float b) {
  /* Second operand on equality or NaN -- see the header comment. */
  return (a < b) ? a : b;
}

static float max_ss(float a, float b) {
  return (a > b) ? a : b;
}

int x86p_simd_float(X86pSimdOp op, const X86pVec *a, const X86pVec *b, uint8_t imm, X86pVec *out, X86pFlags *flags) {
  X86pVec r;
  unsigned n;
  unsigned i;

  if (!a || !b || !out) {
    return 0;
  }
  r = *a; /* scalar forms preserve the destination's upper lanes */
  r.bytes = a->bytes;
  n = a->bytes / 4u;

  switch (op) {
  case kX86pSimdAddps:
    x86p_simd_addps(r.b, b->b);
    break;
  case kX86pSimdSubps:
    x86p_simd_subps(r.b, b->b);
    break;
  case kX86pSimdMulps:
    x86p_simd_mulps(r.b, b->b);
    break;
  case kX86pSimdDivps:
    x86p_simd_divps(r.b, b->b);
    break;
  case kX86pSimdMinps:
  case kX86pSimdMaxps:
    for (i = 0; i < n; i++) {
      float x = vec_f32(a, i);
      float y = vec_f32(b, i);
      float v;
      switch (op) {
      case kX86pSimdMinps:
        v = min_ss(x, y);
        break;
      default:
        v = max_ss(x, y);
        break;
      }
      vec_set_f32(&r, i, v);
    }
    break;

  case kX86pSimdSqrtps:
    for (i = 0; i < n; i++) {
      vec_set_f32(&r, i, x86p_sse_nan_result(vec_f32(b, i), vec_f32(b, i), sqrtf(vec_f32(b, i))));
    }
    break;

  /* ---- scalar: lane 0 only, the rest of the destination untouched ---- */
  case kX86pSimdAddss:
  case kX86pSimdSubss:
  case kX86pSimdMulss:
  case kX86pSimdDivss:
  case kX86pSimdMinss:
  case kX86pSimdMaxss:
  case kX86pSimdSqrtss: {
    float x = vec_f32(a, 0);
    float y = vec_f32(b, 0);
    float v;
    switch (op) {
    case kX86pSimdAddss:
      v = x86p_sse_nan_result(x, y, x + y);
      break;
    case kX86pSimdSubss:
      v = x86p_sse_nan_result(x, y, x - y);
      break;
    case kX86pSimdMulss:
      v = x86p_sse_nan_result(x, y, x * y);
      break;
    case kX86pSimdDivss:
      v = x86p_sse_nan_result(x, y, x / y);
      break;
    case kX86pSimdMinss:
      v = min_ss(x, y);
      break;
    case kX86pSimdMaxss:
      v = max_ss(x, y);
      break;
    default:
      v = x86p_sse_nan_result(y, y, sqrtf(y));
      break;
    }
    vec_set_f32(&r, 0, v);
    break;
  }

  /* ---- bitwise, on float registers. Not the same instructions as PAND and
     friends even though the result is identical: they differ in which
     execution unit they use and, on some parts, in latency. Semantically one
     loop. ---- */
  case kX86pSimdAndps:
  case kX86pSimdAndnps:
  case kX86pSimdOrps:
  case kX86pSimdXorps:
    for (i = 0; i < a->bytes; i++) {
      uint8_t x = a->b[i];
      uint8_t y = b->b[i];
      r.b[i] = (op == kX86pSimdAndps)    ? (uint8_t)(x & y)
               : (op == kX86pSimdAndnps) ? (uint8_t)((uint8_t)~x & y)
               : (op == kX86pSimdOrps)   ? (uint8_t)(x | y)
                                         : (uint8_t)(x ^ y);
    }
    break;

  case kX86pSimdCmpps:
    for (i = 0; i < n; i++) {
      vec_set_u32(&r, i, x86p_sse_compare(imm, vec_f32(a, i), vec_f32(b, i)) ? 0xFFFFFFFFu : 0u);
    }
    break;
  case kX86pSimdCmpss:
    vec_set_u32(&r, 0, x86p_sse_compare(imm, vec_f32(a, 0), vec_f32(b, 0)) ? 0xFFFFFFFFu : 0u);
    break;

  case kX86pSimdComiss:
  case kX86pSimdUcomiss:
    /* The two differ only in which NaN raises an invalid-operation exception,
       and exceptions are masked in every process this targets, so the
       observable result is the same. Kept as two names because a build that
       models the exception must be able to tell them apart. */
    x86p_sse_compare_flags(vec_f32(a, 0), vec_f32(b, 0), flags);
    *out = *a; /* neither writes a register */
    return 1;

  case kX86pSimdShufps:
    /* The low two lanes come from the DESTINATION and the high two from the
       source. Taking all four from one operand is the classic way to write a
       shuffle that works for the identity immediate and nothing else. */
    for (i = 0; i < 2u; i++) {
      vec_set_f32(&r, i, vec_f32(a, (unsigned)((imm >> (i * 2u)) & 3u)));
    }
    for (i = 2u; i < 4u; i++) {
      vec_set_f32(&r, i, vec_f32(b, (unsigned)((imm >> (i * 2u)) & 3u)));
    }
    break;

  case kX86pSimdUnpcklps:
    vec_set_u32(&r, 0, vec_u32(a, 0));
    vec_set_u32(&r, 1, vec_u32(b, 0));
    vec_set_u32(&r, 2, vec_u32(a, 1));
    vec_set_u32(&r, 3, vec_u32(b, 1));
    break;
  case kX86pSimdUnpckhps:
    vec_set_u32(&r, 0, vec_u32(a, 2));
    vec_set_u32(&r, 1, vec_u32(b, 2));
    vec_set_u32(&r, 2, vec_u32(a, 3));
    vec_set_u32(&r, 3, vec_u32(b, 3));
    break;

  /* ---- conversions ---- */
  case kX86pSimdCvtpi2ps:
    /* Two dwords in, two floats into the LOW half; the high half is left. */
    vec_set_f32(&r, 0, (float)(int32_t)vec_u32(b, 0));
    vec_set_f32(&r, 1, (float)(int32_t)vec_u32(b, 1));
    break;
  case kX86pSimdCvtps2pi:
  case kX86pSimdCvttps2pi:
    r.bytes = 8u;
    for (i = 0; i < 2u; i++) {
      float x = vec_f32(b, i);
      /* Rounding for CVT, truncation for CVTT. Nearest-even is the default
         mode and the only one this build models; the truncating form is right
         regardless of the mode, which is why it is the one compilers emit. */
      double d = (op == kX86pSimdCvttps2pi) ? (double)truncf(x) : (double)nearbyintf(x);
      vec_set_u32(&r, i, x86p_sse_integer(d));
    }
    break;
  case kX86pSimdCvtsi2ss:
    vec_set_f32(&r, 0, (float)(int32_t)vec_u32(b, 0));
    break;
  case kX86pSimdCvtss2si:
  case kX86pSimdCvttss2si: {
    float x = vec_f32(b, 0);
    double d = (op == kX86pSimdCvttss2si) ? (double)truncf(x) : (double)nearbyintf(x);
    memset(&r, 0, sizeof r);
    r.bytes = 4u;
    vec_set_u32(&r, 0, x86p_sse_integer(d));
    break;
  }
  case kX86pSimdCvtdq2ps:
    for (i = 0; i < 4u; i++) {
      vec_set_f32(&r, i, (float)(int32_t)vec_u32(b, i));
    }
    break;
  case kX86pSimdCvtps2dq:
  case kX86pSimdCvttps2dq:
    for (i = 0; i < 4u; i++) {
      float x = vec_f32(b, i);
      double d = (op == kX86pSimdCvttps2dq) ? (double)truncf(x) : (double)nearbyintf(x);
      vec_set_u32(&r, i, x86p_sse_integer(d));
    }
    break;

  default:
    return 0;
  }

  *out = r;
  return 1;
}
