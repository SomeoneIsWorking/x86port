/* Software floating-point operations used when the host has no x87/fenv.
 * These preserve numerical ext80 values; raw MMX and noncanonical ext80
 * register encodings still require an architectural register representation. */
#ifndef X86PORT_X87_SOFTFLOAT_H
#define X86PORT_X87_SOFTFLOAT_H
#include "x87.h"
#ifdef __cplusplus
extern "C" {
#endif
long double x86p_x87_software_arith(uint16_t control, X86pX87Op op, long double a, long double b, uint16_t *status);
long double x86p_x87_software_constant(uint16_t control, long double value);
uint64_t x86p_x87_software_narrow(uint16_t control, long double value, int is64);
int x86p_x87_software_integer(uint16_t control, long double value, int width, int64_t *out);
long double x86p_x87_software_decode(const uint8_t bytes[10]);
void x86p_x87_software_encode(long double value, uint8_t bytes[10]);
#ifdef __cplusplus
}
#endif
#endif
