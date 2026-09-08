/* The software implementation is compared with an independent x87 emulator
 * or CPU when available, at the double precision used by the ARM64 state. */
#include "x87.h"
#include "x87_softfloat.h"
#include "x87_transcendental.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if LDBL_MANT_DIG >= 64
static int same_value(long double a, long double b) {
  if (isnan(a) || isnan(b)) {
    return isnan(a) && isnan(b);
  }
  return a == b && (a != 0 || !!signbit(a) == !!signbit(b));
}

#endif

static unsigned arithmetic_checks(unsigned *checks, unsigned *oracle_cases) {
  unsigned failed = 0;
#if (defined(__x86_64__) || defined(__i386__)) && LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
  static const long double values[] = {
      0, -0.0L, 1, -1, 1.5L, -1.5L, 0x1.000000000000001p0L, 0x1p-63L, 0x1p63L, 0x1p-1000L, 0x1p1000L};
  static const unsigned precision[] = {0, 0x200, 0x300};
  for (unsigned pc = 0; pc < 3; pc++) {
    for (unsigned rc = 0; rc < 4; rc++) {
      const uint16_t control = (uint16_t)(0x7F | precision[pc] | rc << 10);
      for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
        for (unsigned j = 0; j < sizeof values / sizeof *values; j++) {
          for (int op = 0; op < kX86pX87OpCount; op++) {
            uint16_t status;
            const long double got = x86p_x87_software_arith(control, (X86pX87Op)op, values[i], values[j], &status);
            (*checks)++;
            X86pX87 f;
            x86p_x87_reset(&f);
            f.control = control;
            x86p_x87_push(&f, values[i]);
            x86p_x87_arith(&f, (X86pX87Op)op, 0, values[j], 0);
            long double expected;
            x86p_x87_get(&f, 0, &expected);
            (*oracle_cases)++;
            if (!same_value(got, expected)) {
              if (failed < 8) {
                printf("software arithmetic op=%d cw=%x inputs=%La,%La result=%La expected=%La\n",
                       op,
                       control,
                       values[i],
                       values[j],
                       got,
                       expected);
              }
              failed++;
            }
          }
        }
      }
    }
  }
#else
  (void)oracle_cases;
  (void)checks;
#endif
  /* This operation separates one guest rounding from a rounded intermediate. */
#if LDBL_MANT_DIG >= 64
  uint16_t status = 0;
  (*checks)++;
  if (!same_value(x86p_x87_software_arith(0xB7F, kX86pX87Add, 1, 0x1p-1000L, &status), 0x1.0000000000000002p0L) ||
      !(status & X86P_X87_PE)) {
    failed++;
  }
#endif
  return failed;
}

static unsigned conversion_checks(unsigned *checks, unsigned *oracle_cases) {
  unsigned failed = 0;
  for (unsigned rc = 0; rc < 4; rc++) {
    X86pX87 f;
    x86p_x87_reset(&f);
    f.control |= (uint16_t)(rc << 10);
#if (defined(__x86_64__) || defined(__i386__)) && LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
    static const long double values[] = {
        0, -0.0L, 1, -1, 1.75L, -1.75L, 0x1.000001p0L, 0x1.00000000000008p0L, 0x1p-150L, 0x1p-1000L, 0x1p1000L};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
      for (int is64 = 0; is64 < 2; is64++) {
        const uint64_t bits = x86p_x87_software_narrow(f.control, values[i], is64);
        const uint64_t expected = is64 ? x86p_x87_to_f64(&f, values[i]) : x86p_x87_to_f32(&f, values[i]);
        (*checks)++;
        (*oracle_cases)++;
        if (bits != expected) {
          failed++;
        }
      }
    }
#else
    (void)oracle_cases;
#endif
    static const uint32_t rounded_half[] = {0x3F800000u, 0x3F800000u, 0x3F800001u, 0x3F800000u};
    (*checks)++;
    if (x86p_x87_software_narrow(f.control, 0x1.000001p0L, 0) != rounded_half[rc]) {
      failed++;
    }
    int64_t integer = 77;
    static const int expected[] = {2, 1, 2, 1};
    (*checks)++;
    if (!x86p_x87_software_integer(f.control, 1.75L, 8, &integer) || integer != expected[rc]) {
      failed++;
    }
    (*checks)++;
    if (x86p_x87_software_integer(f.control, 0x1p63L, 8, &integer) || integer != expected[rc]) {
      failed++;
    }
  }
  return failed;
}

