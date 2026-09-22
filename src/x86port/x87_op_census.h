/*
 * WHAT A RUN'S x87 ARITHMETIC ACTUALLY IS -- counted, never decided on.
 *
 * x86p_x87_arith_raw and x86p_x87_arith_ext80_fast both perform arithmetic and
 * each reaches operations the other does not, so the counting lives here and
 * both call it. Keeping it beside either one is how an instrument comes to
 * under-report the moment the other path takes work away from it.
 *
 * Every column asks the SAME predicates x86p_ext80_mul_ordinary asks, by
 * calling them rather than by repeating their preconditions, so a census
 * cannot report headroom an inline path does not have.
 */
#ifndef X86PORT_X87_OP_CENSUS_H
#define X86PORT_X87_OP_CENSUS_H

#include "x87.h"
#include "x87_ext80_widen.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note one operation, with its operands in the order the guest gave them.
 *
 * `f->op_census` must be armed; the caller tests that, because the test is one
 * predictable branch on a path that costs tens of nanoseconds and a call is
 * not.
 */
void x86p_x87_census_note_ext80(X86pX87 *f, X86pX87Op op, X86pExt80 x, X86pExt80 y);
/* The same, from the register file's own storage. */
void x86p_x87_census_note_reg(X86pX87 *f, X86pX87Op op, X86pX87Reg x, X86pX87Reg y);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_OP_CENSUS_H */
