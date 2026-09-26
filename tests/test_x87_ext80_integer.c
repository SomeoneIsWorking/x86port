/*
 * FILD's and FIST's conversions on the ten-byte encoding (x87_ext80_integer.h).
 *
 * Two authorities, as in the other ext80 tests:
 *   - a written-out table, which runs on every host and is the only check that
 *     knows the right answer rather than merely what something else says;
 *   - the host's own FISTP, on an x86 host, over structured and pseudo-random
 *     sweeps of every rounding mode and width, the invalid operation included.
 *
 * The comparator is checked against a deliberately wrong answer, because a
 * differential that cannot fail reports an agreement it never established.
 */
#include "x87.h"
#include "x87_ext80_integer.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
static long long g_checks;

#define BIAS X86P_EXT80_BIAS
#define ONE (UINT64_C(1) << 63)
#define NEG 0x8000u

static const uint16_t kModes[] = {X86P_X87_RC_NEAREST, X86P_X87_RC_DOWN, X86P_X87_RC_UP, X86P_X87_RC_TRUNCATE};

static X86pExt80 ext80(uint16_t sign_exp, uint64_t signif) {
  X86pExt80 v;
  v.signif = signif;
  v.sign_exp = sign_exp;
  return v;
}

/* want[i] is the answer under kModes[i]; `fits` 0 means every mode refuses. */
static void expect(X86pExt80 v, int width, int fits, const int64_t want[4], const char *what) {
  for (unsigned m = 0; m < 4u; m++) {
    int64_t got = 0x5A5A5A5A;
    const int ok = x86p_ext80_to_int(kModes[m], v, width, &got);
    g_checks++;
    if (ok != fits || (fits && got != want[m])) {
      printf("FAIL %s, RC %u, width %d: %s %" PRId64 ", want %s %" PRId64 "\n",
             what,
             m,
             width,
             ok ? "got" : "refused",
             got,
             fits ? "" : "a refusal",
             fits ? want[m] : 0);
      g_failures++;
    }
  }
}

#define EXPECT(v, width, near, down, up, trunc, what)                                                                  \
  do {                                                                                                                 \
    const int64_t want_[4] = {near, down, up, trunc};                                                                  \
    expect(v, width, 1, want_, what);                                                                                  \
  } while (0)
#define REFUSED(v, width, what)                                                                                        \
  do {                                                                                                                 \
    const int64_t none_[4] = {0, 0, 0, 0};                                                                             \
    expect(v, width, 0, none_, what);                                                                                  \
  } while (0)

