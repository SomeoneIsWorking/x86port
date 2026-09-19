/*
 * The binary64 fast path for x87 arithmetic, checked against the arithmetic it
 * bypasses.
 *
 * x86p_x87_exact_f64_arith answers a subset and refuses the rest, so it has
 * two failure modes and this checks both: an accepted operation whose result
 * differs from the full-precision one by a bit, and an operation it accepts
 * that it must not have. The second is the dangerous one -- an accepted
 * inexact operation is a silently wrong number in the game rather than a
 * crash -- so the sweeps below deliberately spend most of their operands in
 * the region where exactness is a near thing.
 *
 * Two authorities, as in test_x87_ext80_narrow.cpp:
 *   - a written-out table, which knows the right answer rather than merely
 *     what something else also says, and which names each refusal's reason;
 *   - the shipping full-precision arithmetic over pseudo-random sweeps: the
 *     Bochs softfloat where it is what ships (the WebAssembly build, which
 *     this change is for) and the host's own x87 unit where `long double` is
 *     ext80 -- the hardware the format is named after.
 *
 * The comparator is checked against a deliberately wrong answer, because a
 * differential that cannot fail reports an agreement it never established.
 */
#include "x87_exact_f64.h"

#include "x87.h"
#include "x87_softfloat.h"

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures;
long long g_checks;
long long g_taken;
long long g_refused;

X86pExt80 ext80(uint16_t sign_exp, uint64_t signif) {
  X86pExt80 out;
  out.signif = signif;
  out.sign_exp = sign_exp;
  return out;
}

/* A value the guest would have loaded with FLD m32: the case the fast path
   exists for, since a game's floats arrive from memory as binary32. */
X86pExt80 from_f32(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return x86p_ext80_from_f32_bits(bits);
}

X86pExt80 from_f64(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return x86p_ext80_from_f64_bits(bits);
}

bool same(X86pExt80 a, X86pExt80 b) {
  return a.signif == b.signif && (a.sign_exp & 0xFFFFu) == (b.sign_exp & 0xFFFFu);
}

void expect_taken(X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 want, const char *what) {
  X86pExt80 got;
  g_checks++;
  std::memset(&got, 0, sizeof got);
  if (!x86p_x87_exact_f64_arith(X86P_X87_CW_INIT, op, x, y, &got)) {
    std::printf("FAIL %s: refused\n", what);
    g_failures++;
    return;
  }
  if (!same(got, want)) {
    std::printf("FAIL %s: got %04x:%016llx want %04x:%016llx\n",
                what,
                got.sign_exp,
                (unsigned long long)got.signif,
                want.sign_exp,
                (unsigned long long)want.signif);
    g_failures++;
  }
}

void expect_refused(X86pX87Op op, X86pExt80 x, X86pExt80 y, const char *what) {
  X86pExt80 got;
  g_checks++;
  std::memset(&got, 0, sizeof got);
  if (x86p_x87_exact_f64_arith(X86P_X87_CW_INIT, op, x, y, &got)) {
    std::printf("FAIL %s: taken, produced %04x:%016llx\n", what, got.sign_exp, (unsigned long long)got.signif);
    g_failures++;
  }
}

#if X86P_X87_BINARY128
#define HAVE_ORACLE 1
/* The shipping arithmetic on the host where it is the shipping one. */
bool oracle(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *out, uint16_t *status) {
  X86pX87Reg a;
  X86pX87Reg b;
  X86pX87Reg r;
  std::memset(&a, 0, sizeof a);
  std::memset(&b, 0, sizeof b);
  a.signif = x.signif;
  a.sign_exp = x.sign_exp;
  b.signif = y.signif;
  b.sign_exp = y.sign_exp;
  *status = 0;
  r = x86p_x87_software_arith_raw(control, op, a, b, status);
  out->signif = r.signif;
  out->sign_exp = r.sign_exp;
  return true;
}
const char *oracle_name = "softfloat";
#elif LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define HAVE_ORACLE 1
/* The unit the format is named after. The default control word selects
   round-to-nearest at 64-bit precision, which is the mode the rule requires,
   so the host's own default environment is the right one to compare in. */
long double host_value(X86pExt80 v) {
  unsigned char bytes[sizeof(long double)];
  long double value;
  std::memset(bytes, 0, sizeof bytes);
  std::memcpy(bytes, &v.signif, 8);
  std::memcpy(bytes + 8, &v.sign_exp, 2);
  std::memcpy(&value, bytes, sizeof value);
  return value;
}

bool oracle(uint16_t control, X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *out, uint16_t *status) {
  const long double a = host_value(x);
  const long double b = host_value(y);
  long double r = 0.0L;
  unsigned char bytes[sizeof(long double)];
  (void)control;
  *status = 0;
  switch (op) {
  case kX86pX87Add:
    r = a + b;
    break;
  case kX86pX87Sub:
    r = a - b;
    break;
  case kX86pX87Mul:
    r = a * b;
    break;
  case kX86pX87Div:
    r = a / b;
    break;
  default:
    return false;
  }
  std::memcpy(bytes, &r, sizeof r);
  std::memcpy(&out->signif, bytes, 8);
  std::memcpy(&out->sign_exp, bytes + 8, 2);
  return true;
}
const char *oracle_name = "host x87";
#endif

