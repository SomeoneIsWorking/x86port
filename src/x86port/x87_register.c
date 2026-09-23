#include "x87.h"
#include <math.h>
#include <stddef.h>
void x86p_x87_compare_register(X86pX87 *f, int index, unsigned pops) {
  long double other;
  if (!x86p_x87_get(f, index, &other)) {
    return;
  }
  x86p_x87_compare(f, other);
  while (pops--) {
    x86p_x87_pop(f, NULL);
  }
}
void x86p_x87_exchange(X86pX87 *f, int index) {
  long double a, b;
  if (!x86p_x87_get(f, 0, &a) || !x86p_x87_get(f, index, &b)) {
    return;
  }
  x86p_x87_set(f, 0, b);
  x86p_x87_set(f, index, a);
}
void x86p_x87_sign(X86pX87 *f, int absolute) {
  long double v;
  if (!x86p_x87_get(f, 0, &v)) {
    return;
  }
  /* FABS clears the sign bit and FCHS flips it, whatever the value: a NaN's
     sign changes too, and neither raises anything. */
  v = absolute ? fabsl(v) : -v;
  x86p_x87_set(f, 0, v);
}
void x86p_x87_test(X86pX87 *f) {
  long double v;
  if (!x86p_x87_get(f, 0, &v)) {
    return;
  }
  /* FTST clears C1, as every comparison does. */
  f->status &= (uint16_t)~(X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3);
  if (v < 0) {
    f->status |= X86P_X87_C0;
  } else if (v == 0) {
    f->status |= X86P_X87_C3;
  } else if (!(v > 0)) {
    f->status |= X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
  }
}

int x86p_x87_compare_flags(X86pX87 *f, X86pFlags *flags, int index) {
  long double x, y;
  uint32_t result = X86P_EFLAGS_FIXED;
  if (!x86p_x87_get(f, 0, &x) || !x86p_x87_get(f, index, &y)) {
    return 0;
  }
  if (isnan(x) || isnan(y)) {
    result |= X86P_ZF | X86P_PF | X86P_CF;
  } else if (x < y) {
    result |= X86P_CF;
  } else if (x == y) {
    result |= X86P_ZF;
  }
  x86p_flags_set_explicit(flags, result);
  /* FCOMI and FUCOMI report in EFLAGS and clear C1 in the status word. */
  f->status &= (uint16_t)~X86P_X87_C1;
  return 1;
}
void x86p_x87_free(X86pX87 *f, int index) {
  if (!f || index < 0 || index >= X86P_X87_REGS) {
    return;
  }
  f->tag[(f->top + index) & (X86P_X87_REGS - 1)] = (uint8_t)kX86pX87TagEmpty;
}
