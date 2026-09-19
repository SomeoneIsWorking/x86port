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
/* The same arithmetic in the register file's storage type. On a binary128 host
   X86pX87Reg holds exactly the fields floatx80 holds, so this converts nothing
   -- which is the point: the long double form above widens both operands and
   narrows the result on every call. Both share one dispatch below. */
X86pX87Reg x86p_x87_software_arith_raw(uint16_t control, X86pX87Op op, X86pX87Reg a, X86pX87Reg b, uint16_t *status);
long double x86p_x87_software_constant(uint16_t control, long double value);
uint64_t x86p_x87_software_narrow(uint16_t control, long double value, int is64);
/* The same narrowing from the storage type. The long double form has to widen
   its argument to ext80 first, which on a binary128 host means the value was
   converted out of ext80 and straight back into it -- x86p_x87_software_narrow
   was 3.46% of the browser's guest worker doing exactly that. */
uint64_t x86p_x87_software_narrow_raw(uint16_t control, X86pX87Reg value, int is64);
X86pX87Reg x86p_x87_software_widen_f32(uint32_t bits);
X86pX87Reg x86p_x87_software_widen_f64(uint64_t bits);
int x86p_x87_software_integer(uint16_t control, long double value, int width, int64_t *out);
long double x86p_x87_software_decode(const uint8_t bytes[10]);
void x86p_x87_software_encode(long double value, uint8_t bytes[10]);
#ifdef __cplusplus
}
#endif
#endif
