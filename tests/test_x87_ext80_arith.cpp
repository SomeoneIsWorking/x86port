/*
 * The ordinary case of x87 arithmetic, checked against the softfloat it is a
 * fast path for.
 *
 * x86p_ext80_mul_ordinary answers a subset and refuses the rest, so it has two
 * failure modes and this checks both: an accepted operation whose result or
 * status differs from the full-precision one, and an operation it accepts that
 * it must not have. The value, the exponent AND the status word are compared,
 * because a rounding a guest can observe through C1 is part of the answer.
 *
 * Two authorities, as in test_x87_ext80_narrow.cpp:
 *   - a written-out table, which knows the right answer rather than merely
 *     what something else also says, and which names each refusal's reason;
 *   - the shipping arithmetic over pseudo-random sweeps: Bochs's extF80_mul
 *     where it is what ships, the host's own x87 unit where `long double` is
 *     ext80 -- the hardware the format is named after.
 *
 * The sweeps spend most of their operands where the rounding is a near thing:
 * significands whose low bits force a tie, and exponents at the edges of what
 * the rule will take.
 *
 * The comparator is checked against a deliberately wrong product, because a
 * differential that cannot fail reports an agreement it never established.
 */
#include "x87_ext80_arith.h"

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

void expect_taken(X86pExt80 x, X86pExt80 y, X86pExt80 want, uint16_t want_flags, const char *what) {
  X86pExt80 got;
  uint16_t flags = 0;
  g_checks++;
  std::memset(&got, 0, sizeof got);
  if (!x86p_ext80_mul_ordinary(X86P_X87_CW_INIT, x, y, &got, &flags)) {
    std::printf("FAIL %s: refused\n", what);
    g_failures++;
    return;
  }
  if (got.signif != want.signif || got.sign_exp != want.sign_exp || flags != want_flags) {
    std::printf("FAIL %s: got %04x:%016llx f%03x want %04x:%016llx f%03x\n",
                what,
                got.sign_exp,
                (unsigned long long)got.signif,
                flags,
                want.sign_exp,
                (unsigned long long)want.signif,
                want_flags);
    g_failures++;
  }
}

void expect_refused(uint16_t control, X86pExt80 x, X86pExt80 y, const char *what) {
  X86pExt80 got;
  uint16_t flags = 0xA5A5u;
  g_checks++;
  std::memset(&got, 0, sizeof got);
  if (x86p_ext80_mul_ordinary(control, x, y, &got, &flags)) {
    std::printf("FAIL %s: taken, produced %04x:%016llx\n", what, got.sign_exp, (unsigned long long)got.signif);
    g_failures++;
  }
  if (flags != 0xA5A5u) {
    std::printf("FAIL %s: a refusal touched the caller's status word\n", what);
    g_failures++;
  }
}

#if X86P_X87_BINARY128
#define HAVE_ORACLE 1
X86pExt80 oracle(X86pX87Op op, X86pExt80 x, X86pExt80 y, uint16_t *flags) {
  X86pX87Reg a;
  X86pX87Reg b;
  X86pX87Reg r;
  X86pExt80 out;
  std::memset(&a, 0, sizeof a);
  std::memset(&b, 0, sizeof b);
  a.signif = x.signif;
  a.sign_exp = x.sign_exp;
  b.signif = y.signif;
  b.sign_exp = y.sign_exp;
  *flags = 0;
  r = x86p_x87_software_arith_raw(X86P_X87_CW_INIT, op, a, b, flags);
  out.signif = r.signif;
  out.sign_exp = r.sign_exp;
  return out;
}
const char *oracle_name = "softfloat";
const int oracle_reports_flags = 1;
#elif LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define HAVE_ORACLE 1
/* The unit the format is named after. It reports no status word here -- the
   host's own exception flags are not read back -- so the sweep compares the
   VALUE against hardware and leaves the flags to the softfloat arm, which is
   the build this change is for. */
