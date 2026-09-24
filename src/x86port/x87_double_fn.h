/*
 * x87_double_fn.h -- FSQRT, FSIN, FCOS, FSINCOS, FPTAN and FPATAN in binary64,
 * for a unit whose consumer selected binary64 arithmetic (x87_double_arith.h).
 *
 * WHY. The same trade as the arithmetic: on a host with no x87 unit these are
 * 128-bit software evaluations, and in the browser FSINCOS alone was about
 * 0.5% of the guest worker in `f128_mulAdd`, against one libm call for a
 * binary64 answer.
 *
 * WHAT IT ANSWERS, under the arithmetic's own rules -- everything else reaches
 * x86p_x87_fn unchanged, with the machine as the instruction found it:
 *   - a control word x86p_x87_double_control_applies accepts;
 *   - operands that are present (not empty) and are zeros or normals that
 *     round to a binary64 normal;
 *   - FSQRT of a non-negative operand; the trigonometric forms of an operand
 *     under 2^63 in magnitude, which is the range the instructions reduce
 *     (beyond it they set C2 and leave ST0 alone, and that answer is the exact
 *     path's);
 *   - FSINCOS and FPTAN only with ST7 free for the value they push;
 *   - results that are zeros or binary64 normals.
 * An accepted instruction clears the condition codes it defines -- C2 among
 * them, which says the argument was in range -- and raises no exception flags,
 * as the accepted arithmetic does.
 */
#ifndef X86PORT_X87_DOUBLE_FN_H
#define X86PORT_X87_DOUBLE_FN_H

#include "x87.h"
#include "x87_transcendental.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Evaluate `fn` on `f`'s stack in binary64. Returns 1 having written the whole
   result, or 0 having written nothing. */
int x86p_x87_double_fn(X86pX87 *f, X86pX87Fn fn);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_DOUBLE_FN_H */
