/*
 * When an x87 arithmetic operation can be answered in binary64 with the
 * IDENTICAL result the ext80 softfloat would produce.
 *
 * WHY. x87 emulation is about thirty percent of the browser's guest worker,
 * and after issue #162 moved the operand plumbing inline, what is left is the
 * arithmetic itself: x86p_x87_arith_raw at 16.80% of the worker with the
 * softfloat under it at another 5.25%. Every FADD, FSUB, FMUL and FDIV the
 * game executes runs a Bochs ext80 algorithm -- align, add magnitudes, round,
 * pack -- on a host whose own floating point could have done it in one
 * instruction.
 *
 * It cannot do it in general. The guest computes with a 64-bit significand
 * and binary64 has 53, so a rounded binary64 answer is a DIFFERENT number,
 * not an approximation of the same one. That is why this module decides
 * exactness rather than accuracy: it answers only the operations whose true
 * mathematical result is representable in binary64, and for those the ext80
 * result is bit-for-bit the same value, because a result that needs no
 * rounding is rounded identically by every format wide enough to hold it and
 * by every rounding mode.
 *
 * WHAT IT REFUSES, and each exclusion is a case that needs a different answer
 * rather than a case that is merely rare:
 *   - anything but 64-bit precision control. With PC set to single or double
 *     the guest asks for a rounding this does not perform, and #162 measured
 *     PC=64 on 100.0% of this title's operations, so the check costs nothing
 *     and its absence would be a silent wrong answer on a title that moves it.
 *   - an operand that is not a normal or a zero: a subnormal, an unnormal, an
 *     infinity and a NaN each have their own encoding and their own status
 *     flags.
 *   - an operand whose significand does not fit in binary64's 53 bits, or
 *     whose exponent is outside binary64's normal range. Both are ordinary
 *     for a value the guest computed at full precision and both make the
 *     conversion INTO binary64 lossy, which would decide the question before
 *     the arithmetic ran.
 *   - a result that is not a normal binary64: an overflow, a subnormal, and a
 *     ZERO. Zero is refused for a reason that is not size: the sign of an
 *     exact zero difference depends on the rounding mode (x - x is -0 under
 *     round-toward-negative and +0 otherwise), and this module deliberately
 *     does not read RC.
 *   - an inexact operation, proved inexact rather than assumed: the addition
 *     carries Knuth's two-sum error term, the multiplication counts
 *     significand bits, and the division multiplies its quotient back.
 *
 * On a refusal the caller reaches exactly the softfloat it reached before,
 * which is also what makes this testable: the two arms must agree on every
 * operation this accepts, and that is a differential test over the operand
 * space rather than an argument.
 *
 * NO STATUS FLAGS. An accepted operation raises none -- not precision, not
 * underflow, not overflow, not invalid, not divide-by-zero -- because each of
 * those conditions is one of the refusals above.
 */
#ifndef X86PORT_X87_EXACT_F64_H
#define X86PORT_X87_EXACT_F64_H

#include "x87.h"
#include "x87_ext80_widen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `x` op `y`, in operand order (the caller has already applied FSUBR/FDIVR's
 * swap). Returns 1 with *out holding the exact result, or 0 having written
 * nothing.
 */
int x86p_x87_exact_f64_arith(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *out);

/*
 * The conversion the rule above is built on, published for the differential
 * tests and for a backend that emits the accepted case inline: 1 and *out on
 * an ext80 normal or zero whose value binary64 holds exactly, 0 otherwise.
 */
int x86p_x87_exact_f64_from_ext80(X86pExt80 v, double *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_EXACT_F64_H */
