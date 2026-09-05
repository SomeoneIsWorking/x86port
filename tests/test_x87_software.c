/* The software implementation is compared with an independent x87 emulator
 * or CPU when available, at the double precision used by the ARM64 state. */
#include "x87.h"
#include "x87_transcendental.h"
#include <float.h>
#include <math.h>
#include <stdio.h>

int main(void) {
  unsigned checks = 0, failed = 0, oracle_cases = 0;
  for (int fn = 0; fn < kX86pX87FnCount; fn++) {
    if (fn == kX86pX87FnXtract) {
      continue;
    }
    for (int i = -100; i <= 100; i++) {
      long double a = (long double)i / 128, b = 0.375L;
      if (fn == kX86pX87FnYl2x || fn == kX86pX87FnSqrt) {
        a = fabsl(a) + 0.125L;
      }
      long double r0, r1;
      uint16_t sw;
      int pushed;
      checks++;
      if (!x86p_x87_fn_software((X86pX87Fn)fn, a, b, &r0, &r1, &pushed, &sw)) {
        failed++;
        continue;
      }
#if (defined(__x86_64__) || defined(__i386__)) && LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
      long double o0, o1;
      uint16_t osw;
      int opushed;
      oracle_cases++;
      if (!x86p_x87_fn((X86pX87Fn)fn, a, b, &o0, &o1, &opushed, &osw) || pushed != opushed ||
          fabsl(r0 - o0) > 8 * DBL_EPSILON * fmaxl(1, fabsl(o0)) ||
          (pushed && fabsl(r1 - o1) > 8 * DBL_EPSILON * fmaxl(1, fabsl(o1))) || ((sw ^ osw) & X86P_X87_C2)) {
        if (failed < 8) {
          printf("%s a=%Lg: software %.20Lg/%.20Lg versus oracle %.20Lg/%.20Lg, status %x/%x\n",
                 x86p_x87_fn_name((X86pX87Fn)fn),
                 a,
                 r0,
                 r1,
                 o0,
                 o1,
                 sw,
                 osw);
        }
        failed++;
      }
#else
      /* Nonzero ARM coverage has value checks independent of SoftFloat. */
      if (fn == kX86pX87FnSin && fabsl(r0 - sinl(a)) > 8 * DBL_EPSILON) {
        failed++;
      }
      if (fn == kX86pX87FnCos && fabsl(r0 - cosl(a)) > 8 * DBL_EPSILON) {
        failed++;
      }
      if (fn == kX86pX87FnSqrt && fabsl(r0 - sqrtl(a)) > 8 * DBL_EPSILON) {
        failed++;
      }
#endif
    }
  }
  for (unsigned rc = 0; rc < 4; rc++) {
    static const long double expected[] = {2, 1, 2, 1};
    long double result = 0, extra = 0;
    int pushed = 0;
    uint16_t sw = 0;
    checks++;
    if (!x86p_x87_fn_software_control(
            kX86pX87FnRndint, (uint16_t)(0x37F | rc << 10), 1.75L, 0, &result, &extra, &pushed, &sw) ||
        result != expected[rc]) {
      failed++;
    }
  }
  for (int fn = kX86pX87FnSin; fn <= kX86pX87FnPtan; fn++) {
    long double result = 0, extra = 0;
    int pushed = -1;
    uint16_t sw = 0;
    checks++;
    if (!x86p_x87_fn_software((X86pX87Fn)fn, 0x1p63L, 0, &result, &extra, &pushed, &sw) || result != 0x1p63L ||
        pushed || !(sw & X86P_X87_C2)) {
      failed++;
    }
  }
  printf("%u software math checks, %u independent x87 comparisons, %u failures\n", checks, oracle_cases, failed);
  return failed ? 1 : 0;
}