#if HAVE_ORACLE
/* One operation through both arms. Counts which arm answered, so "they agree"
   cannot be reported by a sweep that never took the fast path. */
void differ(X86pX87Op op, X86pExt80 x, X86pExt80 y, const char *what) {
  X86pExt80 got;
  X86pExt80 want;
  uint16_t status = 0;
  std::memset(&got, 0, sizeof got);
  std::memset(&want, 0, sizeof want);
  if (!x86p_x87_exact_f64_arith(X86P_X87_CW_INIT, op, x, y, &got)) {
    g_refused++;
    return;
  }
  g_taken++;
  g_checks++;
  if (!oracle(X86P_X87_CW_INIT, op, x, y, &want, &status)) {
    return;
  }
  if (!same(got, want)) {
    std::printf("FAIL %s %s %04x:%016llx %04x:%016llx: got %04x:%016llx %s %04x:%016llx\n",
                what,
                x86p_x87_op_name(op),
                x.sign_exp,
                (unsigned long long)x.signif,
                y.sign_exp,
                (unsigned long long)y.signif,
                got.sign_exp,
                (unsigned long long)got.signif,
                oracle_name,
                want.sign_exp,
                (unsigned long long)want.signif);
    g_failures++;
  }
  /* An accepted operation raises nothing. Where the oracle reports status --
     the softfloat arm -- a flag here would mean the rule accepted a case it
     documents as a refusal. */
  if (status != 0u) {
    std::printf("FAIL %s: accepted an operation the softfloat flagged 0x%04x\n", what, status);
    g_failures++;
  }
}

/* Each sweep's own acceptance rate. The three sweeps ask different questions
   and a pooled rate would hide the one that matters: what a game's binary32
   operands do. */
void sweep_report(const char *what, long long taken_before, long long refused_before) {
  const long long taken = g_taken - taken_before;
  const long long refused = g_refused - refused_before;
  std::printf("  %-16s %lld of %lld taken (%.1f%%)\n",
              what,
              taken,
              taken + refused,
              100.0 * (double)taken / (double)(taken + refused));
}

/* xorshift64, so every host sweeps the same values and a failure reproduces. */
uint64_t next(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}
#endif

void table() {
  /* Ordinary binary32-derived arithmetic: the population the path is for. */
  expect_taken(kX86pX87Add, from_f32(1.5f), from_f32(2.25f), from_f64(3.75), "1.5 + 2.25");
  expect_taken(kX86pX87Sub, from_f32(5.0f), from_f32(0.5f), from_f64(4.5), "5 - 0.5");
  expect_taken(kX86pX87Mul, from_f32(3.0f), from_f32(0.25f), from_f64(0.75), "3 * 0.25");
  expect_taken(kX86pX87Div, from_f32(7.0f), from_f32(2.0f), from_f64(3.5), "7 / 2");
  /* Two binary32 significands multiply to at most 48 bits, so the whole f32
     population multiplies exactly. */
  expect_taken(kX86pX87Mul,
               from_f32(16777215.0f),
               from_f32(16777215.0f),
               from_f64(16777215.0 * 16777215.0),
               "f32 max significands");

  /* Inexact, and each for its own reason. */
  expect_refused(kX86pX87Div, from_f32(1.0f), from_f32(3.0f), "1/3 does not terminate");
  expect_refused(kX86pX87Add, from_f64(1.0), from_f64(DBL_EPSILON / 4.0), "sum needs more than 53 bits");
  expect_refused(kX86pX87Mul, from_f64(1.0 + DBL_EPSILON), from_f64(1.0 + DBL_EPSILON), "product needs 55 bits");

  /* Operands binary64 cannot hold. */
  expect_refused(kX86pX87Add, ext80(0x3FFFu, 0x8000000000000001ull), from_f32(1.0f), "65-bit significand");
  expect_refused(kX86pX87Add, ext80(0x7FFFu, 0x8000000000000000ull), from_f32(1.0f), "infinity");
  expect_refused(kX86pX87Add, ext80(0x7FFFu, 0xC000000000000000ull), from_f32(1.0f), "NaN");
  expect_refused(kX86pX87Add, ext80(0x0000u, 0x0000000000000001ull), from_f32(1.0f), "ext80 subnormal");
  expect_refused(kX86pX87Add, ext80(0x3FFFu, 0x4000000000000000ull), from_f32(1.0f), "unnormal");
  expect_refused(kX86pX87Mul, ext80(0x7000u, 0x8000000000000000ull), from_f32(1.0f), "exponent above binary64");
  expect_refused(kX86pX87Mul, ext80(0x1000u, 0x8000000000000000ull), from_f32(1.0f), "exponent below binary64");

  /* Results this rule does not produce. */
  expect_refused(kX86pX87Sub, from_f32(1.0f), from_f32(1.0f), "an exact zero, whose sign is RC's");
  expect_refused(kX86pX87Mul, from_f32(0.0f), from_f32(2.0f), "a zero product");
  expect_refused(kX86pX87Mul, from_f64(DBL_MAX), from_f64(2.0), "an overflow");
  expect_refused(kX86pX87Mul, from_f64(DBL_MIN), from_f64(0.5), "a binary64 subnormal result");

  /* The precision the guest asked for is part of the rule. */
  {
    X86pExt80 got;
    const uint16_t single = (uint16_t)((X86P_X87_CW_INIT & ~X86P_X87_PC_MASK) | X86P_X87_PC_SINGLE);
    const uint16_t dbl = (uint16_t)((X86P_X87_CW_INIT & ~X86P_X87_PC_MASK) | X86P_X87_PC_DOUBLE);
    g_checks += 2;
    if (x86p_x87_exact_f64_arith(single, kX86pX87Add, from_f32(1.5f), from_f32(2.25f), &got)) {
      std::printf("FAIL PC=single was taken\n");
      g_failures++;
    }
    if (x86p_x87_exact_f64_arith(dbl, kX86pX87Add, from_f32(1.5f), from_f32(2.25f), &got)) {
      std::printf("FAIL PC=double was taken\n");
      g_failures++;
    }
  }
}

} /* namespace */

