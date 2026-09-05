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
  if (absolute) {
    v = (v < 0 || (v == 0 && signbit(v))) ? -v : v;
  } else {
    v = -v;
  }
  x86p_x87_set(f, 0, v);
}
void x86p_x87_test(X86pX87 *f) {
  long double v;
  if (!x86p_x87_get(f, 0, &v)) {
    return;
  }
  f->status &= (uint16_t)~(X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3);
  if (v < 0) {
    f->status |= X86P_X87_C0;
  } else if (v == 0) {
    f->status |= X86P_X87_C3;
  } else if (!(v > 0)) {
    f->status |= X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
  }
}
