/*
 * The binary64 arithmetic mode (x87_double_arith.h): what it answers, what it
 * refuses to the exact path, and the unit-level switch that selects it.
 *
 * Every accepted answer is checked against the host's own binary64 operation
 * on the same values, and every refusal class is exercised, because a mode that
 * accepted an infinity or a subnormal result would lose the x87 status bits the
 * exact path reports for it.
 */
#include "x87.h"
#include "x87_double_arith.h"
#include "x87_ext80_widen.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
  if (!ok) {
    printf("  FAIL  %s\n", what);
    failures++;
  }
}

static uint64_t bits_of(double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  return bits;
}

static X86pExt80 ext80_of(double v) {
  return x86p_ext80_from_f64_bits(bits_of(v));
}

static X86pExt80 ext80(uint16_t sign_exp, uint64_t signif) {
  X86pExt80 out;
  out.signif = signif;
  out.sign_exp = sign_exp;
  return out;
}

static int same(X86pExt80 a, X86pExt80 b) {
  return a.signif == b.signif && a.sign_exp == b.sign_exp;
}

/* x op y through the mode, against the host's binary64 answer. */
static void accepts(X86pX87Op op, double x, double y, double expected, const char *what) {
  X86pExt80 r;
  X86pExt80 want;
  const int answered = x86p_ext80_double_arith(X86P_X87_CW_INIT, op, ext80_of(x), ext80_of(y), &r);
  check(answered && x86p_ext80_of_double(expected, &want) && same(r, want), what);
}

static void refuses(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, const char *what) {
  X86pExt80 r = ext80(0x1234u, 0x5678u);
  check(!x86p_ext80_double_arith(control, op, x, y, &r) && r.sign_exp == 0x1234u && r.signif == 0x5678u, what);
}

static void test_answers(void) {
  volatile double third = 1.0 / 3.0;
  accepts(kX86pX87Add, 1.5, 2.25, 3.75, "an exact sum");
  accepts(kX86pX87Sub, 1.0, third, 1.0 - third, "a rounded difference is binary64's");
  accepts(kX86pX87Mul, third, 3.0, third * 3.0, "a rounded product is binary64's");
  accepts(kX86pX87Div, 1.0, 3.0, third, "a quotient, which the exact rules never answer");
  accepts(kX86pX87Sub, 2.5, 2.5, 0.0, "a cancellation is +0 at round-to-nearest");
  accepts(kX86pX87Mul, -0.0, 4.0, -0.0, "a signed zero operand keeps its sign");
  check(x86p_x87_double_control_applies(0x027Fu), "double precision control is answered");
}

static void test_operand_rounding(void) {
  /* 1 + 2^-60 is not a binary64: it rounds to 1 on the way in, so 1 is the
     answer to (1 + 2^-60) - 1 here and 2^-60 on the exact path. */
  X86pExt80 r;
  X86pExt80 zero;
  const X86pExt80 one_and_a_bit = ext80(0x3FFFu, 0x8000000000000010ull);
  check(x86p_ext80_double_arith(X86P_X87_CW_INIT, kX86pX87Sub, one_and_a_bit, ext80_of(1.0), &r) &&
            x86p_ext80_of_double(0.0, &zero) && same(r, zero),
        "an ext80 operand is rounded to binary64 before the operation");
}

static void test_refusals(void) {
  const X86pExt80 one = ext80_of(1.0);
  refuses(0x007Fu, kX86pX87Add, one, one, "single precision control");
  refuses(0x0B7Fu, kX86pX87Add, one, one, "round-up control");
  refuses(X86P_X87_CW_INIT, kX86pX87Add, ext80(0x7FFFu, 0x8000000000000000ull), one, "an infinite operand");
  refuses(X86P_X87_CW_INIT, kX86pX87Add, ext80(0x7FFFu, 0xC000000000000000ull), one, "a NaN operand");
  refuses(
      X86P_X87_CW_INIT, kX86pX87Add, ext80(0x0001u, 0x8000000000000000ull), one, "an operand below binary64's range");
  refuses(X86P_X87_CW_INIT, kX86pX87Add, ext80(0x3FFFu, 0x4000000000000000ull), one, "an unnormal operand");
  refuses(X86P_X87_CW_INIT, kX86pX87Div, one, ext80_of(0.0), "a divide by zero");
  refuses(X86P_X87_CW_INIT, kX86pX87Mul, ext80_of(DBL_MAX), ext80_of(2.0), "an overflow");
  refuses(X86P_X87_CW_INIT, kX86pX87Mul, ext80_of(DBL_MIN), ext80_of(0.5), "a subnormal result");
  refuses(X86P_X87_CW_INIT, kX86pX87OpCount, one, one, "an operation that is not arithmetic");
}

static long double sum_minus_one(X86pX87 *f) {
  long double r = -1.0L;
  x86p_x87_push(f, 1.0L);
  x86p_x87_arith(f, kX86pX87Add, 0, 0x1p-60L, 0);
  x86p_x87_arith(f, kX86pX87Sub, 0, 1.0L, 0);
  x86p_x87_pop(f, &r);
  return r;
}

static void test_unit_switch(void) {
  X86pX87 f;
  x86p_x87_reset(&f);
  check(!f.double_arith, "a reset unit computes in extended");
  if (!x86p_x87_double_arith_available()) {
    check(!x86p_x87_set_double_arith(&f, 1) && !f.double_arith, "a host without the ext80 file refuses the mode");
    check(x86p_x87_set_double_arith(&f, 0), "any host accepts extended");
    return;
  }
  check(x86p_x87_set_double_arith(&f, 1) && f.double_arith, "the mode is selected");
  /* 1 + 2^-60 - 1: 2^-60 in extended, 0 in binary64, so the result says
     which arithmetic ran. */
  check(sum_minus_one(&f) == 0.0L, "the unit's arithmetic ran in binary64");
  check(!(f.status & X86P_X87_PE), "an accepted operation raises no status bits");
  x86p_x87_reset(&f);
  check(f.double_arith, "FINIT leaves the host's choice alone");
  x86p_x87_set_double_arith(&f, 0);
  check(sum_minus_one(&f) == 0x1p-60L, "extended arithmetic is observably different");
}

int main(void) {
  test_answers();
  test_operand_rounding();
  test_refusals();
  test_unit_switch();
  if (failures) {
    printf("test_x87_double_arith: %d failure(s)\n", failures);
    return 1;
  }
  printf("test_x87_double_arith: passed (mode %s on this host)\n",
         x86p_x87_double_arith_available() ? "available" : "refused");
  return 0;
}
