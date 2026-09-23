/*
 * x87 arithmetic computed in binary64: the mode a consumer selects when it
 * accepts, on every host, the precision a Darwin binary64 host already has.
 *
 * WHY. On a host with no x87 unit, extended-precision arithmetic is software:
 * the browser's guest worker spends a fifth of its time on x87 arithmetic and
 * its operand plumbing (issue #162 in X-Men 2), against one host instruction
 * per operation for a binary64 answer. The guest runs at 64-bit precision
 * control, so binary64 is not its answer -- measured, one operation in seven
 * differs in its low bits -- and the mode is therefore a consumer's choice,
 * off by default, never an optimisation applied behind its back.
 *
 * WHAT IT ANSWERS, and each exclusion reaches the exact path unchanged:
 *   - round-to-nearest control, at double or extended precision. Single
 *     precision asks for a narrower rounding than this performs, and the other
 *     rounding modes are the guest's to choose.
 *   - operands that are zeros, or normals that round to a binary64 normal.
 *     An infinity, a NaN, a subnormal and an unnormal each have their own
 *     answer and flags.
 *   - results that are zeros or binary64 normals. An overflow, a divide by
 *     zero, an invalid operation and a subnormal result are refused, so every
 *     exceptional case keeps its x87 status bits.
 * An accepted operation raises no status bits: its inexact and rounded-up
 * reports would describe a rounding the guest did not ask for.
 *
 * THERE ARE TWO IMPLEMENTATIONS OF THIS: the C below, and the WebAssembly
 * jit_wasm_x87_arith.c emits. tests/test_wasm_x87.c runs the emitted form
 * against this one over values that separate every refusal.
 */
#ifndef X86PORT_X87_DOUBLE_ARITH_H
#define X86PORT_X87_DOUBLE_ARITH_H

#include "x87.h"
#include "x87_ext80_widen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether `control` is one this mode answers under. */
int x86p_x87_double_control_applies(uint16_t control);

/* `v` rounded to nearest binary64, when it is a zero or rounds to a normal. */
int x86p_ext80_to_double(X86pExt80 v, double *out);

/* `v` as ext80, when it is a zero or a normal. */
int x86p_ext80_of_double(double v, X86pExt80 *out);

/*
 * `x` op `y` in binary64, both operands in the order the guest gave them.
 * Returns 1 with *out, or 0 having written nothing.
 */
int x86p_ext80_double_arith(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_DOUBLE_ARITH_H */