static void test_table(void) {
  EXPECT(ext80(0, 0), 4, 0, 0, 0, 0, "+0");
  EXPECT(ext80(NEG, 0), 4, 0, 0, 0, 0, "-0");
  EXPECT(ext80(BIAS + 1, 0xA000000000000000u), 4, 2, 2, 3, 2, "2.5 ties to even");
  EXPECT(ext80(BIAS + 1, 0xE000000000000000u), 4, 4, 3, 4, 3, "3.5 ties to even");
  EXPECT(ext80(NEG | (BIAS + 1), 0xA000000000000000u), 4, -2, -3, -2, -2, "-2.5");
  EXPECT(ext80(BIAS - 1, ONE), 4, 0, 0, 1, 0, "0.5");
  EXPECT(ext80(NEG | (BIAS - 1), ONE), 4, 0, -1, 0, 0, "-0.5");
  EXPECT(ext80(BIAS - 1, 0xC000000000000000u), 4, 1, 0, 1, 0, "0.75");
  EXPECT(ext80(BIAS - 2, 0xC000000000000000u), 4, 0, 0, 1, 0, "0.375");
  EXPECT(ext80(0, 1u), 4, 0, 0, 1, 0, "the smallest denormal");
  EXPECT(ext80(NEG, 1u), 4, 0, -1, 0, 0, "the smallest negative denormal");
  EXPECT(ext80(0, ONE), 4, 0, 0, 1, 0, "a pseudo-denormal");
  EXPECT(ext80(BIAS + 14, 0xFFFE000000000000u), 2, 32767, 32767, 32767, 32767, "32767");
  EXPECT(ext80(NEG | (BIAS + 15), ONE), 2, -32768, -32768, -32768, -32768, "-32768 in two bytes");
  REFUSED(ext80(BIAS + 15, ONE), 2, "32768 in two bytes");
  EXPECT(
      ext80(BIAS + 30, 0xFFFFFFFE00000000u), 4, 2147483647, 2147483647, INT64_C(2147483647), 2147483647, "INT32_MAX");
  {
    /* 2147483647.5: nearest and up leave the range, down and truncate do not. */
    X86pExt80 v = ext80(BIAS + 30, 0xFFFFFFFF00000000u);
    int64_t got = 0;
    g_checks += 4;
    if (x86p_ext80_to_int(X86P_X87_RC_NEAREST, v, 4, &got) || x86p_ext80_to_int(X86P_X87_RC_UP, v, 4, &got) ||
        !x86p_ext80_to_int(X86P_X87_RC_DOWN, v, 4, &got) || got != 2147483647 ||
        !x86p_ext80_to_int(X86P_X87_RC_TRUNCATE, v, 4, &got) || got != 2147483647) {
      printf("FAIL 2147483647.5 in four bytes\n");
      g_failures++;
    }
  }
  EXPECT(ext80(BIAS + 62, UINT64_MAX - 1u), 8, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX, "INT64_MAX");
  EXPECT(ext80(NEG | (BIAS + 63), ONE), 8, INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN, "-2^63");
  REFUSED(ext80(BIAS + 63, ONE), 8, "2^63");
  REFUSED(ext80(NEG | (BIAS + 63), ONE), 4, "-2^63 in four bytes");
  EXPECT(ext80(BIAS + 62, ONE | 1u),
         8,
         INT64_C(1) << 62,
         INT64_C(1) << 62,
         (INT64_C(1) << 62) + 1,
         INT64_C(1) << 62,
         "2^62 + 0.5 ties to even");
  EXPECT(ext80(BIAS + 62, ONE | 3u),
         8,
         (INT64_C(1) << 62) + 2,
         (INT64_C(1) << 62) + 1,
         (INT64_C(1) << 62) + 2,
         (INT64_C(1) << 62) + 1,
         "2^62 + 1.5 ties to even");
  REFUSED(ext80(0x7FFF, ONE), 8, "+infinity");
  REFUSED(ext80(0x7FFF, 0xC000000000000000u), 8, "a quiet NaN");
  REFUSED(ext80(BIAS, 0x4000000000000000u), 8, "an unnormal");
}

static void test_from_int(void) {
  static const struct {
    int64_t value;
    uint16_t sign_exp;
    uint64_t signif;
  } cases[] = {
      {0, 0, 0},
      {1, BIAS, ONE},
      {-1, NEG | BIAS, ONE},
      {3, BIAS + 1, 0xC000000000000000u},
      {-32768, NEG | (BIAS + 15), ONE},
      {INT64_MAX, BIAS + 62, UINT64_MAX - 1u},
      {INT64_MIN, NEG | (BIAS + 63), ONE},
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const X86pExt80 got = x86p_ext80_from_int(cases[i].value);
    g_checks++;
    if (got.sign_exp != cases[i].sign_exp || got.signif != cases[i].signif) {
      printf("FAIL from_int(%" PRId64 "): %04X %016" PRIX64 ", want %04X %016" PRIX64 "\n",
             cases[i].value,
             got.sign_exp,
             got.signif,
             cases[i].sign_exp,
             cases[i].signif);
      g_failures++;
    }
  }
}

