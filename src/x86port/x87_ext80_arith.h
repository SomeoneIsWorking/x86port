/*
 * The ordinary case of x87 arithmetic, as integer operations on the ext80
 * encoding.
 *
 * WHY. x87 is about thirty percent of the browser's guest worker and, after
 * issue #162 moved the operand plumbing inline, the arithmetic itself is what
 * is left: `x86p_x87_arith_raw` at 16.80% with the Bochs softfloat's own
 * frames at another 5.25%. Measured under node, one operation through the
 * shipping path costs 15.6 ns for a multiply, of which about 3.5 ns is the
 * adapter building a `softfloat_status_t` -- a rounding mode, a precision and
 * six exception masks -- for a computation that will raise at most one flag.
 *
 * WHAT "ORDINARY" MEANS, and each exclusion is a case that needs a different
 * answer rather than a case that is merely rare:
 *   - both operands NORMAL: a stored exponent that is neither zero nor all
 *     ones, and the explicit integer bit set. A zero, a subnormal, an
 *     unnormal, an infinity and a NaN each have their own encoding, their own
 *     result and their own flags.
 *   - round-to-nearest-even only. The other three modes are the guest's RC
 *     field, which #162 measured moving on 1.65% of operations.
 *   - 80-bit precision control. With PC set to single or double the guest asks
 *     for a narrower rounding this does not perform.
 *   - a result that is a normal: no overflow to infinity, no subnormal, and no
 *     exponent at the format's edges.
 * Everything else returns 0 and reaches exactly the softfloat it reached
 * before, so this decides speed and never an answer.
 *
 * THE FLAGS ARE PART OF THE ANSWER. An accepted operation reports the same
 * bits Bochs would: the precision (inexact) flag when the rounding dropped
 * anything, and the rounding-up bit when it rounded away from zero. A fast
 * path that produced the right number and the wrong status word would be
 * wrong in the one place a guest can see a rounding.
 */
#ifndef X86PORT_X87_EXT80_ARITH_H
#define X86PORT_X87_EXT80_ARITH_H

#include "x87.h"
#include "x87_ext80_widen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The two halves of "ordinary" that a caller can ask about without performing
 * the operation. A census that wants to know how much of a run an inline path
 * could reach needs exactly these, and reusing them here is what stops the
 * answer drifting from what the operation actually accepts.
 */
int x86p_ext80_is_normal(X86pExt80 v);
/* A true zero, of either sign: the case a census must separate from the rest,
   because an operand that is zero has a trivial answer a wider rule could give
   and a subnormal or a NaN does not. */
int x86p_ext80_is_zero(X86pExt80 v);
/* What both rules below actually accept as an operand. */
int x86p_ext80_is_normal_or_zero(X86pExt80 v);
int x86p_ext80_control_is_ordinary(uint16_t control);

/*
 * `x` * `y`, both operands in the order the guest gave them. Returns 1 with
 * *out and the status bits ORed into *flags, or 0 having written nothing.
 *
 * `control` is the guest's control word; the rounding mode and precision
 * fields decide whether this can answer at all.
 */
int x86p_ext80_mul_ordinary(uint16_t control, X86pExt80 x, X86pExt80 y, X86pExt80 *out, uint16_t *flags);

/*
 * `x` + `y`, or `x` - `y` when `subtract` is non-zero -- which is the only
 * difference between them, so FSUB is this entry point and not a second one
 * that could acquire a bug FADD does not have.
 *
 * The same contract as the multiply: 1 with *out and the status bits ORed into
 * *flags, or 0 having written nothing. An exact cancellation to zero is
 * refused along with everything else non-normal, because a zero's sign is
 * decided by the rounding mode and that is the softfloat's rule to apply.
 */
int x86p_ext80_add_ordinary(uint16_t control, X86pExt80 x, X86pExt80 y, int subtract, X86pExt80 *out, uint16_t *flags);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_EXT80_ARITH_H */
