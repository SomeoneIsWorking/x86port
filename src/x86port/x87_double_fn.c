/* See x87_double_fn.h. */
#include "x87_double_fn.h"

#include "x87_double_arith.h"
#include "x87_ext80_arith.h"
#include "x87_stack.h"

#include <math.h>
#include <string.h>

#if X86P_X87_BINARY128

/* The reduction range of FSIN, FCOS, FSINCOS and FPTAN. */
static const double kTrigRange = 9223372036854775808.0; /* 2^63 */

static int operand(const X86pX87 *f, int i, double *out) {
  X86pExt80 wide;
  return x86p_x87_ext80_of_st(f, i, &wide) && x86p_ext80_to_double(wide, out);
}

static int result(double v, X86pX87Reg *out) {
  X86pExt80 wide;
  if (!x86p_ext80_of_double(v, &wide)) {
    return 0;
  }
  /* The whole object, padding included: the register file is compared as
     memory (x87.c, reg_of_ext80). */
  memset(out, 0, sizeof *out);
  out->signif = wide.signif;
  out->sign_exp = wide.sign_exp;
  return 1;
}

int x86p_x87_double_fn(X86pX87 *f, X86pX87Fn fn) {
  double a;
  double b = 0.0;
  X86pX87Reg r0;
  X86pX87Reg r1;
  int pushes = fn == kX86pX87FnSinCos || fn == kX86pX87FnPtan;

  if (!x86p_x87_double_control_applies(f->control) || !operand(f, 0, &a)) {
    return 0;
  }
  if (pushes && f->tag[(f->top - 1) & (X86P_X87_REGS - 1)] != (uint8_t)kX86pX87TagEmpty) {
    return 0;
  }
  switch (fn) {
  case kX86pX87FnSqrt:
    if (signbit(a) && a != 0.0) {
      return 0;
    }
    if (!result(sqrt(a), &r0)) {
      return 0;
    }
    break;
  case kX86pX87FnSin:
  case kX86pX87FnCos:
  case kX86pX87FnSinCos:
  case kX86pX87FnPtan:
    if (fabs(a) >= kTrigRange) {
      return 0;
    }
    if (fn == kX86pX87FnPtan) {
      if (!result(tan(a), &r0) || !result(1.0, &r1)) {
        return 0;
      }
    } else if (!result(fn == kX86pX87FnCos ? cos(a) : sin(a), &r0) ||
               (fn == kX86pX87FnSinCos && !result(cos(a), &r1))) {
      return 0;
    }
    break;
  case kX86pX87FnPatan:
    if (!operand(f, 1, &b) || !result(atan2(b, a), &r0)) {
      return 0;
    }
    break;
  default:
    return 0;
  }

  /* Decided; now the writes, in the order x86p_x87_apply_fn makes them. */
  f->status &= (uint16_t)~(fn == kX86pX87FnSqrt ? (uint16_t)X86P_X87_C1
                                                : (uint16_t)(X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3));
  if (fn == kX86pX87FnPatan) {
    (void)x86p_x87_pop_value(f, NULL);
  }
  (void)x86p_x87_write(f, 0, r0);
  if (pushes) {
    (void)x86p_x87_push_value(f, r1);
  }
  return 1;
}

#else

int x86p_x87_double_fn(X86pX87 *f, X86pX87Fn fn) {
  /* No binary64 mode here (x86p_x87_double_arith_available). */
  (void)f;
  (void)fn;
  return 0;
}

#endif
