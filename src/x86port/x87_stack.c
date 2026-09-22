/*
 * x87_stack.c -- the public register-file symbols.
 *
 * The eight registers, their tags and TOP. The implementations are `static
 * inline` in x87_stack.h because they are on every x87 operation the engine
 * performs; this file gives them external definitions for callers outside the
 * library, and owns the two things that have no reason to be inline.
 *
 * Nothing here computes a value.
 */
#include "x87_stack.h"

int x86p_x87_depth(const X86pX87 *f) {
  int i, n = 0;
  if (!f) {
    return 0;
  }
  for (i = 0; i < X86P_X87_REGS; i++) {
    if (f->tag[i] != (uint8_t)kX86pX87TagEmpty) {
      n++;
    }
  }
  return n;
}

int x86p_x87_get_raw(const X86pX87 *f, int i, X86pX87Reg *out) {
  return x86p_x87_read(f, i, out);
}

int x86p_x87_set_raw(X86pX87 *f, int i, X86pX87Reg v) {
  return x86p_x87_write(f, i, v);
}

int x86p_x87_push_raw(X86pX87 *f, X86pX87Reg v) {
  return x86p_x87_push_value(f, v);
}

int x86p_x87_pop_raw(X86pX87 *f, X86pX87Reg *out) {
  return x86p_x87_pop_value(f, out);
}

int x86p_x87_get(const X86pX87 *f, int i, long double *out) {
  X86pX87Reg raw;
  if (!out || !x86p_x87_read(f, i, &raw)) {
    return 0;
  }
  *out = x86p_x87_long_double_of(raw);
  return 1;
}

int x86p_x87_set(X86pX87 *f, int i, long double v) {
  return x86p_x87_write(f, i, x86p_x87_reg_of(v));
}

int x86p_x87_push(X86pX87 *f, long double v) {
  return x86p_x87_push_value(f, x86p_x87_reg_of(v));
}

int x86p_x87_pop(X86pX87 *f, long double *out) {
  X86pX87Reg raw;
  if (!x86p_x87_pop_value(f, &raw)) {
    return 0;
  }
  if (out) {
    *out = x86p_x87_long_double_of(raw);
  }
  return 1;
}
