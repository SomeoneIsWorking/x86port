/*
 * The ordinary case of a float store, checked against the conversion it is a
 * fast path for.
 *
 * x86p_ext80_narrow_nearest answers a subset and refuses the rest, so it has
 * TWO failure modes and this checks both: an accepted value whose bits differ
 * from the general conversion, and a value it accepts that it must not have
 * (or refuses that it should have taken, which is measured as a denominator
 * rather than asserted case by case).
 *
 * Two authorities, as in test_x87_ext80_widen.cpp:
 *   - a written-out table with the bits spelled out, which runs on every host
 *     and is the only check that knows the right answer rather than merely
 *     what something else also says;
 *   - the softfloat conversion this bypasses, or the host's own x87 where
 *     `long double` is ext80, over structured and pseudo-random sweeps.
 *
 * The comparator is checked against a deliberately wrong value, because a
 * differential that cannot fail reports an agreement it never established.
 */
#include "x87_ext80_narrow.h"

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

void expect_taken(X86pExt80 v, unsigned width, uint64_t want, const char *what) {
  uint64_t got = 0;
  g_checks++;
  if (!x86p_ext80_narrow_nearest(v, width, &got)) {
    std::printf("FAIL %s: refused, want 0x%016llx\n", what, (unsigned long long)want);
    g_failures++;
    return;
  }
  if (got != want) {
    std::printf("FAIL %s: got 0x%016llx want 0x%016llx\n", what, (unsigned long long)got, (unsigned long long)want);
    g_failures++;
  }
}

void expect_refused(X86pExt80 v, unsigned width, const char *what) {
  uint64_t got = 0xA5A5A5A5A5A5A5A5ull;
  g_checks++;
  if (x86p_ext80_narrow_nearest(v, width, &got)) {
    std::printf("FAIL %s: taken, produced 0x%016llx\n", what, (unsigned long long)got);
    g_failures++;
  }
}

#if X86P_X87_BINARY128
#define HAVE_ORACLE 1
/* The shipping conversion, on the host where it is the shipping one -- which
   is the WASM build, the one this change is for. */
uint64_t oracle(X86pExt80 v, unsigned width) {
  X86pX87Reg reg;
  std::memset(&reg, 0, sizeof reg);
  reg.signif = v.signif;
  reg.sign_exp = v.sign_exp;
  return x86p_x87_software_narrow_raw(X86P_X87_CW_INIT, reg, width == 8u ? 1 : 0);
}
const char *oracle_name = "softfloat";
#elif LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define HAVE_ORACLE 1
/* The unit the format is named after, rounding to nearest because that is the
   one mode the module under test implements and the host's default. */
uint64_t oracle(X86pExt80 v, unsigned width) {
  unsigned char bytes[sizeof(long double)];
  long double value;
  std::memset(bytes, 0, sizeof bytes);
  std::memcpy(bytes, &v.signif, 8);
  std::memcpy(bytes + 8, &v.sign_exp, 2);
  std::memcpy(&value, bytes, sizeof value);
  if (width == 8u) {
    const double narrowed = (double)value;
    uint64_t bits;
    std::memcpy(&bits, &narrowed, sizeof bits);
    return bits;
  }
  {
    const float narrowed = (float)value;
    uint32_t bits;
    std::memcpy(&bits, &narrowed, sizeof bits);
    return bits;
  }
}
const char *oracle_name = "host x87";
#endif

#if HAVE_ORACLE
/* One value through both arms. Counts which arm answered, so "they agree"
   cannot be reported by a sweep that took the fast path for nothing. */
void differ(X86pExt80 v, unsigned width, const char *what) {
  uint64_t got = 0;
  if (!x86p_ext80_narrow_nearest(v, width, &got)) {
    g_refused++;
    return;
  }
  g_taken++;
  g_checks++;
  {
    const uint64_t want = oracle(v, width);
    if (got != want) {
      std::printf("FAIL %s %04x:%016llx width %u: got 0x%016llx %s 0x%016llx\n",
                  what,
                  v.sign_exp,
                  (unsigned long long)v.signif,
                  width,
                  (unsigned long long)got,
                  oracle_name,
                  (unsigned long long)want);
      g_failures++;
    }
  }
}

