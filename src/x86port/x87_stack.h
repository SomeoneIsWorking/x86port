/*
 * ST(i) -> physical register, for the two modules that need it.
 *
 * x87's eight registers are a rotating file: ST(0) is whatever TOP points at,
 * and every access is that one addition. It is `static inline` in a header
 * rather than a call because it is on every register read and write in the
 * engine, and because both x87_stack.c and the binary128 paths in x87.c ask
 * it the same question.
 */
#ifndef X86PORT_X87_STACK_H
#define X86PORT_X87_STACK_H

#include "x87.h"

static inline int x86p_x87_phys(const X86pX87 *f, int i) {
  return (f->top + i) & (X86P_X87_REGS - 1);
}

#endif /* X86PORT_X87_STACK_H */
