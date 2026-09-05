/* Adapter to Bochs software x87 math. Only values and status cross this seam;
 * decoding, guest stack lifetime, and JIT dispatch remain in x86port. */
#include "fpu/fpu_trans.h"
#include "x87.h"
#include "x87_transcendental.h"
#include <cstring>

static floatx80 widen(long double value) {
  uint8_t bytes[10];
  x86p_x87_to_f80(value, bytes);
  floatx80 result{};
  std::memcpy(&result.signif, bytes, 8);
  std::memcpy(&result.signExp, bytes + 8, 2);
  return result;
}
static long double narrow(floatx80 value) {
  uint8_t bytes[10];
  std::memcpy(bytes, &value.signif, 8);
  std::memcpy(bytes + 8, &value.signExp, 2);
  return x86p_x87_from_f80(bytes);
}
extern "C" int x86p_x87_fn_software_control(X86pX87Fn fn,
                                            uint16_t control,
                                            long double a,
                                            long double b,
                                            long double *r0,
                                            long double *r1,
                                            int *pushed,
                                            uint16_t *sw) {
  if (!r0 || !pushed) {
    return 0;
  }
  softfloat_status_t status{};
  status.softfloat_exceptionMasks = 0x3F;
  status.extF80_roundingPrecision = 80;
  status.softfloat_roundingMode = (control >> 10) & 3;
  floatx80 x = widen(a), y = widen(b), second{};
  int more = 0, incomplete = 0;
  Bit64u quotient = 0;
  uint16_t condition = 0;
  switch (fn) {
  case kX86pX87FnSqrt:
    x = extF80_sqrt(x, &status);
    break;
  case kX86pX87FnSin:
    incomplete = fsin(x, status);
    break;
  case kX86pX87FnCos:
    incomplete = fcos(x, status);
    break;
  case kX86pX87FnSinCos:
    incomplete = fsincos(x, &x, &second, status);
    more = incomplete != -1;
    break;
  case kX86pX87FnPtan:
    incomplete = ftan(x, status);
    second = i32_to_extF80(1);
    more = incomplete != -1;
    break;
  case kX86pX87FnPatan:
    x = fpatan(x, y, status);
    break;
  case kX86pX87FnYl2x:
    x = fyl2x(x, y, status);
    break;
  case kX86pX87FnYl2xp1:
    x = fyl2xp1(x, y, status);
    break;
  case kX86pX87Fn2xm1:
    x = f2xm1(x, status);
    break;
  case kX86pX87FnScale:
    x = extF80_scale(x, y, &status);
    break;
  case kX86pX87FnRndint:
    x = extF80_roundToInt(x, &status);
    break;
  case kX86pX87FnAbs:
    x.signExp &= 0x7FFF;
    break;
  case kX86pX87FnChs:
    x.signExp ^= 0x8000;
    break;
  case kX86pX87FnPrem:
  case kX86pX87FnPrem1:
    incomplete = fn == kX86pX87FnPrem ? floatx80_remainder(x, y, x, quotient, &status)
                                      : floatx80_ieee754_remainder(x, y, x, quotient, &status);
    condition = static_cast<uint16_t>(((quotient & 4) ? 0x100 : 0) | ((quotient & 2) ? 0x4000 : 0) |
                                      ((quotient & 1) ? 0x200 : 0));
    break;
  default:
    return 0;
  }
  *r0 = narrow(x);
  if (r1) {
    *r1 = more ? narrow(second) : 0;
  }
  *pushed = more;
  if (sw) {
    *sw = static_cast<uint16_t>(condition | (incomplete ? 0x400 : 0));
  }
  return 1;
}

extern "C" int x86p_x87_fn_software(
    X86pX87Fn fn, long double a, long double b, long double *r0, long double *r1, int *pushed, uint16_t *sw) {
  return x86p_x87_fn_software_control(fn, 0x37F, a, b, r0, r1, pushed, sw);
}