static unsigned production_rounding_checks(unsigned *checks) {
  unsigned failed = 0;
#if LDBL_MANT_DIG >= 64
  static const unsigned precision[] = {0, 0x200, 0x300};
  static const unsigned bits[] = {24, 53, 64};
  for (unsigned pc = 0; pc < 3; pc++) {
    for (unsigned rc = 0; rc < 4; rc++) {
      for (int negative = 0; negative < 2; negative++) {
        X86pX87 f;
        x86p_x87_reset(&f);
        f.control = (uint16_t)(0x7F | precision[pc] | rc << 10);
        const long double sign = negative ? -1 : 1;
        const long double step = ldexpl(1, 1 - (int)bits[pc]);
        const long double expected = sign * ((rc == (negative ? 1u : 2u)) ? 1 + step : 1);
        x86p_x87_push(&f, sign);
        long double result = 0;
        (*checks)++;
        if (!x86p_x87_arith(&f, kX86pX87Add, 0, sign * 0x1p-1000L, 0) || !x86p_x87_get(&f, 0, &result) ||
            result != expected) {
          failed++;
        }
      }
    }
  }
  static const uint8_t encodings[][10] = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80},
                                          {1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                          {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0, 0},
                                          {0, 0, 0, 0, 0, 0, 0, 0x80, 1, 0},
                                          {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0x7f},
                                          {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x7f},
                                          {0, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f}};
  for (unsigned i = 0; i < sizeof encodings / sizeof *encodings; i++) {
    uint8_t bytes[10];
    x86p_x87_to_f80(x86p_x87_from_f80(encodings[i]), bytes);
    (*checks)++;
    if (memcmp(bytes, encodings[i], sizeof bytes)) {
      failed++;
    }
  }
#else
  (void)checks;
#endif
  return failed;
}

static unsigned transcendental_precision_checks(unsigned *checks, unsigned *oracle_cases) {
  static const struct {
    X86pX87Fn fn;
    long double a, b;
  } cases[] = {{kX86pX87FnPatan, -1, 0.5L},
               {kX86pX87FnPatan, -1, -0.5L},
               {kX86pX87FnPatan, 0.5L, -1},
               {kX86pX87FnYl2xp1, 0.25L, 0.75L},
               {kX86pX87FnYl2xp1, -0.25L, 0.75L},
               {kX86pX87FnYl2x, 1.25L, 0.75L},
               {kX86pX87FnSqrt, 2, 0}};
  static const uint16_t precision[] = {0, 0x200, 0x300};
  unsigned failed = 0;
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    for (unsigned rc = 0; rc < 4; rc++) {
      const uint16_t extended = (uint16_t)(X86P_X87_CW_INIT | rc << 10);
      long double baseline = 0, baseline_extra = 0;
      int baseline_pushed = 0;
      uint16_t baseline_status = 0;
      if (!x86p_x87_fn_software_control(cases[i].fn,
                                        extended,
                                        cases[i].a,
                                        cases[i].b,
                                        &baseline,
                                        &baseline_extra,
                                        &baseline_pushed,
                                        &baseline_status)) {
        (*checks)++;
        failed++;
        continue;
      }
      for (unsigned pc = 0; pc < 3; pc++) {
        const uint16_t control = (uint16_t)(0x7F | precision[pc] | rc << 10);
        long double result = 0, extra = 0;
        int pushed = 0;
        uint16_t status = 0;
        const int evaluated = x86p_x87_fn_software_control(
            cases[i].fn, control, cases[i].a, cases[i].b, &result, &extra, &pushed, &status);
        if (cases[i].fn != kX86pX87FnSqrt) {
          /* PC governs basic arithmetic and SQRT, not transcendental results
             or the intermediate reductions in their software implementation. */
          (*checks)++;
          if (!evaluated || result != baseline || pushed != baseline_pushed) {
            if (failed < 8) {
              printf("%s cw=%x changed with PC: got=%La extended=%La\n",
                     x86p_x87_fn_name(cases[i].fn),
                     control,
                     result,
                     baseline);
            }
            failed++;
          }
        }
#if (defined(__x86_64__) || defined(__i386__)) && LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
        long double expected = 0, expected_extra = 0;
        uint16_t saved_control, expected_status;
        int expected_pushed;
        __asm__ volatile("fnstcw %0\n\tfldcw %1" : "=m"(saved_control) : "m"(control) : "memory");
        const int oracle_evaluated = x86p_x87_fn(
            cases[i].fn, cases[i].a, cases[i].b, &expected, &expected_extra, &expected_pushed, &expected_status);
        __asm__ volatile("fldcw %0" : : "m"(saved_control) : "memory");
        (*checks)++;
        (*oracle_cases)++;
        const int matches = cases[i].fn == kX86pX87FnSqrt
                                ? result == expected
                                : fabsl(result - expected) <= 8 * DBL_EPSILON * fmaxl(1, fabsl(expected));
        if (!evaluated || !oracle_evaluated || !matches || pushed != expected_pushed) {
          failed++;
        }
#else
        (void)oracle_cases;
#endif
      }
    }
  }
  return failed;
}

int main(void) {
  unsigned checks = 0, failed = 0, oracle_cases = 0;
  failed += arithmetic_checks(&checks, &oracle_cases);
  failed += conversion_checks(&checks, &oracle_cases);
  failed += production_rounding_checks(&checks);
  failed += transcendental_precision_checks(&checks, &oracle_cases);
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
