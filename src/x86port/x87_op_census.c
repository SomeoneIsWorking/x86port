/* See x87_op_census.h. */
#include "x87_op_census.h"

#include "x87_ext80_arith.h"

#include <stdint.h>

/*
 * The mode the operation runs in, which EVERY host can measure.
 *
 * The per-operand refusal columns below can only be asked where the register
 * file is the ext80 encoding; on a host with a real x87 unit they stay zero,
 * and a reader cannot then tell "the mode is fine" from "the mode was never
 * looked at". These two are read out of the control word's own fields, so they
 * are counted the same way whatever the storage is -- and a title running at
 * 53-bit precision is one an 80-bit-only rule cannot answer however ordinary
 * its operands are.
 */
static void note_mode(X86pX87 *f) {
  f->op_census->by_precision[(f->control >> 8) & 3u]++;
  f->op_census->by_rounding[(f->control >> 10) & 3u]++;
}

#if X86P_X87_BINARY128

void x86p_x87_census_note_ext80(X86pX87 *f, X86pX87Op op, X86pExt80 ex, X86pExt80 ey) {
  const unsigned i = (unsigned)op;
  X86pExt80 ignored;
  uint16_t ignored_flags = 0u;
  note_mode(f);
  f->op_census->ordinary_measured = 1;
  f->op_census->total[i]++;
  if (!x86p_ext80_control_is_ordinary(f->control)) {
    f->op_census->refused_control[i]++;
    return;
  }
  if (!x86p_ext80_is_normal_or_zero(ex) || !x86p_ext80_is_normal_or_zero(ey)) {
    f->op_census->refused_other[i]++;
    return;
  }
  /*
   * Everything from here is an operand shape the rules are written for, and
   * whether one TAKES it is asked by calling it rather than by repeating its
   * preconditions -- a second copy of those is how a census comes to report
   * headroom that does not exist.
   *
   * The gap between this count and the eligible ones (the total less the two
   * refusal columns) is the work not done: an operation with no rule at all,
   * an exponent that leaves the normal range, an exact cancellation.
   */
  switch (op) {
  case kX86pX87Mul:
    if (x86p_ext80_mul_ordinary(f->control, ex, ey, &ignored, &ignored_flags)) {
      f->op_census->taken[i]++;
    }
    return;
  case kX86pX87Add:
  case kX86pX87Sub:
    if (x86p_ext80_add_ordinary(f->control, ex, ey, op == kX86pX87Sub, &ignored, &ignored_flags)) {
      f->op_census->taken[i]++;
    }
    return;
  case kX86pX87Div:
  case kX86pX87OpCount:
  default:
    return;
  }
}

void x86p_x87_census_note_reg(X86pX87 *f, X86pX87Op op, X86pX87Reg x, X86pX87Reg y) {
  X86pExt80 ex;
  X86pExt80 ey;
  ex.signif = x.signif;
  ex.sign_exp = x.sign_exp;
  ey.signif = y.signif;
  ey.sign_exp = y.sign_exp;
  x86p_x87_census_note_ext80(f, op, ex, ey);
}

#else

void x86p_x87_census_note_ext80(X86pX87 *f, X86pX87Op op, X86pExt80 ex, X86pExt80 ey) {
  /* There is no ext80-encoded register file on this host, so nothing calls
     this; the entry point exists so both hosts present the same interface. */
  (void)ex;
  (void)ey;
  note_mode(f);
  f->op_census->total[(unsigned)op]++;
}

void x86p_x87_census_note_reg(X86pX87 *f, X86pX87Op op, X86pX87Reg x, X86pX87Reg y) {
  /* The register file is the host's own long double here, so the encoding the
     predicates read is not the storage. Counting the totals and the mode is
     still honest; claiming to have measured the operand columns would not
     be. */
  (void)x;
  (void)y;
  note_mode(f);
  f->op_census->total[(unsigned)op]++;
}

#endif
