/* Adapter to Bochs software x87 math. Only values and status cross this seam;
 * decoding, guest stack lifetime, and JIT dispatch remain in x86port. */
#include "x87_softfloat.h"
#include "fpu/fpu_trans.h"
#include "x87.h"
#include "x87_f128_ext80.h"
#include "x87_transcendental.h"
#include <cfloat>
#include <cstring>

static_assert(sizeof(X86pX87Tag) == sizeof(unsigned int), "C ABI enum width");
static_assert(sizeof(X86pX87Op) == sizeof(unsigned int), "C ABI enum width");
static_assert(sizeof(X86pX87Insn) == sizeof(unsigned int), "C ABI enum width");
static_assert(sizeof(X86pX87Fn) == sizeof(unsigned int), "C ABI enum width");

static softfloat_status_t environment(uint16_t control) {
  softfloat_status_t status{};
  status.softfloat_exceptionMasks = 0x3F;
  status.softfloat_roundingMode = (control >> 10) & 3;
  switch (control & X86P_X87_PC_MASK) {
  case X86P_X87_PC_SINGLE:
    status.extF80_roundingPrecision = 32;
    break;
  case X86P_X87_PC_DOUBLE:
    status.extF80_roundingPrecision = 64;
    break;
  default:
    status.extF80_roundingPrecision = 80;
    break;
  }
  return status;
}
static floatx80 widen(long double value) {
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  static_assert(sizeof(long double) == sizeof(float128_t), "binary128 storage");
  /* The guest's own values round-trip through ext80, so the exact reassembly
   * covers them; anything else keeps the general conversion. */
  floatx80 exact{};
  if (x86p_x87_f128_to_ext80_exact(&value, &exact)) {
    return exact;
  }
  float128_t bits{};
  std::memcpy(&bits, &value, sizeof bits);
  auto status = environment(X86P_X87_CW_INIT);
  return f128_to_extF80(bits, &status);
#else
  uint8_t bytes[10];
  x86p_x87_to_f80(value, bytes);
  floatx80 result{};
  std::memcpy(&result.signif, bytes, 8);
  std::memcpy(&result.signExp, bytes + 8, 2);
  return result;
#endif
}
static long double narrow(floatx80 value) {
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  long double exact;
  if (x86p_x87_ext80_to_f128_exact(value, &exact)) {
    return exact;
  }
  auto status = environment(X86P_X87_CW_INIT);
  const float128_t bits = extF80_to_f128(value, &status);
  long double result;
  std::memcpy(&result, &bits, sizeof result);
  return result;
#else
  uint8_t bytes[10];
  std::memcpy(bytes, &value.signif, 8);
  std::memcpy(bytes + 8, &value.signExp, 2);
  return x86p_x87_from_f80(bytes);
#endif
}
extern "C" long double x86p_x87_software_decode(const uint8_t bytes[10]) {
  floatx80 value{};
  std::memcpy(&value.signif, bytes, 8);
  std::memcpy(&value.signExp, bytes + 8, 2);
  return narrow(value);
}
extern "C" void x86p_x87_software_encode(long double value, uint8_t bytes[10]) {
  const auto encoded = widen(value);
  std::memcpy(bytes, &encoded.signif, 8);
  std::memcpy(bytes + 8, &encoded.signExp, 2);
}
extern "C" long double
x86p_x87_software_arith(uint16_t control, X86pX87Op op, long double a, long double b, uint16_t *sw) {
  auto status = environment(control);
  const auto x = widen(a), y = widen(b);
  floatx80 result{};
  switch (op) {
  case kX86pX87Add:
    result = extF80_add(x, y, &status);
    break;
  case kX86pX87Sub:
    result = extF80_sub(x, y, &status);
    break;
  case kX86pX87Mul:
    result = extF80_mul(x, y, &status);
    break;
  case kX86pX87Div:
    result = extF80_div(x, y, &status);
    break;
  default:
    return 0;
  }
  if (sw) {
    *sw = static_cast<uint16_t>(status.softfloat_exceptionFlags);
  }
  return narrow(result);
}
extern "C" long double x86p_x87_software_constant(uint16_t control, long double value) {
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  float128_t bits{};
  std::memcpy(&bits, &value, sizeof bits);
  auto status = environment(control);
  return narrow(f128_to_extF80(bits, &status));
#else
  (void)control;
  return value;
#endif
}
extern "C" uint64_t x86p_x87_software_narrow(uint16_t control, long double value, int is64) {
  auto status = environment(control);
  const auto x = widen(value);
  return is64 ? extF80_to_f64(x, &status) : extF80_to_f32(x, &status);
}
extern "C" int x86p_x87_software_integer(uint16_t control, long double value, int width, int64_t *out) {
  if (!out || (width != 2 && width != 4 && width != 8)) {
    return 0;
  }
  auto status = environment(control);
  const auto x = widen(value);
  const int64_t result = width == 2   ? extF80_to_i16(x, &status)
                         : width == 4 ? extF80_to_i32(x, &status)
                                      : extF80_to_i64(x, &status);
  if (status.softfloat_exceptionFlags & softfloat_flag_invalid) {
    return 0;
  }
  *out = result;
  return 1;
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
  auto status = environment(control);
  /* Only FSQRT obeys PC in this instruction family. Transcendental
     reductions/results retain extended precision while still honoring RC. */
  if (fn != kX86pX87FnSqrt) {
    status.extF80_roundingPrecision = 80;
  }
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
    *sw = static_cast<uint16_t>(condition | (incomplete ? X86P_X87_C2 : 0) | status.softfloat_exceptionFlags);
  }
  return 1;
}

extern "C" int x86p_x87_fn_software(
    X86pX87Fn fn, long double a, long double b, long double *r0, long double *r1, int *pushed, uint16_t *sw) {
  return x86p_x87_fn_software_control(fn, 0x37F, a, b, r0, r1, pushed, sw);
}
