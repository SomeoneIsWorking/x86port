/* See x87_double_arith.h. */
#include "x87_double_arith.h"

#include "x87_ext80_arith.h"
#include "x87_ext80_narrow.h"

#include <string.h>

enum { kF64ExpMax = 0x7FF, kF64FieldBits = 52 };

int x86p_x87_double_control_applies(uint16_t control) {
  return (control & X86P_X87_RC_MASK) == X86P_X87_RC_NEAREST && (control & X86P_X87_PC_MASK) != X86P_X87_PC_SINGLE;
}

int x86p_ext80_to_double(X86pExt80 v, double *out) {
  uint64_t bits;
  if (x86p_ext80_is_zero(v)) {
    bits = (uint64_t)(v.sign_exp >> 15) << 63;
  } else if (!x86p_ext80_narrow_nearest(v, 8u, &bits)) {
    return 0;
  }
  memcpy(out, &bits, sizeof bits);
  return 1;
}

int x86p_ext80_of_double(double v, X86pExt80 *out) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  const unsigned exponent = (unsigned)(bits >> kF64FieldBits) & (unsigned)kF64ExpMax;
  if (exponent == (unsigned)kF64ExpMax || (exponent == 0u && (bits << 1) != 0u)) {
    return 0;
  }
  if (exponent == 0u) {
    out->signif = 0u;
    out->sign_exp = (uint16_t)((bits >> 63) << 15);
    return 1;
  }
  *out = x86p_ext80_from_f64_bits(bits);
  return 1;
}

int x86p_ext80_double_arith(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *out) {
  double a;
  double b;
  double r;
  if (!x86p_x87_double_control_applies(control) || !x86p_ext80_to_double(x, &a) || !x86p_ext80_to_double(y, &b)) {
    return 0;
  }
  switch (op) {
  case kX86pX87Add:
    r = a + b;
    break;
  case kX86pX87Sub:
    r = a - b;
    break;
  case kX86pX87Mul:
    r = a * b;
    break;
  case kX86pX87Div:
    r = a / b;
    break;
  case kX86pX87OpCount:
  default:
    return 0;
  }
  return x86p_ext80_of_double(r, out);
}