X86pExt80 oracle(X86pX87Op op, X86pExt80 x, X86pExt80 y, uint16_t *flags) {
  unsigned char bytes[sizeof(long double)];
  long double a;
  long double b;
  long double r;
  X86pExt80 out;
  std::memset(bytes, 0, sizeof bytes);
  std::memcpy(bytes, &x.signif, 8);
  std::memcpy(bytes + 8, &x.sign_exp, 2);
  std::memcpy(&a, bytes, sizeof a);
  std::memset(bytes, 0, sizeof bytes);
  std::memcpy(bytes, &y.signif, 8);
  std::memcpy(bytes + 8, &y.sign_exp, 2);
  std::memcpy(&b, bytes, sizeof b);
  switch (op) {
  case kX86pX87Add:
    r = a + b;
    break;
  case kX86pX87Sub:
    r = a - b;
    break;
  default:
    r = a * b;
    break;
  }
  std::memcpy(bytes, &r, sizeof r);
  std::memcpy(&out.signif, bytes, 8);
  std::memcpy(&out.sign_exp, bytes + 8, 2);
  *flags = 0;
  return out;
}
const char *oracle_name = "host x87";
const int oracle_reports_flags = 0;
#endif

#if HAVE_ORACLE
int rule(X86pX87Op op, X86pExt80 x, X86pExt80 y, X86pExt80 *got, uint16_t *flags) {
  if (op == kX86pX87Mul) {
    return x86p_ext80_mul_ordinary(X86P_X87_CW_INIT, x, y, got, flags);
  }
  return x86p_ext80_add_ordinary(X86P_X87_CW_INIT, x, y, op == kX86pX87Sub, got, flags);
}

const char *op_name(X86pX87Op op) {
  return op == kX86pX87Mul ? "*" : (op == kX86pX87Sub ? "-" : "+");
}

void differ(X86pX87Op op, X86pExt80 x, X86pExt80 y, const char *what) {
  X86pExt80 got;
  X86pExt80 want;
  uint16_t flags = 0;
  uint16_t want_flags = 0;
  std::memset(&got, 0, sizeof got);
  if (!rule(op, x, y, &got, &flags)) {
    g_refused++;
    return;
  }
  g_taken++;
  g_checks++;
  want = oracle(op, x, y, &want_flags);
  if (got.signif != want.signif || got.sign_exp != want.sign_exp || (oracle_reports_flags && flags != want_flags)) {
    std::printf("FAIL %s %04x:%016llx %s %04x:%016llx: got %04x:%016llx f%03x %s %04x:%016llx f%03x\n",
                what,
                x.sign_exp,
                (unsigned long long)x.signif,
                op_name(op),
                y.sign_exp,
                (unsigned long long)y.signif,
                got.sign_exp,
                (unsigned long long)got.signif,
                flags,
                oracle_name,
                want.sign_exp,
                (unsigned long long)want.signif,
                want_flags);
    g_failures++;
  }
}

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
  const uint16_t one = (uint16_t)(X86P_EXT80_BIAS);
  const uint64_t half = 0x8000000000000000ull;
  /* 1.0 * 1.0, and the sign rule. */
  expect_taken(ext80(one, half), ext80(one, half), ext80(one, half), 0u, "1 * 1");
  expect_taken(
      ext80((uint16_t)(one | 0x8000u), half), ext80(one, half), ext80((uint16_t)(one | 0x8000u), half), 0u, "-1 * 1");
  expect_taken(
      ext80((uint16_t)(one | 0x8000u), half), ext80((uint16_t)(one | 0x8000u), half), ext80(one, half), 0u, "-1 * -1");
  /* 2.0 * 3.0 = 6.0: the exponent carries, and nothing rounds. */
  expect_taken(ext80((uint16_t)(one + 1u), half),
               ext80((uint16_t)(one + 1u), 0xC000000000000000ull),
               ext80((uint16_t)(one + 2u), 0xC000000000000000ull),
               0u,
               "2 * 3");
  /* A product whose leading one lands one place lower, so the result is
     doubled and the exponent comes back down. */
  expect_taken(ext80(one, 0x9000000000000000ull),
               ext80(one, 0x9000000000000000ull),
               ext80(one, 0xA200000000000000ull),
               0u,
               "1.125 * 1.125");

  /* A zero operand, whose answer is a zero of the combined sign and raises
     nothing. Written out rather than left to the sweep because the SIGN is the
     whole content of the case. */
  expect_taken(ext80(0u, 0u), ext80(one, half), ext80(0u, 0u), 0u, "0 * 1");
  expect_taken(ext80(0x8000u, 0u), ext80(one, half), ext80(0x8000u, 0u), 0u, "-0 * 1");
  expect_taken(ext80(0x8000u, 0u), ext80((uint16_t)(one | 0x8000u), half), ext80(0u, 0u), 0u, "-0 * -1");
  expect_taken(ext80(0u, 0u), ext80(0x8000u, 0u), ext80(0x8000u, 0u), 0u, "0 * -0");

  /* Operands with no ordinary answer. */
  expect_refused(X86P_X87_CW_INIT, ext80(0x7FFFu, half), ext80(one, half), "infinity");
  expect_refused(X86P_X87_CW_INIT, ext80(0x7FFFu, 0xC000000000000000ull), ext80(one, half), "NaN");
  expect_refused(X86P_X87_CW_INIT, ext80(0u, 1u), ext80(one, half), "subnormal");
  expect_refused(X86P_X87_CW_INIT, ext80(one, 0x4000000000000000ull), ext80(one, half), "unnormal");
  expect_refused(X86P_X87_CW_INIT, ext80(0x7FFEu, half), ext80(0x7FFEu, half), "overflow");
  expect_refused(X86P_X87_CW_INIT, ext80(1u, half), ext80(1u, half), "underflow to subnormal");

  /* The modes this rule does not implement. */
  expect_refused((uint16_t)((X86P_X87_CW_INIT & ~X86P_X87_RC_MASK) | X86P_X87_RC_DOWN),
                 ext80(one, half),
                 ext80(one, half),
                 "round toward negative");
  expect_refused((uint16_t)((X86P_X87_CW_INIT & ~X86P_X87_PC_MASK) | X86P_X87_PC_DOUBLE),
                 ext80(one, half),
                 ext80(one, half),
                 "PC=double");
}

} /* namespace */

