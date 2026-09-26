/* FILD and FIST's conversions: see x87_ext80_integer.h. */
#include "x87_ext80_integer.h"

#include "x87.h"

#include <stdint.h>
#include <string.h>

#define EXT80_EXP_MASK 0x7FFFu
#define EXT80_SIGN_BIT 0x8000u
#define EXT80_INTEGER_BIT (UINT64_C(1) << 63)

X86pExt80 x86p_ext80_from_int(int64_t value) {
  X86pExt80 out = {0u, 0u};
  uint64_t magnitude;
  unsigned shift = 0u;
  if (value == 0) {
    return out;
  }
  /* The magnitude as unsigned, so INT64_MIN is 2^63 rather than an overflow. */
  magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
  while (!(magnitude & EXT80_INTEGER_BIT)) {
    magnitude <<= 1;
    shift++;
  }
  out.signif = magnitude;
  out.sign_exp = (uint16_t)((X86P_EXT80_BIAS + 63 - (int)shift) | (value < 0 ? EXT80_SIGN_BIT : 0));
  return out;
}

/* Whether the discarded fraction rounds the magnitude up, by RC. `below` is
   the fraction's bits, `half` the value of one half in the same units, and a
   magnitude that is exactly a tie rounds to the even integer. */
static int rounds_up(uint16_t control, int negative, uint64_t integer, uint64_t below, uint64_t half) {
  if (below == 0u) {
    return 0;
  }
  switch (control & X86P_X87_RC_MASK) {
  case X86P_X87_RC_DOWN:
    return negative;
  case X86P_X87_RC_UP:
    return !negative;
  case X86P_X87_RC_TRUNCATE:
    return 0;
  default:
    return below > half || (below == half && (integer & 1u));
  }
}

int x86p_ext80_to_int(uint16_t control, X86pExt80 value, int width, int64_t *out) {
  const int negative = (value.sign_exp & EXT80_SIGN_BIT) != 0;
  const int exponent = (int)(value.sign_exp & EXT80_EXP_MASK);
  const unsigned bits = (unsigned)width * 8u;
  uint64_t magnitude;
  uint64_t limit;
  if (!out || (width != 2 && width != 4 && width != 8)) {
    return 0;
  }
  if (exponent == (int)EXT80_EXP_MASK) {
    return 0; /* an infinity or a NaN */
  }
  if (exponent != 0 && !(value.signif & EXT80_INTEGER_BIT)) {
    return 0; /* an unnormal, which the 387 and later refuse as an operand */
  }
  if (value.signif == 0u) {
    magnitude = 0u;
  } else if (exponent - X86P_EXT80_BIAS < 0) {
    /* Below one: a denormal, or 0.5 <= |v| < 1 when the unbiased exponent is
       -1, whose tie is exactly the integer bit alone. */
    const int unbiased = exponent - X86P_EXT80_BIAS;
    const uint64_t below = unbiased == -1 ? value.signif : 1u;
    const uint64_t half = unbiased == -1 ? EXT80_INTEGER_BIT : UINT64_MAX;
    magnitude = (uint64_t)rounds_up(control, negative, 0u, below, half);
  } else if (exponent - X86P_EXT80_BIAS >= 63) {
    /* 2^63 or more: only -2^63 itself fits, and only in eight bytes. */
    if (!(negative && width == 8 && exponent - X86P_EXT80_BIAS == 63 && value.signif == EXT80_INTEGER_BIT)) {
      return 0;
    }
    *out = INT64_MIN;
    return 1;
  } else {
    const unsigned shift = 63u - (unsigned)(exponent - X86P_EXT80_BIAS);
    const uint64_t integer = value.signif >> shift;
    const uint64_t below = shift ? value.signif & ((UINT64_C(1) << shift) - 1u) : 0u;
    const uint64_t half = shift ? UINT64_C(1) << (shift - 1u) : 0u;
    magnitude = integer + (uint64_t)rounds_up(control, negative, integer, below, half);
  }
  limit = (UINT64_C(1) << (bits - 1u)) - (negative ? 0u : 1u);
  if (magnitude > limit) {
    return 0;
  }
  *out = negative ? (int64_t)((uint64_t)0 - magnitude) : (int64_t)magnitude;
  return 1;
}

static X86pExt80 ext80_of_reg(X86pX87Reg value) {
  uint8_t bytes[10];
  X86pExt80 out;
  x86p_x87_reg_to_f80(value, bytes);
  memcpy(&out.signif, bytes, 8);
  out.sign_exp = (uint16_t)((uint16_t)bytes[8] | ((uint16_t)bytes[9] << 8));
  return out;
}

static X86pX87Reg reg_of_ext80(X86pExt80 value) {
  uint8_t bytes[10];
  memcpy(bytes, &value.signif, 8);
  bytes[8] = (uint8_t)(value.sign_exp & 0xFFu);
  bytes[9] = (uint8_t)(value.sign_exp >> 8);
  return x86p_x87_reg_from_f80(bytes);
}

static int64_t integer_of_bits(uint64_t bits, unsigned width) {
  if (width == 2) {
    return (int16_t)bits;
  }
  if (width == 4) {
    return (int32_t)bits;
  }
  return (int64_t)bits;
}

X86pX87Reg x86p_x87_reg_from_integer_bits(uint64_t bits, unsigned width) {
  return reg_of_ext80(x86p_ext80_from_int(integer_of_bits(bits, width)));
}

long double x86p_x87_integer_value(uint64_t bits, unsigned width) {
  return x86p_x87_reg_to_long_double(x86p_x87_reg_from_integer_bits(bits, width));
}

int x86p_x87_reg_to_int(uint16_t control, X86pX87Reg value, int width_bytes, int64_t *out) {
  return x86p_ext80_to_int(control, ext80_of_reg(value), width_bytes, out);
}

int x86p_x87_to_int(const X86pX87 *f, long double v, int width_bytes, int64_t *out) {
  return f && x86p_x87_reg_to_int(f->control, x86p_x87_reg_from_long_double(v), width_bytes, out);
}

uint64_t x86p_x87_reg_to_integer_bits(X86pX87 *f, X86pX87Reg value, int width) {
  int64_t result;
  if (!x86p_x87_reg_to_int(f->control, value, width, &result)) {
    f->status |= X86P_X87_IE;
    result = width == 2 ? INT16_MIN : width == 4 ? INT32_MIN : INT64_MIN;
  }
  return (uint64_t)result;
}

uint64_t x86p_x87_to_i16(X86pX87 *f, long double value) {
  return x86p_x87_reg_to_integer_bits(f, x86p_x87_reg_from_long_double(value), 2);
}
uint64_t x86p_x87_to_i32(X86pX87 *f, long double value) {
  return x86p_x87_reg_to_integer_bits(f, x86p_x87_reg_from_long_double(value), 4);
}
uint64_t x86p_x87_to_i64(X86pX87 *f, long double value) {
  return x86p_x87_reg_to_integer_bits(f, x86p_x87_reg_from_long_double(value), 8);
}
