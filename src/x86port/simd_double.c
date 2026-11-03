/*
 * simd_double.c -- SSE2 double precision, and the conversions between the two
 * precisions.
 *
 * IEEE-754 binary64 with round-to-nearest-even is the host's `double`, so the
 * lanes are host arithmetic, with the SSE NaN rule applied through
 * x86p_sse_nan_result_f64. The MXCSR limits stated in simd_float.c apply here
 * unchanged: flush-to-zero, denormals-are-zero and the rounding-control field
 * are not modelled.
 *
 * A scalar (SD) form writes the low double and keeps the destination's high
 * one; a narrowing packed conversion (CVTPD2PS, CVTPD2DQ) zeroes the upper
 * half of the destination.
 */
#include "simd_internal.h"
#include "simd_packed.h"

#include <math.h>

static double min_sd(double a, double b) {
  /* The second operand on equality or NaN, as MINPS. */
  return (a < b) ? a : b;
}

static double max_sd(double a, double b) {
  return (a > b) ? a : b;
}

static double arithmetic(X86pSimdOp op, double x, double y) {
  switch (op) {
  case kX86pSimdAddpd:
  case kX86pSimdAddsd:
    return x86p_sse_nan_result_f64(x, y, x + y);
  case kX86pSimdSubpd:
  case kX86pSimdSubsd:
    return x86p_sse_nan_result_f64(x, y, x - y);
  case kX86pSimdMulpd:
  case kX86pSimdMulsd:
    return x86p_sse_nan_result_f64(x, y, x * y);
  case kX86pSimdDivpd:
  case kX86pSimdDivsd:
    return x86p_sse_nan_result_f64(x, y, x / y);
  case kX86pSimdMinpd:
  case kX86pSimdMinsd:
    return min_sd(x, y);
  case kX86pSimdMaxpd:
  case kX86pSimdMaxsd:
    return max_sd(x, y);
  default:
    return x86p_sse_nan_result_f64(y, y, sqrt(y));
  }
}

static uint32_t to_integer(double x, int truncate) {
  return x86p_sse_integer(truncate ? trunc(x) : nearbyint(x));
}

int x86p_simd_double(X86pSimdOp op, const X86pVec *a, const X86pVec *b, uint8_t imm, X86pVec *out, X86pFlags *flags) {
  X86pVec r;
  unsigned i;

  if (!a || !b || !out) {
    return 0;
  }
  r = *a;

  switch (op) {
  case kX86pSimdAddpd:
  case kX86pSimdSubpd:
  case kX86pSimdMulpd:
  case kX86pSimdDivpd:
  case kX86pSimdMinpd:
  case kX86pSimdMaxpd:
  case kX86pSimdSqrtpd:
    for (i = 0; i < 2u; i++) {
      vec_set_f64(&r, i, arithmetic(op, vec_f64(a, i), vec_f64(b, i)));
    }
    break;
  case kX86pSimdAddsd:
  case kX86pSimdSubsd:
  case kX86pSimdMulsd:
  case kX86pSimdDivsd:
  case kX86pSimdMinsd:
  case kX86pSimdMaxsd:
  case kX86pSimdSqrtsd:
    vec_set_f64(&r, 0, arithmetic(op, vec_f64(a, 0), vec_f64(b, 0)));
    break;

  case kX86pSimdCmppd:
    for (i = 0; i < 2u; i++) {
      vec_set_u64(&r, i, x86p_sse_compare(imm, vec_f64(a, i), vec_f64(b, i)) ? UINT64_MAX : 0u);
    }
    break;
  case kX86pSimdCmpsd:
    vec_set_u64(&r, 0, x86p_sse_compare(imm, vec_f64(a, 0), vec_f64(b, 0)) ? UINT64_MAX : 0u);
    break;
  case kX86pSimdComisd:
  case kX86pSimdUcomisd:
    /* They differ only in which NaN raises a masked exception. */
    x86p_sse_compare_flags(vec_f64(a, 0), vec_f64(b, 0), flags);
    *out = *a;
    return 1;

  case kX86pSimdShufpd:
    vec_set_u64(&r, 0, vec_u64(a, imm & 1u));
    vec_set_u64(&r, 1, vec_u64(b, (imm >> 1) & 1u));
    break;

  case kX86pSimdCvtps2pd:
    r.bytes = 16u;
    for (i = 0; i < 2u; i++) {
      vec_set_f64(&r, i, (double)vec_f32(b, i));
    }
    break;
  case kX86pSimdCvtpd2ps:
    memset(&r, 0, sizeof r);
    r.bytes = 16u;
    for (i = 0; i < 2u; i++) {
      vec_set_f32(&r, i, (float)vec_f64(b, i));
    }
    break;
  case kX86pSimdCvtss2sd:
    vec_set_f64(&r, 0, (double)vec_f32(b, 0));
    break;
  case kX86pSimdCvtsd2ss:
    vec_set_f32(&r, 0, (float)vec_f64(b, 0));
    break;
  case kX86pSimdCvtdq2pd:
  case kX86pSimdCvtpi2pd:
    r.bytes = 16u;
    for (i = 0; i < 2u; i++) {
      vec_set_f64(&r, i, (double)(int32_t)vec_u32(b, i));
    }
    break;
  case kX86pSimdCvtpd2dq:
  case kX86pSimdCvttpd2dq:
    memset(&r, 0, sizeof r);
    r.bytes = 16u;
    for (i = 0; i < 2u; i++) {
      vec_set_u32(&r, i, to_integer(vec_f64(b, i), op == kX86pSimdCvttpd2dq));
    }
    break;
  case kX86pSimdCvtpd2pi:
  case kX86pSimdCvttpd2pi:
    memset(&r, 0, sizeof r);
    r.bytes = 8u;
    for (i = 0; i < 2u; i++) {
      vec_set_u32(&r, i, to_integer(vec_f64(b, i), op == kX86pSimdCvttpd2pi));
    }
    break;
  case kX86pSimdCvtsi2sd:
    vec_set_f64(&r, 0, (double)(int32_t)vec_u32(b, 0));
    break;
  case kX86pSimdCvtsd2si:
  case kX86pSimdCvttsd2si:
    memset(&r, 0, sizeof r);
    r.bytes = 4u;
    vec_set_u32(&r, 0, to_integer(vec_f64(b, 0), op == kX86pSimdCvttsd2si));
    break;

  default:
    return 0;
  }

  *out = r;
  return 1;
}