/* xorshift64, so every host sweeps the same values and a failure is one a
   rerun reproduces. */
uint64_t next(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

void sweeps() {
  uint64_t state = 0x9E3779B97F4A7C15ull;
  /* Exponents around 1.0, where a game's coordinates and matrices live, and
     out to both ends of binary64's normal range. */
  for (int i = 0; i < 200000; i++) {
    const uint64_t r = next(&state);
    const uint16_t sign = (uint16_t)((r & 1u) << 15);
    /* An exponent inside binary64's normal range once rebiased, so the sweep
       exercises the arm rather than the refusal. */
    const uint16_t exp = (uint16_t)(X86P_EXT80_BIAS - 1000 + (int)((r >> 8) % 2000u));
    const uint64_t signif = 0x8000000000000000ull | (next(&state) >> 1);
    differ(ext80((uint16_t)(sign | exp), signif), 8u, "sweep f64");
    differ(ext80((uint16_t)(sign | exp), signif), 4u, "sweep f32");
  }
  /* The significands that decide a tie: exactly half an ulp, half plus one,
     half minus one, and all-ones, at both widths. */
  for (unsigned width = 4u; width <= 8u; width += 4u) {
    const unsigned drop = 63u - (width == 4u ? 23u : 52u);
    const uint64_t half = (uint64_t)1u << (drop - 1u);
    for (int odd = 0; odd < 2; odd++) {
      const uint64_t base = 0x8000000000000000ull | ((uint64_t)odd << drop);
      differ(ext80(X86P_EXT80_BIAS, base | half), width, "tie exactly");
      differ(ext80(X86P_EXT80_BIAS, base | (half + 1u)), width, "tie above");
      differ(ext80(X86P_EXT80_BIAS, base | (half - 1u)), width, "tie below");
    }
    /* All ones below the integer bit: rounding carries out of the top. */
    differ(ext80(X86P_EXT80_BIAS, ~(uint64_t)0), width, "carry out");
  }
  /* Every exponent, at both widths, so the boundaries between taken and
     refused are crossed rather than assumed. */
  for (uint32_t exp = 1u; exp < 0x7FFFu; exp++) {
    differ(ext80((uint16_t)exp, 0x8000000000000000ull), 4u, "exponent sweep f32");
    differ(ext80((uint16_t)exp, 0xFFFFFFFFFFFFFFFFull), 4u, "exponent sweep f32 max");
    differ(ext80((uint16_t)exp, 0x8000000000000000ull), 8u, "exponent sweep f64");
    differ(ext80((uint16_t)exp, 0xFFFFFFFFFFFFFFFFull), 8u, "exponent sweep f64 max");
  }
}
#endif

void table() {
  const uint64_t one = 0x8000000000000000ull;
  /* 1.0 and its neighbours, both widths. */
  expect_taken(ext80(X86P_EXT80_BIAS, one), 4u, 0x3F800000ull, "f32 1.0");
  expect_taken(ext80((uint16_t)(0x8000u | X86P_EXT80_BIAS), one), 4u, 0xBF800000ull, "f32 -1.0");
  expect_taken(ext80(X86P_EXT80_BIAS, one), 8u, 0x3FF0000000000000ull, "f64 1.0");
  expect_taken(ext80(X86P_EXT80_BIAS, one | 0x4000000000000000ull), 8u, 0x3FF8000000000000ull, "f64 1.5");
  /* The bits below binary32's fraction are dropped: 1 + 2^-24 is a tie and
     rounds to even, which is 1.0 itself. */
  expect_taken(ext80(X86P_EXT80_BIAS, one | ((uint64_t)1u << 39)), 4u, 0x3F800000ull, "f32 tie down to even");
  /* One ulp above that tie rounds up instead. */
  expect_taken(ext80(X86P_EXT80_BIAS, one | ((uint64_t)1u << 39) | 1u), 4u, 0x3F800001ull, "f32 above the tie");
  /* A tie from an odd last bit rounds up, to the even neighbour. */
  expect_taken(ext80(X86P_EXT80_BIAS, one | ((uint64_t)1u << 40) | ((uint64_t)1u << 39)),
               4u,
               0x3F800002ull,
               "f32 tie up to even");
  /* Rounding carries out of the significand: every bit set becomes 2.0. */
  expect_taken(ext80(X86P_EXT80_BIAS, ~(uint64_t)0), 4u, 0x40000000ull, "f32 carry to 2.0");
  expect_taken(ext80(X86P_EXT80_BIAS, ~(uint64_t)0), 8u, 0x4000000000000000ull, "f64 carry to 2.0");
  /* binary32's normal boundaries, in ext80 exponents. */
  expect_taken(ext80((uint16_t)(X86P_EXT80_BIAS - 126), one), 4u, 0x00800000ull, "f32 min normal");
  expect_taken(ext80((uint16_t)(X86P_EXT80_BIAS + 127), 0xFFFFFF0000000000ull), 4u, 0x7F7FFFFFull, "f32 max normal");

  /* THE REFUSALS. Each is a different answer, not a rare one. */
  expect_refused(ext80(0u, 0u), 4u, "refuse +0");
  expect_refused(ext80(0x8000u, 0u), 8u, "refuse -0");
  expect_refused(ext80(1u, one), 8u, "refuse ext80 subnormal exponent");
  expect_refused(ext80(0x7FFFu, one), 8u, "refuse infinity");
  expect_refused(ext80(0x7FFFu, one | 0x4000000000000000ull), 8u, "refuse NaN");
  /* An unnormal: a stored exponent with no explicit integer bit. */
  expect_refused(ext80(X86P_EXT80_BIAS, one >> 1), 8u, "refuse unnormal");
  /* One exponent below binary32's smallest normal is a subnormal result. */
  expect_refused(ext80((uint16_t)(X86P_EXT80_BIAS - 127), one), 4u, "refuse f32 subnormal result");
  /* One above its largest is an infinity. */
  expect_refused(ext80((uint16_t)(X86P_EXT80_BIAS + 128), one), 4u, "refuse f32 overflow");
  /* And a value that rounds INTO the overflow: binary32's largest normal with
     every dropped bit set rounds up to infinity, so the carry must refuse. */
  expect_refused(ext80((uint16_t)(X86P_EXT80_BIAS + 127), ~(uint64_t)0), 4u, "refuse f32 rounds to infinity");
  expect_refused(ext80((uint16_t)(X86P_EXT80_BIAS + 1023), ~(uint64_t)0), 8u, "refuse f64 rounds to infinity");
  /* A width this does not narrow to. */
  expect_refused(ext80(X86P_EXT80_BIAS, one), 10u, "refuse width 10");
  expect_refused(ext80(X86P_EXT80_BIAS, one), 2u, "refuse width 2");
}

/* The comparator, against a value that must not pass. */
void comparator() {
  uint64_t got = 0;
  g_checks++;
  if (!x86p_ext80_narrow_nearest(ext80(X86P_EXT80_BIAS, 0x8000000000000000ull), 8u, &got) ||
      got == 0x3FF0000000000001ull) {
    std::printf("FAIL comparator: a wrong answer would have passed\n");
    g_failures++;
  }
}

} /* namespace */

int main() {
  table();
  comparator();
#if HAVE_ORACLE
  sweeps();
  std::printf("differential against %s: %lld taken, %lld refused\n", oracle_name, g_taken, g_refused);
  if (g_taken == 0 || g_refused == 0) {
    std::printf("FAIL: one arm was never reached, so this proved nothing about it\n");
    g_failures++;
  }
#else
  std::printf("no ext80 oracle on this host: the table ran, the sweeps did not\n");
#endif
  std::printf("%s: %lld checks, %d failure(s)\n", g_failures ? "FAILED" : "ok", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
