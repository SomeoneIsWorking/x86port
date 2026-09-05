#include "x87.h"
#include <stdint.h>

long double x86p_x87_integer_value(uint64_t bits, unsigned width) {
  if (width == 2) {
    return (long double)(int16_t)bits;
  }
  if (width == 4) {
    return (long double)(int32_t)bits;
  }
  return (long double)(int64_t)bits;
}
static uint64_t integer_bits(X86pX87 *f, long double value, int width) {
  int64_t result;
  if (!x86p_x87_to_int(f, value, width, &result)) {
    f->status |= X86P_X87_IE;
    result = width == 2 ? INT16_MIN : width == 4 ? INT32_MIN : INT64_MIN;
  }
  return (uint64_t)result;
}
uint64_t x86p_x87_to_i16(X86pX87 *f, long double value) {
  return integer_bits(f, value, 2);
}
uint64_t x86p_x87_to_i32(X86pX87 *f, long double value) {
  return integer_bits(f, value, 4);
}
uint64_t x86p_x87_to_i64(X86pX87 *f, long double value) {
  return integer_bits(f, value, 8);
}
