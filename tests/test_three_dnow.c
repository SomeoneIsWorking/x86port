/*
 * test_three_dnow -- the 3DNow! semantics, and the refusals.
 *
 * The refusal tests are the point. 3DNow! is 90% of the instructions pc/xmen2's
 * static translator could not handle (jit-common I004), so this file is the
 * first thing that will be leaned on to say "the engine covers it" -- and an
 * approximation instruction that silently returned a plausible float would make
 * "PFRCP ran correctly" and "PFRCP was never implemented" the same test result.
 */
#include "three_dnow.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                       \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ_U(got, want)                                                                                          \
  do {                                                                                                                 \
    unsigned long long g_ = (unsigned long long)(got);                                                                 \
    unsigned long long w_ = (unsigned long long)(want);                                                                \
    g_checks++;                                                                                                        \
    if (g_ != w_) {                                                                                                    \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s: got %llu (0x%llx) want %llu\n", __FILE__, __LINE__, #got, g_, g_, w_);               \
    }                                                                                                                  \
  } while (0)

static int g_test_failed;
#define RUN(fn)                                                                                                        \
  do {                                                                                                                 \
    int before = g_failed;                                                                                             \
    printf("test %s\n", #fn);                                                                                          \
    fn();                                                                                                              \
    if (g_failed != before) {                                                                                          \
      g_test_failed++;                                                                                                 \
      printf("  FAIL\n");                                                                                              \
    } else {                                                                                                           \
      printf("  PASS\n");                                                                                              \
    }                                                                                                                  \
  } while (0)

static X86pMm mmf(float lo, float hi) {
  X86pMm m;
  m.q = 0;
  m.f[0] = lo;
  m.f[1] = hi;
  return m;
}

static X86pMm mmi(int32_t lo, int32_t hi) {
  X86pMm m;
  m.q = 0;
  m.i[0] = lo;
  m.i[1] = hi;
  return m;
}

/* Every enum value has a distinct, non-empty name, and every name parses back
   to the value it came from. A new opcode that gains a route but no spelling is
   an opcode nobody can name in a refusal message. */
static void test_names_round_trip(void) {
  int i, j;
  for (i = 0; i < (int)kX86pPfCount; i++) {
    const char *ni = x86p_3dnow_name((X86pPfOp)i);
    X86pPfOp back = kX86pPfCount;
    CHECK(ni != NULL && ni[0] != '\0');
    CHECK(x86p_3dnow_parse(ni, &back));
    CHECK(back == (X86pPfOp)i);
    for (j = i + 1; j < (int)kX86pPfCount; j++) {
      CHECK(strcmp(ni, x86p_3dnow_name((X86pPfOp)j)) != 0);
    }
  }
  /* Out of range never returns null and never parses. */
  CHECK(x86p_3dnow_name((X86pPfOp)((int)kX86pPfCount + 7)) != NULL);
}

static void test_parse_rejects_what_it_should(void) {
  const char *bad[] = {NULL, "", " ", "PF", "PFADDD", "ADD", "PFRCPIT", "pfmull"};
  const int n = (int)(sizeof bad / sizeof bad[0]);
  int rejected = 0;
  int i;
  for (i = 0; i < n; i++) {
    X86pPfOp got = kX86pPfMul; /* a sentinel that is not the first value */
    if (!x86p_3dnow_parse(bad[i], &got) && got == kX86pPfMul) {
      rejected++;
    }
  }
  CHECK_EQ_U(rejected, n); /* denominator: every bad spelling probed */

  /* Case-insensitive, like every other mnemonic surface. */
  {
    X86pPfOp got = kX86pPfCount;
    CHECK(x86p_3dnow_parse("pfmul", &got) && got == kX86pPfMul);
    CHECK(x86p_3dnow_parse("PfCmpGe", &got) && got == kX86pPfCmpGe);
  }
}

static void test_arithmetic(void) {
  X86pMm r;
  CHECK(x86p_3dnow_eval(kX86pPfAdd, mmf(1.5f, -2.0f), mmf(2.5f, 0.5f), &r));
  CHECK(r.f[0] == 4.0f && r.f[1] == -1.5f);

  CHECK(x86p_3dnow_eval(kX86pPfSub, mmf(1.5f, -2.0f), mmf(2.5f, 0.5f), &r));
  CHECK(r.f[0] == -1.0f && r.f[1] == -2.5f);

  /* PFSUBR is b - a, and getting it the way round of PFSUB is the entire
     failure mode this opcode has. */
  CHECK(x86p_3dnow_eval(kX86pPfSubR, mmf(1.5f, -2.0f), mmf(2.5f, 0.5f), &r));
  CHECK(r.f[0] == 1.0f && r.f[1] == 2.5f);

  CHECK(x86p_3dnow_eval(kX86pPfMul, mmf(3.0f, -2.0f), mmf(4.0f, 0.5f), &r));
  CHECK(r.f[0] == 12.0f && r.f[1] == -1.0f);

  CHECK(x86p_3dnow_eval(kX86pPfMax, mmf(3.0f, -2.0f), mmf(4.0f, -7.0f), &r));
  CHECK(r.f[0] == 4.0f && r.f[1] == -2.0f);
  CHECK(x86p_3dnow_eval(kX86pPfMin, mmf(3.0f, -2.0f), mmf(4.0f, -7.0f), &r));
  CHECK(r.f[0] == 3.0f && r.f[1] == -7.0f);
}

/* The horizontal three. Each takes its low lane from `a` and its high lane from
   `b`, and they differ only in sign -- exactly the shape a copy-paste gets
   wrong, so all three are pinned. */
static void test_horizontal(void) {
  X86pMm r;
  CHECK(x86p_3dnow_eval(kX86pPfAcc, mmf(1.0f, 2.0f), mmf(10.0f, 20.0f), &r));
  CHECK(r.f[0] == 3.0f && r.f[1] == 30.0f);

  CHECK(x86p_3dnow_eval(kX86pPfNAcc, mmf(1.0f, 2.0f), mmf(10.0f, 20.0f), &r));
  CHECK(r.f[0] == -1.0f && r.f[1] == -10.0f);

  CHECK(x86p_3dnow_eval(kX86pPfPNAcc, mmf(1.0f, 2.0f), mmf(10.0f, 20.0f), &r));
  CHECK(r.f[0] == -1.0f && r.f[1] == 30.0f);
}

/* The compares write an all-ones MASK, not a float. A version that wrote 1.0f
   would pass an "is it non-zero" test and corrupt every use. */
static void test_compares_write_masks(void) {
  X86pMm r;
  CHECK(x86p_3dnow_eval(kX86pPfCmpEq, mmf(1.0f, 2.0f), mmf(1.0f, 3.0f), &r));
  CHECK_EQ_U(r.u[0], 0xFFFFFFFFu);
  CHECK_EQ_U(r.u[1], 0x00000000u);

  CHECK(x86p_3dnow_eval(kX86pPfCmpGe, mmf(2.0f, 2.0f), mmf(2.0f, 3.0f), &r));
  CHECK_EQ_U(r.u[0], 0xFFFFFFFFu);
  CHECK_EQ_U(r.u[1], 0x00000000u);

  /* GT and GE differ only on equality, which is the one input that tells them
     apart. */
  CHECK(x86p_3dnow_eval(kX86pPfCmpGt, mmf(2.0f, 4.0f), mmf(2.0f, 3.0f), &r));
  CHECK_EQ_U(r.u[0], 0x00000000u);
  CHECK_EQ_U(r.u[1], 0xFFFFFFFFu);
}

static void test_conversions_and_swap(void) {
  X86pMm r;
  /* The converts read the SOURCE operand (b), not the destination. */
  CHECK(x86p_3dnow_eval(kX86pPi2Fd, mmi(0, 0), mmi(-3, 7), &r));
  CHECK(r.f[0] == -3.0f && r.f[1] == 7.0f);

  /* Truncating toward zero, not rounding. */
  CHECK(x86p_3dnow_eval(kX86pPf2Id, mmi(0, 0), mmf(-3.9f, 7.9f), &r));
  CHECK_EQ_U((uint32_t)r.i[0], (uint32_t)-3);
  CHECK_EQ_U(r.i[1], 7);

  /* Out of range and NaN are CLAMPED, not left to C's undefined conversion --
     a guest instruction whose answer depends on the host is not a definition. */
  CHECK(x86p_3dnow_eval(kX86pPf2Id, mmi(0, 0), mmf(1e30f, -1e30f), &r));
  CHECK_EQ_U((uint32_t)r.i[0], 0x7FFFFFFFu);
  CHECK_EQ_U((uint32_t)r.i[1], 0x80000000u);

  CHECK(x86p_3dnow_eval(kX86pPf2Iw, mmi(0, 0), mmf(70000.0f, -70000.0f), &r));
  CHECK_EQ_U(r.i[0], 32767);
  CHECK_EQ_U((uint32_t)r.i[1], (uint32_t)-32768);

  CHECK(x86p_3dnow_eval(kX86pPi2Fw, mmi(0, 0), mmi((int32_t)0xDEADFFFFu, 5), &r));
  CHECK(r.f[0] == -1.0f); /* low word 0xFFFF read as signed, upper half ignored */
  CHECK(r.f[1] == 5.0f);

  CHECK(x86p_3dnow_eval(kX86pPSwapD, mmi(0, 0), mmi(0x11111111, 0x22222222), &r));
  CHECK_EQ_U((uint32_t)r.i[0], 0x22222222u);
  CHECK_EQ_U((uint32_t)r.i[1], 0x11111111u);
}

static void test_packed_integer(void) {
  X86pMm a, b, r;
  a.q = 0x0102030405060708ull;
  b.q = 0x0102030405060708ull;
  CHECK(x86p_3dnow_eval(kX86pPAvgUsb, a, b, &r));
  CHECK_EQ_U(r.q, 0x0102030405060708ull); /* average of equals is itself */

  a.q = 0x0000000000000001ull;
  b.q = 0x0000000000000002ull;
  CHECK(x86p_3dnow_eval(kX86pPAvgUsb, a, b, &r));
  CHECK_EQ_U(r.q & 0xFFu, 2u); /* (1+2+1)>>1 == 2: rounds UP, not down */

  a.q = 0x4000400040004000ull; /* 0x4000 in all four words */
  b.q = 0x4000400040004000ull;
  CHECK(x86p_3dnow_eval(kX86pPMulHrw, a, b, &r));
  CHECK_EQ_U(r.q & 0xFFFFu, 0x1000u); /* (0x4000*0x4000 + 0x8000) >> 16 */
}

/* Denormal inputs read as a signed zero, and the sign survives. */
static void test_denormals_flush_to_zero(void) {
  X86pMm r;
  X86pMm den;
  den.q = 0;
  den.u[0] = 0x00000001u; /* smallest positive denormal */
  den.u[1] = 0x80000001u; /* smallest negative denormal */

  CHECK(x86p_pf_daz(1.0f) == 1.0f); /* a normal value is untouched */

  /* The flush keeps the sign, checked on the BIT PATTERN: -0.0f == 0.0f is
     true in C, so a value comparison here would pass on a flush that dropped
     the sign. */
  {
    float flushed = x86p_pf_daz(den.f[1]);
    uint32_t bits;
    memcpy(&bits, &flushed, sizeof bits);
    CHECK_EQ_U(bits, 0x80000000u);
    flushed = x86p_pf_daz(den.f[0]);
    memcpy(&bits, &flushed, sizeof bits);
    CHECK_EQ_U(bits, 0x00000000u);
  }

  /* And the flush reaches the operations: adding zero to a denormal yields a
     true zero, where an unflushed input would have returned the denormal.
     The RESULT sign is IEEE's, not the flush's -- (-0) + (+0) is +0 -- so
     this asserts the magnitude went away, and PFMUL below asserts the sign
     survived into an operation. */
  CHECK(x86p_3dnow_eval(kX86pPfAdd, den, mmf(0.0f, 0.0f), &r));
  CHECK_EQ_U(r.u[0], 0x00000000u);
  CHECK_EQ_U(r.u[1], 0x00000000u);

  CHECK(x86p_3dnow_eval(kX86pPfMul, den, mmf(1.0f, 1.0f), &r));
  CHECK_EQ_U(r.u[0], 0x00000000u); /* +denormal * 1 -> +0 */
  CHECK_EQ_U(r.u[1], 0x80000000u); /* -denormal * 1 -> -0, sign carried */
}

/*
 * THE NEGATIVE, and the reason this file exists. The five approximation
 * instructions must REFUSE and leave the destination untouched. A version that
 * computed 1.0f/x would agree to six digits, pass any tolerance-based test, and
 * be wrong in the low mantissa bits everywhere the game uses a reciprocal.
 */
static void test_approximations_are_refused_not_guessed(void) {
  const X86pPfOp refused[] = {kX86pPfRcp, kX86pPfRsqrt, kX86pPfRcpIt1, kX86pPfRcpIt2, kX86pPfRsqIt1};
  const int n = (int)(sizeof refused / sizeof refused[0]);
  int i, count = 0;
  for (i = 0; i < n; i++) {
    X86pMm r = mmf(12345.0f, 54321.0f);
    CHECK(!x86p_3dnow_implemented(refused[i]));
    if (!x86p_3dnow_eval(refused[i], mmf(4.0f, 16.0f), mmf(4.0f, 16.0f), &r) && r.f[0] == 12345.0f &&
        r.f[1] == 54321.0f) {
      count++;
    }
    /* ...and it is still nameable. "PFRCP, unimplemented" and "unknown
       instruction" are different facts about a run. */
    CHECK(strcmp(x86p_3dnow_name(refused[i]), "unknown") != 0);
  }
  CHECK_EQ_U(count, n); /* denominator: every refused opcode probed */
}

/* Nothing outside the enum is implemented, and eval refuses a null destination
   rather than writing through it. */
static void test_out_of_range_and_null(void) {
  X86pMm r;
  int i, refused = 0;
  const int probes = 8;
  for (i = 0; i < probes; i++) {
    if (!x86p_3dnow_implemented((X86pPfOp)((int)kX86pPfCount + i))) {
      refused++;
    }
  }
  CHECK_EQ_U(refused, probes);
  CHECK(!x86p_3dnow_eval((X86pPfOp)((int)kX86pPfCount + 1), mmf(1, 1), mmf(1, 1), &r));
  CHECK(!x86p_3dnow_eval(kX86pPfAdd, mmf(1, 1), mmf(1, 1), NULL));
}

/*
 * The coverage denominator. 19 of the 24 named opcodes are implemented and 5
 * are refused; if that ratio changes, the count printed by the engine's
 * coverage report changes with it and this is where it is asserted.
 */
static void test_coverage_denominator(void) {
  int i, implemented = 0;
  for (i = 0; i < (int)kX86pPfCount; i++) {
    if (x86p_3dnow_implemented((X86pPfOp)i)) {
      implemented++;
    }
  }
  CHECK_EQ_U((int)kX86pPfCount, 24);
  CHECK_EQ_U(implemented, 19);
}

int main(void) {
  RUN(test_names_round_trip);
  RUN(test_parse_rejects_what_it_should);
  RUN(test_arithmetic);
  RUN(test_horizontal);
  RUN(test_compares_write_masks);
  RUN(test_conversions_and_swap);
  RUN(test_packed_integer);
  RUN(test_denormals_flush_to_zero);
  RUN(test_approximations_are_refused_not_guessed);
  RUN(test_out_of_range_and_null);
  RUN(test_coverage_denominator);
  printf("%d check(s), %d failed, %d failing test(s)\n", g_checks, g_failed, g_test_failed);
  return g_test_failed ? 1 : 0;
}
