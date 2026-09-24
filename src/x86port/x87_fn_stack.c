#include "x87.h"
#include "x87_double_fn.h"
#include "x87_transcendental.h"

int x86p_x87_apply_fn(X86pX87 *f, X86pX87Fn fn) {
  long double a = 0.0L;
  long double b = 0.0L;
  long double r0 = 0.0L;
  long double r1 = 0.0L;
  int pushed = 0;
  uint16_t sw = 0u;
  int two_operand = (fn == kX86pX87FnPatan || fn == kX86pX87FnYl2x || fn == kX86pX87FnYl2xp1 || fn == kX86pX87FnScale ||
                     fn == kX86pX87FnPrem || fn == kX86pX87FnPrem1);

  if (f->double_arith && x86p_x87_double_fn(f, fn)) {
    return 1;
  }
  if (!x86p_x87_get(f, 0, &a)) {
    /* x86p_x87_get has already set the stack-fault and invalid-operation
       flags; the instruction produces no result, exactly as the other arms
       here do. */
    return 1;
  }
  if (two_operand && !x86p_x87_get(f, 1, &b)) {
    /* x86p_x87_get has already set the stack-fault and invalid-operation
       flags; the instruction produces no result, exactly as the other arms
       here do. */
    return 1;
  }
  const int evaluated = x86p_x87_fn(fn, f->control, a, b, &r0, &r1, &pushed, &sw);
  if (!evaluated) {
    /* No x87 unit on this host. Refused by name rather than substituted. */
    return 0;
  }
  /* C1 and C2 are guest-visible: C2 reports an incomplete FPREM reduction
     and an out-of-range trigonometric argument, and guest code loops on it. */
  /* FSQRT defines only C1, its rounding direction; the host's leftover
     C0/C2/C3 are not the guest's, so they keep the guest's values -- as the
     x64 inline form does. */
  const uint16_t written =
      fn == kX86pX87FnSqrt ? (uint16_t)X86P_X87_C1 : (uint16_t)(X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3);
  f->status &= (uint16_t)~written;
  f->status |= (uint16_t)(sw & written);
#if !defined(__x86_64__) && !defined(__i386__)
  /* The software environment starts clean for each guest operation. Native
     x87 status also contains unrelated host sticky flags and is not copied. */
  f->status |= (uint16_t)(sw & (X86P_X87_IE | X86P_X87_DE | X86P_X87_ZE | X86P_X87_OE | X86P_X87_UE | X86P_X87_PE));
#endif

  if (fn == kX86pX87FnPatan || fn == kX86pX87FnYl2x || fn == kX86pX87FnYl2xp1) {
    /* These consume BOTH registers and leave one result: pop, then replace. */
    long double dropped;
    (void)x86p_x87_pop(f, &dropped);
    x86p_x87_set(f, 0, r0);
  } else {
    x86p_x87_set(f, 0, r0);
    if (pushed) {
      x86p_x87_push(f, r1);
    }
  }
  return 1;
}