int main() {
  table();

#if HAVE_ORACLE
  {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    const X86pX87Op ops[4] = {kX86pX87Add, kX86pX87Sub, kX86pX87Mul, kX86pX87Div};
    int i;
    /* Binary32 operands: what a game's floats are, and where the acceptance
       rate is the number the fast path lives or dies by. */
    long long mark_taken = g_taken;
    long long mark_refused = g_refused;
    for (i = 0; i < 40000; i++) {
      const uint64_t bits = next(&state);
      float a;
      float b;
      const uint32_t ab = (uint32_t)(bits >> 32);
      const uint32_t bb = (uint32_t)bits;
      std::memcpy(&a, &ab, sizeof a);
      std::memcpy(&b, &bb, sizeof b);
      if (a != a || b != b) {
        continue;
      }
      differ(ops[i & 3], from_f32(a), from_f32(b), "f32 sweep");
    }
    sweep_report("binary32", mark_taken, mark_refused);
    mark_taken = g_taken;
    mark_refused = g_refused;
    /* Full-width ext80 operands: mostly refusals, and the sweep that would
       catch a rule that accepts a 64-bit significand. */
    for (i = 0; i < 40000; i++) {
      const uint64_t sa = next(&state) | (1ull << 63);
      const uint64_t sb = next(&state) | (1ull << 63);
      const uint16_t ea = (uint16_t)(0x3F00u + (next(&state) & 0xFFu));
      const uint16_t eb = (uint16_t)(0x3F00u + (next(&state) & 0xFFu));
      differ(ops[i & 3], ext80(ea, sa), ext80(eb, sb), "ext80 sweep");
    }
    sweep_report("full ext80", mark_taken, mark_refused);
    mark_taken = g_taken;
    mark_refused = g_refused;
    /* The near-miss region: significands with just enough trailing zeros to
       be a binary64 value, so acceptance turns on the arithmetic rather than
       on the operands. */
    for (i = 0; i < 40000; i++) {
      const uint64_t sa = (next(&state) | (1ull << 63)) & ~0x7FFull;
      const uint64_t sb = (next(&state) | (1ull << 63)) & ~0x7FFull;
      const uint16_t ea = (uint16_t)(0x3FF0u + (next(&state) & 0x1Fu));
      const uint16_t eb = (uint16_t)(0x3FF0u + (next(&state) & 0x1Fu));
      differ(ops[i & 3], ext80(ea, sa), ext80(eb, sb), "near-miss sweep");
    }
    sweep_report("near miss", mark_taken, mark_refused);
  }

  /* The comparator must be able to fail. */
  {
    X86pExt80 got;
    X86pExt80 want;
    uint16_t status = 0;
    g_checks++;
    if (!x86p_x87_exact_f64_arith(X86P_X87_CW_INIT, kX86pX87Add, from_f32(1.5f), from_f32(2.25f), &got)) {
      std::printf("FAIL the comparator check could not take its own case\n");
      g_failures++;
    } else {
      oracle(X86P_X87_CW_INIT, kX86pX87Add, from_f32(1.5f), from_f32(2.5f), &want, &status);
      if (same(got, want)) {
        std::printf("FAIL the comparator called two different sums equal\n");
        g_failures++;
      }
    }
  }

  std::printf(
      "x87 exact binary64 (%s oracle): %lld check(s), %lld taken, %lld refused (%.1f%% of %lld), %d failure(s)\n",
      oracle_name,
      g_checks,
      g_taken,
      g_refused,
      100.0 * (double)g_taken / (double)(g_taken + g_refused),
      g_taken + g_refused,
      g_failures);
#else
  std::printf("x87 exact binary64: table only, this host has no full-precision oracle: %lld check(s), %d failure(s)\n",
              g_checks,
              g_failures);
#endif
  return g_failures == 0 ? 0 : 1;
}