#if defined(__x86_64__) || defined(__i386__)
/* FISTP under `control`, masked: the stored integer and whether IE was set. */
static int64_t hardware_fist(uint16_t control, X86pExt80 v, int width, int *invalid) {
  unsigned char bytes[10];
  uint16_t cw = (uint16_t)(0x037Fu & ~X86P_X87_RC_MASK) | control;
  uint16_t saved;
  uint16_t sw;
  int64_t out = 0;
  memcpy(bytes, &v.signif, 8);
  memcpy(bytes + 8, &v.sign_exp, 2);
  __asm__ volatile("fnstcw %0" : "=m"(saved));
  __asm__ volatile("fnclex\n\tfldcw %0" : : "m"(cw));
  __asm__ volatile("fldt %0" : : "m"(bytes));
  if (width == 2) {
    int16_t r;
    __asm__ volatile("fistps %0" : "=m"(r));
    out = r;
  } else if (width == 4) {
    int32_t r;
    __asm__ volatile("fistpl %0" : "=m"(r));
    out = r;
  } else {
    __asm__ volatile("fistpll %0" : "=m"(out));
  }
  __asm__ volatile("fnstsw %0" : "=m"(sw));
  __asm__ volatile("fnclex\n\tfldcw %0" : : "m"(saved));
  *invalid = (sw & X86P_X87_IE) != 0;
  return out;
}

static uint64_t g_state = 0x9E3779B97F4A7C15u;
static uint64_t next(void) {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

/* 1 when ours and the hardware agree. `lie` perturbs ours, for the check
   that the comparison can fail. */
static int agrees(uint16_t control, X86pExt80 v, int width, int lie) {
  int invalid = 0;
  const int64_t want = hardware_fist(control, v, width, &invalid);
  int64_t got = 0;
  const int ok = x86p_ext80_to_int(control, v, width, &got);
  if (lie) {
    got ^= 1;
  }
  return invalid ? !ok : (ok && got == want);
}

static void test_against_the_hardware(void) {
  long long compared = 0;
  const int widths[] = {2, 4, 8};
  for (int i = 0; i < 400000; i++) {
    const uint64_t r = next();
    /* Exponents from well below one to past 2^64, and every class: a clear
       integer bit makes an unnormal (or a pseudo-denormal at exponent 0). */
    const int unbiased = (int)(next() % 72u) - 4;
    uint16_t exp = (uint16_t)(BIAS + unbiased);
    uint64_t signif = r;
    if ((i & 15) == 0) {
      exp = 0;
    } else if ((i & 63) == 1) {
      exp = 0x7FFF;
    }
    if ((i & 7) != 3) {
      signif |= ONE;
    }
    if ((i & 3) == 2) {
      /* A short significand, so ties and exact integers occur. */
      signif &= ~((UINT64_C(1) << (next() % 64u)) - 1u);
      signif |= ONE;
    }
    const X86pExt80 v = ext80((uint16_t)(exp | ((next() & 1u) ? NEG : 0u)), signif);
    for (unsigned m = 0; m < 4u; m++) {
      for (unsigned w = 0; w < 3u; w++) {
        g_checks++;
        compared++;
        if (!agrees(kModes[m], v, widths[w], 0)) {
          int invalid = 0;
          int64_t got = 0;
          const int64_t want = hardware_fist(kModes[m], v, widths[w], &invalid);
          const int ok = x86p_ext80_to_int(kModes[m], v, widths[w], &got);
          printf("FAIL %04X %016" PRIX64 " RC %u width %d: ours %s %" PRId64 ", the x87 %s %" PRId64 "\n",
                 v.sign_exp,
                 v.signif,
                 m,
                 widths[w],
                 ok ? "stores" : "refuses",
                 got,
                 invalid ? "raises IE" : "stores",
                 want);
          if (++g_failures > 20) {
            return;
          }
        }
      }
    }
  }
  /* The comparator itself: a perturbed answer must disagree on an exact
     integer the hardware stores. */
  g_checks++;
  if (agrees(X86P_X87_RC_NEAREST, ext80(BIAS + 1, 0xC000000000000000u), 4, 1)) {
    printf("FAIL the hardware comparison accepted a wrong answer\n");
    g_failures++;
  }
  printf("compared %lld conversion(s) with the host's FISTP\n", compared);
}
#endif

int main(void) {
  test_table();
  test_from_int();
#if defined(__x86_64__) || defined(__i386__)
  test_against_the_hardware();
#else
  printf("the host has no x87: the table is the only authority here\n");
#endif
  printf("%lld check(s), %d failure(s)\n", g_checks, g_failures);
  if (g_checks == 0) {
    printf("REFUSED: nothing was checked\n");
    return 1;
  }
  return g_failures ? 1 : 0;
}