int main() {
  table();

#if HAVE_ORACLE
  {
    static const X86pX87Op kOps[] = {kX86pX87Mul, kX86pX87Add, kX86pX87Sub};
    unsigned o;
    for (o = 0; o < sizeof kOps / sizeof kOps[0]; o++) {
      const X86pX87Op op = kOps[o];
      /* Each op gets the same stream, so a failure is the op's and not the
         operands'. */
      uint64_t state = 0x9E3779B97F4A7C15ull;
      int i;
      /* Full-width significands: every result rounds, so this is the sweep
         that exercises the tie rule and the carry out of the significand.
         The sign bits are drawn too: for an add or a subtract they decide
         whether the operation is a magnitude add or a cancellation, which are
         different code and different normalisation. */
      for (i = 0; i < 200000; i++) {
        const uint64_t sa = next(&state) | (1ull << 63);
        const uint64_t sb = next(&state) | (1ull << 63);
        const uint64_t signs = next(&state);
        const uint16_t ea = (uint16_t)(0x3000u + (next(&state) & 0x1FFFu) + ((signs & 1u) << 15));
        const uint16_t eb = (uint16_t)(0x3000u + (next(&state) & 0x1FFFu) + (((signs >> 1) & 1u) << 15));
        differ(op, ext80(ea, sa), ext80(eb, sb), "wide sweep");
      }
      /* Exact ties: a result whose dropped half is exactly one half, which is
         where round-to-even and round-half-up part company. */
      for (i = 0; i < 100000; i++) {
        const uint64_t sa = (next(&state) | (1ull << 63)) & ~0xFFFFFFFFull;
        const uint64_t sb = 0x8000000000000000ull | ((uint64_t)(next(&state) & 0xFFFFFFFFull) << 32);
        const uint64_t signs = next(&state);
        const uint16_t ea = (uint16_t)(0x3FF0u + (next(&state) & 0xFu) + ((signs & 1u) << 15));
        const uint16_t eb = (uint16_t)(0x3FF0u + (next(&state) & 0xFu) + (((signs >> 1) & 1u) << 15));
        differ(op, ext80(ea, sa), ext80(eb, sb), "tie sweep");
      }
      /* The edges: exponents that make the result overflow or go subnormal, so
         the refusals are exercised against an oracle rather than asserted. */
      for (i = 0; i < 100000; i++) {
        const uint64_t sa = next(&state) | (1ull << 63);
        const uint64_t sb = next(&state) | (1ull << 63);
        const uint64_t signs = next(&state);
        const uint16_t ea = (uint16_t)(1u + (next(&state) % 0x7FFEu) + ((signs & 1u) << 15));
        const uint16_t eb = (uint16_t)(1u + (next(&state) % 0x7FFEu) + (((signs >> 1) & 1u) << 15));
        differ(op, ext80(ea, sa), ext80(eb, sb), "edge sweep");
      }
      /*
       * NEAR CANCELLATION, and it is the sweep add and subtract exist for.
       *
       * Two operands within a few bits of each other and of opposite sign lose
       * most of the significand, and what is left has to be shifted back up by
       * a count nothing else in this file produces -- up to and past 64 places,
       * where the low half becomes the high one. The sweep above would reach
       * this only by accident: its exponents differ by thousands.
       */
      for (i = 0; i < 200000; i++) {
        const uint64_t sa = next(&state) | (1ull << 63);
        const uint64_t delta = next(&state);
        /* b within 2^-k of a, for a k that reaches both sides of 64. */
        const unsigned k = (unsigned)(next(&state) % 130u);
        const uint64_t sb = k >= 64u ? (sa ^ (delta & 1u)) : (sa ^ (delta & ((1ull << k) - 1u))) | (1ull << 63);
        const uint64_t signs = next(&state);
        const uint16_t ea = (uint16_t)(0x4000u + ((signs & 1u) << 15));
        const uint16_t eb = (uint16_t)(0x4000u - (unsigned)(next(&state) % 2u) + (((signs >> 1) & 1u) << 15));
        differ(op, ext80(ea, sa), ext80(eb, sb), "cancellation sweep");
      }
      /*
       * ZEROS, which are 44.6% of what the game's route actually performs --
       * more than half of everything the rules refused before they took them.
       * Both signs of zero on both sides, against normals and against each
       * other, because a zero's sign is the whole difference between the
       * cases: (+0) + (-0) is +0 and (-0) + (-0) is not.
       */
      for (i = 0; i < 100000; i++) {
        const uint64_t pick = next(&state);
        const uint64_t sa = next(&state) | (1ull << 63);
        const uint64_t sb = next(&state) | (1ull << 63);
        const uint16_t ea = (uint16_t)(0x3F00u + (next(&state) & 0xFFu) + ((pick & 1u) << 15));
        const uint16_t eb = (uint16_t)(0x3F00u + (next(&state) & 0xFFu) + (((pick >> 1) & 1u) << 15));
        const X86pExt80 a = (pick & 4u) ? ext80((uint16_t)((pick & 8u) << 12), 0u) : ext80(ea, sa);
        const X86pExt80 b = (pick & 16u) ? ext80((uint16_t)((pick & 32u) << 10), 0u) : ext80(eb, sb);
        differ(op, a, b, "zero sweep");
      }
      /* Binary32-derived operands: what a game supplies, and a population
         whose results mostly do not round at all. */
      for (i = 0; i < 100000; i++) {
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
        differ(op, x86p_ext80_from_f32_bits(ab), x86p_ext80_from_f32_bits(bb), "binary32 sweep");
      }
    }
  }

  /* The comparator must be able to fail. */
  {
    X86pExt80 got;
    X86pExt80 want;
    uint16_t flags = 0;
    uint16_t want_flags = 0;
    const uint16_t one = (uint16_t)X86P_EXT80_BIAS;
    g_checks++;
    if (!x86p_ext80_mul_ordinary(X86P_X87_CW_INIT,
                                 ext80((uint16_t)(one + 1u), 0x8000000000000000ull),
                                 ext80(one, 0xC000000000000000ull),
                                 &got,
                                 &flags)) {
      std::printf("FAIL the comparator check could not take its own case\n");
      g_failures++;
    } else {
      want = oracle(kX86pX87Mul, ext80(one, 0x8000000000000000ull), ext80(one, 0xC000000000000000ull), &want_flags);
      if (got.signif == want.signif && got.sign_exp == want.sign_exp) {
        std::printf("FAIL the comparator called two different products equal\n");
        g_failures++;
      }
    }
  }

  std::printf("x87 ordinary ext80 multiply, add and subtract (%s oracle): %lld check(s), %lld taken, %lld refused "
              "(%.1f%% of %lld), "
              "%d failure(s)\n",
              oracle_name,
              g_checks,
              g_taken,
              g_refused,
              100.0 * (double)g_taken / (double)(g_taken + g_refused),
              g_taken + g_refused,
              g_failures);
#else
  std::printf("x87 ordinary ext80 multiply, add and subtract: table only, no oracle on this host: %lld check(s), %d "
              "failure(s)\n",
              g_checks,
              g_failures);
#endif
  return g_failures == 0 ? 0 : 1;
}
