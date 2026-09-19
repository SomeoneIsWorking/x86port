/*
 * test_x87 -- the floating-point stack, against the host's own FPU.
 *
 * On an x86 host the real x87 unit is still there and reachable from inline
 * asm, and this suite uses it two different ways. Be clear about which is
 * which, because they prove different things:
 *
 *   test_hw_arithmetic       -- x86p_x87_arith against the instruction. On x86
 *                               the implementation EXECUTES that instruction,
 *                               so this cannot fail on the arithmetic; what it
 *                               checks is the routing around it (operand
 *                               order, the reverse flag, which ST(i), whether
 *                               the control word arrives). See the note above
 *                               it, and test_the_oracle_can_fail.
 *   test_portable_path_divergence
 *                            -- the portable reimplementation against the
 *                               instruction. This one is genuinely
 *                               independent, and it MEASURES the fidelity gap
 *                               a host without an x87 unit is left with.
 *
 * Where the host is not x86 there is no oracle at all, which is said loudly
 * rather than passing on the hermetic cases alone. The stack-discipline cases
 * still run there -- those are pointer arithmetic, not floating point, and
 * they are where most of the bugs actually are.
 */
#include "x87.h"

#include "x87_ext80_arith.h"

_Static_assert(sizeof(X86pX87Tag) == sizeof(unsigned int), "C ABI enum width");
_Static_assert(sizeof(X86pX87Op) == sizeof(unsigned int), "C ABI enum width");
_Static_assert(sizeof(X86pX87Insn) == sizeof(unsigned int), "C ABI enum width");
_Static_assert(sizeof(X86pX87Fn) == sizeof(unsigned int), "C ABI enum width");

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;
static int g_test_failed;

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
      printf("    FAIL %s:%d: %s: got %#llx want %#llx\n", __FILE__, __LINE__, #got, g_, w_);                          \
    }                                                                                                                  \
  } while (0)

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

/* ---- stack discipline: where the bugs are ------------------------------- */

/*
 * ST(0) IS A POSITION. After three pushes, ST(0) is the last one pushed and
 * ST(2) the first -- and TOP has moved, which an implementation that treats
 * ST(i) as register i gets right only while TOP is zero.
 */
static void test_st_is_a_position_not_a_register(void) {
  X86pX87 f;
  long double v;
  x86p_x87_reset(&f);
  CHECK_EQ_U(f.top, 0);
  CHECK_EQ_U(x86p_x87_depth(&f), 0);

  CHECK(x86p_x87_push(&f, 1.0L));
  CHECK_EQ_U(f.top, 7); /* TOP DECREMENTS on push */
  CHECK(x86p_x87_push(&f, 2.0L));
  CHECK(x86p_x87_push(&f, 3.0L));
  CHECK_EQ_U(f.top, 5);
  CHECK_EQ_U(x86p_x87_depth(&f), 3);

  CHECK(x86p_x87_get(&f, 0, &v) && v == 3.0L); /* last pushed */
  CHECK(x86p_x87_get(&f, 1, &v) && v == 2.0L);
  CHECK(x86p_x87_get(&f, 2, &v) && v == 1.0L);

  /* And the physical layout is what the positions say it is -- checked
     directly, because a get/set pair that are wrong the same way agree. The
     storage type is the host's, so this reads it through the one edge
     conversion rather than assuming which host it is. */
  CHECK(x86p_x87_reg_to_long_double(f.reg[5]) == 3.0L);
  CHECK(x86p_x87_reg_to_long_double(f.reg[6]) == 2.0L);
  CHECK(x86p_x87_reg_to_long_double(f.reg[7]) == 1.0L);

  CHECK(x86p_x87_pop(&f, &v) && v == 3.0L);
  CHECK_EQ_U(f.top, 6);
  CHECK(x86p_x87_get(&f, 0, &v) && v == 2.0L);
}

/* TOP is OBSERVABLE, in bits 11-13 of the status word. Omitting it is
   invisible until a guest switches on the whole word rather than on C0-C3. */
static void test_top_appears_in_the_status_word(void) {
  X86pX87 f;
  int i;
  x86p_x87_reset(&f);
  for (i = 0; i < 8; i++) {
    uint16_t sw = x86p_x87_status(&f);
    CHECK_EQ_U((sw >> X86P_X87_TOP_SHIFT) & 7u, f.top);
    if (i < 7) {
      CHECK(x86p_x87_push(&f, (long double)i));
    }
  }
}

static void test_clear_exceptions_preserves_unrelated_state(void) {
  X86pX87 f;
  X86pX87Reg registers[X86P_X87_REGS];
  uint8_t tags[X86P_X87_REGS];
  uint8_t top;
  uint16_t control;
  uint16_t status;
  int i;

  x86p_x87_reset(&f);
  for (i = 0; i < 3; i++) {
    CHECK(x86p_x87_push(&f, (long double)(i + 1)));
  }
  f.control = UINT16_C(0x0B7F);
  f.status = UINT16_C(0xFFFF);
  memcpy(registers, f.reg, sizeof registers);
  memcpy(tags, f.tag, sizeof tags);
  top = f.top;
  control = f.control;
  status = f.status;

  x86p_x87_clear_exceptions(&f);

  CHECK_EQ_U(f.status, status & (uint16_t)~X86P_X87_FNCLEX_MASK);
  CHECK_EQ_U(f.top, top);
  CHECK_EQ_U(f.control, control);
  CHECK(memcmp(f.tag, tags, sizeof tags) == 0);
  for (i = 0; i < X86P_X87_REGS; i++) {
    CHECK(x86p_x87_reg_to_long_double(f.reg[i]) == x86p_x87_reg_to_long_double(registers[i]));
  }
}

/* Overflow and underflow are REPORTED. Silently wrapping TOP produces an
   engine that drifts from hardware with no symptom for thousands of
   instructions. */
/*
 * The op census counts what a run performs, and its second column says what an
 * encoding-level inline path could have taken.
 *
 * The designed negatives are the point. An unarmed census must count nothing,
 * or an off switch that silently records would make every reading suspect; a
 * rounding mode the fast path refuses must not appear in the `ordinary`
 * column, or a run would report headroom no inline path could reach; and where
 * the storage is not the ext80 encoding the column is not measured at all,
 * which `ordinary_measured` says instead of a row of zeroes.
 */
static void test_the_op_census_counts_what_a_run_performs(void) {
  X86pX87OpCensus census;
  X86pX87 f;
  memset(&census, 0, sizeof census);

  /* Unarmed: the same work, and nothing recorded. */
  x86p_x87_reset(&f);
  CHECK(x86p_x87_push(&f, 3.0L));
  CHECK(x86p_x87_push(&f, 5.0L));
  CHECK(x86p_x87_arith(&f, kX86pX87Mul, 0, 7.0L, 0));
  CHECK_EQ_U(census.total[kX86pX87Mul], 0);
  CHECK_EQ_U(census.ordinary_measured, 0);

  x86p_x87_set_op_census(&f, &census);
  CHECK(x86p_x87_arith(&f, kX86pX87Mul, 0, 7.0L, 0));
  CHECK(x86p_x87_arith(&f, kX86pX87Add, 0, 1.0L, 0));
  CHECK(x86p_x87_arith(&f, kX86pX87Add, 0, 1.0L, 0));
  CHECK(x86p_x87_arith(&f, kX86pX87Div, 0, 2.0L, 0));
  CHECK_EQ_U(census.total[kX86pX87Mul], 1);
  CHECK_EQ_U(census.total[kX86pX87Add], 2);
  CHECK_EQ_U(census.total[kX86pX87Sub], 0);
  CHECK_EQ_U(census.total[kX86pX87Div], 1);

  if (census.ordinary_measured) {
    /* Four normals at the default control word: every one of them taken. */
    CHECK_EQ_U(census.taken[kX86pX87Mul], 1);
    CHECK_EQ_U(census.taken[kX86pX87Add], 2);
    /* A divide has no rule, so it is eligible and not taken -- which is the
       gap the census exists to show, and it must not read as a refusal. */
    CHECK_EQ_U(census.taken[kX86pX87Div], 0);
    CHECK_EQ_U(census.refused_control[kX86pX87Div], 0);
    CHECK_EQ_U(census.refused_other[kX86pX87Div], 0);

    /* Round-toward-negative is not the ordinary case: counted, not taken, and
       the refusal named as the control word's rather than an operand's. */
    f.control = (uint16_t)((f.control & ~(uint16_t)X86P_X87_RC_MASK) | (uint16_t)X86P_X87_RC_DOWN);
    CHECK(x86p_x87_arith(&f, kX86pX87Mul, 0, 3.0L, 0));
    CHECK_EQ_U(census.total[kX86pX87Mul], 2);
    CHECK_EQ_U(census.taken[kX86pX87Mul], 1);
    CHECK_EQ_U(census.refused_control[kX86pX87Mul], 1);

    /* A zero operand IS taken -- it was 44.6% of the game's route and the
       rules were extended for it, so a reading that fell back would show up
       here first. */
    f.control = (uint16_t)(f.control & ~(uint16_t)X86P_X87_RC_MASK);
    CHECK(x86p_x87_arith(&f, kX86pX87Mul, 0, 0.0L, 0));
    CHECK_EQ_U(census.total[kX86pX87Mul], 3);
    CHECK_EQ_U(census.taken[kX86pX87Mul], 2);
    CHECK_EQ_U(census.refused_other[kX86pX87Mul], 0);

    /* An infinity is where the softfloat earns its place. */
    CHECK(x86p_x87_push(&f, 1.0L));
    CHECK(x86p_x87_arith(&f, kX86pX87Div, 0, 0.0L, 0)); /* 1/0 -> +inf in ST(0) */
    CHECK(x86p_x87_arith(&f, kX86pX87Mul, 0, 2.0L, 0));
    CHECK_EQ_U(census.refused_other[kX86pX87Mul], 1);

    /* No operation escapes the accounting: taken plus both refusals can never
       exceed the total, and what is left is the named gap. */
    {
      unsigned op;
      for (op = 0; op < X86P_X87_OPS; op++) {
        CHECK(census.taken[op] + census.refused_control[op] + census.refused_other[op] <= census.total[op]);
      }
    }
  } else {
    printf("  NOTE: this host's register file is not the ext80 encoding, so "
           "the census counted operations and measured no headroom.\n");
  }

  /* Disarming stops it again. */
  x86p_x87_set_op_census(&f, NULL);
  CHECK(x86p_x87_arith(&f, kX86pX87Sub, 0, 1.0L, 0));
  CHECK_EQ_U(census.total[kX86pX87Sub], 0);
}

/*
 * The fused path, checked AGAINST the long one on the same operation.
 *
 * It exists to skip x86p_x87_arith_raw's bookkeeping, so the thing that can go
 * wrong is not the arithmetic -- the rules are differentially tested elsewhere
 * -- but the register file: the tag, and the six padding bytes the WASM
 * differential compares with memcmp. So this runs both paths from an identical
 * starting state and compares the WHOLE X86pX87, which is the comparison that
 * would catch a padding byte left behind.
 *
 * It also asserts that the fast path was TAKEN. A version that always returned
 * 0 would make every value comparison here pass while proving nothing.
 */
static void test_the_fused_arith_path_matches_the_long_one(void) {
  static const struct {
    long double a;
    long double b;
    X86pX87Op op;
    int reverse;
  } kCases[] = {
      {3.0L, 5.0L, kX86pX87Mul, 0},
      {3.0L, 5.0L, kX86pX87Sub, 1},
      {1.0L, 0.0L, kX86pX87Mul, 0},  /* a zero operand, which the rules take */
      {0.0L, 7.0L, kX86pX87Add, 0},  /* and which must leave tag = zero */
      {1.5L, -1.5L, kX86pX87Add, 0}, /* exact cancellation: the rules refuse */
      {1.0L, 3.0L, kX86pX87Div, 0},  /* no rule for a divide */
  };
  size_t i;
  int taken = 0;
  int refused = 0;
  for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    X86pX87 fast;
    X86pX87 slow;
    X86pExt80 src;
    x86p_x87_reset(&fast);
    CHECK(x86p_x87_push(&fast, kCases[i].b));
    CHECK(x86p_x87_push(&fast, kCases[i].a));
    slow = fast;
    if (!x86p_x87_ext80_of_st(&fast, 1, &src)) {
      /* No ext80 register file on this host; the entry point says so by
         refusing rather than by inventing an encoding. */
      printf("  NOTE: this host has no ext80 register file, so the fused "
             "path is not present to compare.\n");
      return;
    }
    if (x86p_x87_arith_ext80_fast(&fast, kCases[i].op, 0, src, kCases[i].reverse)) {
      taken++;
    } else {
      refused++;
      continue;
    }
    CHECK(x86p_x87_arith_raw(&slow, kCases[i].op, 0, x86p_x87_reg_from_long_double(kCases[i].b), kCases[i].reverse));
    /* Every byte, including the padding and the tag word. */
    CHECK(memcmp(&fast, &slow, sizeof fast) == 0);
  }
  /* Both answers, or this proves nothing: the divide and the cancellation must
     refuse, and the other four must be taken. */
  CHECK_EQ_U(taken, 4);
  CHECK_EQ_U(refused, 2);
}

static void test_stack_overflow_and_underflow_are_reported(void) {
  X86pX87 f;
  long double v;
  int i;
  x86p_x87_reset(&f);
  for (i = 0; i < 8; i++) {
    CHECK(x86p_x87_push(&f, (long double)i));
  }
  CHECK_EQ_U(x86p_x87_depth(&f), 8);
  /* The ninth push must fail, and set the fault bits. */
  CHECK(!x86p_x87_push(&f, 99.0L));
  CHECK((f.status & X86P_X87_SF) != 0);
  CHECK((f.status & X86P_X87_IE) != 0);
  CHECK((f.status & X86P_X87_C1) != 0); /* C1 set = overflow, not underflow */
  CHECK_EQ_U(x86p_x87_depth(&f), 8);    /* and nothing was overwritten */

  x86p_x87_reset(&f);
  CHECK(!x86p_x87_pop(&f, &v));
  CHECK((f.status & X86P_X87_SF) != 0);
  CHECK_EQ_U(f.status & X86P_X87_C1, 0); /* C1 clear = underflow */
}

/* Reading an empty register is a fact the engine reports, not a zero it
   carries forward into arithmetic. */
static void test_empty_register_is_not_zero(void) {
  X86pX87 f;
  long double v = 12345.0L;
  x86p_x87_reset(&f);
  CHECK(!x86p_x87_get(&f, 0, &v));
  CHECK(v == 12345.0L); /* untouched */
  CHECK(!x86p_x87_arith(&f, kX86pX87Add, 0, 1.0L, 0));
  CHECK((f.status & X86P_X87_SF) != 0);
}

/* Precision control rounds every RESULT, not just stores. */
static void test_precision_control_rounds_results(void) {
  X86pX87 f;
  long double v;
  /* A value that needs more than 24 mantissa bits: 1 + 2^-30. */
  const long double eps = ldexpl(1.0L, -30);

  x86p_x87_reset(&f);
  f.control = (uint16_t)((f.control & ~X86P_X87_PC_MASK) | X86P_X87_PC_EXTENDED);
  CHECK(x86p_x87_push(&f, 1.0L));
  CHECK(x86p_x87_arith(&f, kX86pX87Add, 0, eps, 0));
  CHECK(x86p_x87_get(&f, 0, &v));
  CHECK(v != 1.0L); /* extended precision keeps it */

  x86p_x87_reset(&f);
  f.control = (uint16_t)((f.control & ~X86P_X87_PC_MASK) | X86P_X87_PC_SINGLE);
  CHECK(x86p_x87_push(&f, 1.0L));
  CHECK(x86p_x87_arith(&f, kX86pX87Add, 0, eps, 0));
  CHECK(x86p_x87_get(&f, 0, &v));
  CHECK(v == 1.0L); /* single precision loses it -- which is the point */
}

/* FIST ROUNDS; it truncates only when the control word says to. Assuming
   truncation is the classic x87 porting bug, and it is right for every value
   that is already an integer. */
static void test_fist_rounds_by_the_control_word(void) {
  X86pX87 f;
  int64_t n;
  x86p_x87_reset(&f);

  CHECK(x86p_x87_to_int(&f, 2.5L, 4, &n));
  CHECK_EQ_U(n, 2); /* nearest, ties to EVEN -- not 3 */
  CHECK(x86p_x87_to_int(&f, 3.5L, 4, &n));
  CHECK_EQ_U(n, 4);
  CHECK(x86p_x87_to_int(&f, 2.6L, 4, &n));
  CHECK_EQ_U(n, 3);
  CHECK(x86p_x87_to_int(&f, -2.5L, 4, &n));
  CHECK_EQ_U((unsigned long long)(n + 100), 98); /* -2, ties to even */

  f.control = (uint16_t)((f.control & ~X86P_X87_RC_MASK) | X86P_X87_RC_TRUNCATE);
  CHECK(x86p_x87_to_int(&f, 2.9L, 4, &n));
  CHECK_EQ_U(n, 2);
  f.control = (uint16_t)((f.control & ~X86P_X87_RC_MASK) | X86P_X87_RC_DOWN);
  CHECK(x86p_x87_to_int(&f, -2.1L, 4, &n));
  CHECK_EQ_U((unsigned long long)(n + 100), 97); /* -3 */
  f.control = (uint16_t)((f.control & ~X86P_X87_RC_MASK) | X86P_X87_RC_UP);
  CHECK(x86p_x87_to_int(&f, 2.1L, 4, &n));
  CHECK_EQ_U(n, 3);

  /* Out of range is the guest-visible invalid operation, not a wrapped value. */
  x86p_x87_reset(&f);
  n = 777;
  CHECK(!x86p_x87_to_int(&f, 1.0e30L, 4, &n));
  CHECK_EQ_U(n, 777);
  CHECK(!x86p_x87_to_int(&f, (long double)NAN, 4, &n));
  CHECK(!x86p_x87_to_int(&f, 40000.0L, 2, &n)); /* fits 32 bits, not 16 */
  CHECK(x86p_x87_to_int(&f, 40000.0L, 4, &n));
}

/* The 80-bit round trip, including the values that are only representable
   there. */
static void test_f80_round_trip(void) {
  static const long double vals[] = {0.0L, 1.0L, -1.0L, 0.5L, 3.14159265358979323846L, 1.0e300L, -1.0e-300L, 65535.5L};
  const int n = (int)(sizeof vals / sizeof vals[0]);
  int i;
  for (i = 0; i < n; i++) {
    uint8_t b[10];
    long double back;
    memset(b, 0xAA, sizeof b);
    x86p_x87_to_f80(vals[i], b);
    back = x86p_x87_from_f80(b);
    CHECK(back == vals[i]);
  }
  /* And the layout is asserted, not just round-tripped: 1.0 is exponent
     0x3FFF with the explicit leading bit set and nothing else. A reader and
     writer that are wrong the same way round-trip perfectly. */
  {
    uint8_t b[10];
    x86p_x87_to_f80(1.0L, b);
    CHECK_EQ_U(b[7], 0x80u); /* the explicit integer bit, the format's oddity */
    CHECK_EQ_U(b[8], 0xFFu);
    CHECK_EQ_U(b[9], 0x3Fu);
  }
}

/* ---- against the host FPU ----------------------------------------------- */

#if (defined(__x86_64__) || defined(__i386__)) && LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define HAVE_HW_ORACLE 1

/*
 * Each helper takes the CONTROL WORD as an argument and loads it into the real
 * FPU, because precision control is the part of this module most likely to be
 * wrong and it is invisible to an oracle that always runs at the default
 * setting. The previous control word is restored, since clobbering the
 * process's FPU state would corrupt every test after this one.
 */
#define HW_ARITH(name, insn)                                                                                           \
  static long double name(long double a, long double b, uint16_t cw) {                                                 \
    long double r;                                                                                                     \
    uint16_t saved;                                                                                                    \
    __asm__ volatile("fnstcw %0\n\tfldcw %4\n\tfldt %3\n\tfldt %2\n\t" insn " %%st(1), %%st\n\t"                       \
                     "fstpt %1\n\tfstp %%st(0)\n\tfldcw %0"                                                            \
                     : "=m"(saved), "=m"(r)                                                                            \
                     : "m"(a), "m"(b), "m"(cw)                                                                         \
                     : "st", "st(1)", "memory");                                                                       \
    return r;                                                                                                          \
  }

HW_ARITH(hw_add, "fadd")
HW_ARITH(hw_sub, "fsub")
HW_ARITH(hw_mul, "fmul")
HW_ARITH(hw_div, "fdiv")
HW_ARITH(hw_subr, "fsubr")
HW_ARITH(hw_divr, "fdivr")

/* FCOM against the real unit, reading the real C0/C2/C3 out of the real status
   word. The comparison flags are what every float branch in the game turns
   on. Comparison does not round, so this one needs no control word. */
static uint16_t hw_compare(long double a, long double b) {
  uint16_t sw;
  __asm__ volatile("fldt %2\n\tfldt %1\n\tfcompp\n\tfnstsw %0" : "=m"(sw) : "m"(a), "m"(b) : "st", "st(1)", "memory");
  return sw;
}

/* FST m32 / FST m64 on the real unit, under the guest control word: the store
   narrows the significand per the RC field exactly as FADD narrows a result. */
static uint32_t hw_narrow32(long double v, uint16_t cw) {
  uint16_t saved;
  float out;
  uint32_t bits;
  __asm__ volatile("fnstcw %0\n\tfldcw %3\n\tfldt %2\n\tfstps %1\n\tfldcw %0"
                   : "=m"(saved), "=m"(out)
                   : "m"(v), "m"(cw)
                   : "st", "memory");
  memcpy(&bits, &out, sizeof bits);
  return bits;
}

static uint64_t hw_narrow64(long double v, uint16_t cw) {
  uint16_t saved;
  double out;
  uint64_t bits;
  __asm__ volatile("fnstcw %0\n\tfldcw %3\n\tfldt %2\n\tfstpl %1\n\tfldcw %0"
                   : "=m"(saved), "=m"(out)
                   : "m"(v), "m"(cw)
                   : "st", "memory");
  memcpy(&bits, &out, sizeof bits);
  return bits;
}

/*
 * The value corpus. The named values are the ones with special behaviour; the
 * rest are generated, because the interesting failures in floating point are
 * the values that need every mantissa bit, and a hand-written list of round
 * numbers never contains one. Deterministic: the same sweep every run, so a
 * mismatch is reproducible.
 */
#define NVALS 256
static long double g_vals[NVALS];

static void build_vals(void) {
  static const long double named[] = {
      0.0L,
      -0.0L,
      1.0L,
      -1.0L,
      0.5L,
      -0.5L,
      2.0L,
      3.0L,
      7.0L,
      0.1L,
      1.0e10L,
      -1.0e10L,
      1.0e-10L,
      3.14159265358979323846L,
      1.0e300L,
      -1.0e-300L,
      65535.5L,
      LDBL_MAX,
      -LDBL_MAX,
      LDBL_MIN,
      LDBL_EPSILON,
      (long double)INFINITY,
      -(long double)INFINITY,
      (long double)NAN,
      /* denormal, and the smallest step above 1.0 -- the two places a
         precision-control bug shows up first */
      LDBL_MIN / 4096.0L,
      1.0L + LDBL_EPSILON,
  };
  const int nnamed = (int)(sizeof named / sizeof named[0]);
  uint64_t s = 0x9E3779B97F4A7C15ull; /* fixed seed: the sweep is reproducible */
  int i;
  for (i = 0; i < nnamed && i < NVALS; i++) {
    g_vals[i] = named[i];
  }
  for (; i < NVALS; i++) {
    uint64_t m;
    int e;
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    m = s >> 11; /* 53 significant bits, then scaled to need more */
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    e = (int)(s >> 56) % 120 - 60; /* exponents spanning 1e-18 .. 1e18 */
    g_vals[i] = ldexpl((long double)m, e - 53);
    if (s & 1) {
      g_vals[i] = -g_vals[i];
    }
  }
}

/* The control words swept: every precision-control setting crossed with every
   rounding-control setting would be twelve, but PC and RC interact only
   through the rounding of the narrowed result, so the diagonal plus the
   defaults covers the interaction without a twelvefold sweep. */
static const struct {
  const char *name;
  uint16_t cw;
} kControls[] = {
    {"PC=ext RC=near", (uint16_t)(0x0300u | 0x007Fu | X86P_X87_RC_NEAREST)},
    {"PC=dbl RC=near", (uint16_t)(0x0200u | 0x007Fu | X86P_X87_RC_NEAREST)},
    {"PC=sgl RC=near", (uint16_t)(0x0000u | 0x007Fu | X86P_X87_RC_NEAREST)},
    {"PC=sgl RC=trunc", (uint16_t)(0x0000u | 0x007Fu | X86P_X87_RC_TRUNCATE)},
    {"PC=sgl RC=down", (uint16_t)(0x0000u | 0x007Fu | X86P_X87_RC_DOWN)},
    {"PC=dbl RC=up", (uint16_t)(0x0200u | 0x007Fu | X86P_X87_RC_UP)},
};
#define NCONTROLS ((int)(sizeof kControls / sizeof kControls[0]))

static void test_hw_arithmetic(void) {
  struct {
    const char *name;
    X86pX87Op op;
    int reverse;
    long double (*hw)(long double, long double, uint16_t);
  } ops[] = {
      {"FADD", kX86pX87Add, 0, hw_add},
      {"FSUB", kX86pX87Sub, 0, hw_sub},
      {"FMUL", kX86pX87Mul, 0, hw_mul},
      {"FDIV", kX86pX87Div, 0, hw_div},
      {"FSUBR", kX86pX87Sub, 1, hw_subr},
      {"FDIVR", kX86pX87Div, 1, hw_divr},
  };
  const int nops = (int)(sizeof ops / sizeof ops[0]);
  int k, c, i, j;
  unsigned long total = 0, bad = 0;

  for (c = 0; c < NCONTROLS; c++) {
    unsigned long cases = 0, mismatch = 0;
    for (k = 0; k < nops; k++) {
      for (i = 0; i < NVALS; i++) {
        for (j = 0; j < NVALS; j++) {
          X86pX87 f;
          long double us, them;
          x86p_x87_reset(&f);
          f.control = kControls[c].cw;
          if (!x86p_x87_push(&f, g_vals[i]) || !x86p_x87_arith(&f, ops[k].op, 0, g_vals[j], ops[k].reverse) ||
              !x86p_x87_get(&f, 0, &us)) {
            CHECK(0);
            continue;
          }
          /* The hardware helper computes st(0) OP st(1) with st(0) = a, so the
             operands go in the same order the model sees them; the reverse
             forms are the reversing instruction itself, not a swap here. */
          them = ops[k].hw(g_vals[i], g_vals[j], kControls[c].cw);
          cases++;
          /* Bit-exact, or both NaN. "Close enough" is not a claim a reference
             implementation gets to make. */
          if (!(us == them || (isnan(us) && isnan(them)))) {
            mismatch++;
            if (mismatch <= 3) {
              printf("    MISMATCH %s %s(%.21Lg, %.21Lg): hw=%.21Lg us=%.21Lg\n",
                     kControls[c].name,
                     ops[k].name,
                     g_vals[i],
                     g_vals[j],
                     them,
                     us);
            }
          }
        }
      }
    }
    printf("    %-16s %8lu case(s), %lu mismatch(es)\n", kControls[c].name, cases, mismatch);
    total += cases;
    bad += mismatch;
    g_checks++;
    if (mismatch) {
      g_failed++;
    }
  }
  printf("    TOTAL %lu comparison(s) against the host FPU, %lu mismatch(es)\n", total, bad);
  CHECK_EQ_U(total, (unsigned long)nops * NVALS * NVALS * NCONTROLS);
  CHECK_EQ_U(bad, 0u);
}

/*
 * WHAT THE SWEEP ABOVE DOES AND DOES NOT PROVE, stated because it would
 * otherwise be read as more than it is.
 *
 * On an x86 host x86p_x87_arith executes the real instruction under the
 * guest's control word, so the sweep compares that against the same
 * instruction -- it CANNOT fail on the arithmetic, and a "0 mismatches" there
 * is not evidence the arithmetic is right. It is not useless: it still
 * exercises the routing, and the routing is where the bugs are. Operand order,
 * the reverse flag, which ST(i) is read and written, and whether the control
 * word reaches the unit at all are all live, and getting any of them wrong
 * makes the sweep fail -- which is exactly what test_the_oracle_can_fail
 * demonstrates by getting one of them wrong on purpose.
 *
 * The genuinely independent measurement is below: the PORTABLE path, which is
 * a real reimplementation, against the hardware.
 */
static void test_portable_path_divergence(void) {
  static const X86pX87Op ops[] = {kX86pX87Add, kX86pX87Sub, kX86pX87Mul, kX86pX87Div};
  static long double (*const hw[])(long double, long double, uint16_t) = {hw_add, hw_sub, hw_mul, hw_div};
  const int nops = 4;
  int c, k, i, j;

  for (c = 0; c < NCONTROLS; c++) {
    unsigned long cases = 0, mismatch = 0;
    for (k = 0; k < nops; k++) {
      for (i = 0; i < NVALS; i++) {
        for (j = 0; j < NVALS; j++) {
          long double us = x86p_x87_arith_portable(kControls[c].cw, ops[k], g_vals[i], g_vals[j]);
          long double them = hw[k](g_vals[i], g_vals[j], kControls[c].cw);
          cases++;
          if (!(us == them || (isnan(us) && isnan(them)))) {
            mismatch++;
          }
        }
      }
    }
    printf("    %-16s %8lu case(s), %lu differ (%.3f%%)\n",
           kControls[c].name,
           cases,
           mismatch,
           100.0 * (double)mismatch / (double)cases);
    /* At EXTENDED precision the portable path rounds once too, so it must be
       exact. This is the assertion; the narrowed rows are a measurement. */
    if ((kControls[c].cw & X86P_X87_PC_MASK) == X86P_X87_PC_EXTENDED) {
      CHECK_EQ_U(mismatch, 0u);
    }
  }
  printf("    ^ the non-zero rows are the fidelity gap on a host with no x87 unit\n"
         "      (double rounding). They are a MEASUREMENT, not a failure: on such a\n"
         "      host x86p_x87_precision_is_exact() reports false and the port must\n"
         "      say so rather than claim parity.\n");
}

static void test_hw_compare_flags(void) {
  int i, j;
  unsigned long cases = 0, mismatch = 0;
  const uint16_t mask = X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
  for (i = 0; i < NVALS; i++) {
    for (j = 0; j < NVALS; j++) {
      X86pX87 f;
      uint16_t us, them;
      x86p_x87_reset(&f);
      CHECK(x86p_x87_push(&f, g_vals[i]));
      CHECK(x86p_x87_compare(&f, g_vals[j]));
      us = (uint16_t)(x86p_x87_status(&f) & mask);
      them = (uint16_t)(hw_compare(g_vals[i], g_vals[j]) & mask);
      cases++;
      if (us != them) {
        mismatch++;
        if (mismatch <= 3) {
          printf("    MISMATCH FCOM(%.20Lg, %.20Lg): hw=%04x us=%04x\n", g_vals[i], g_vals[j], them, us);
        }
      }
    }
  }
  printf("    FCOM   %4lu case(s), %lu mismatch(es) on C0/C2/C3\n", cases, mismatch);
  CHECK_EQ_U(cases, (unsigned long)(NVALS * NVALS));
  CHECK_EQ_U(mismatch, 0u);

  /* NaN is UNORDERED: all three set, a distinct outcome from equal and from
     less-than, and guests branch on it. */
  {
    X86pX87 f;
    x86p_x87_reset(&f);
    CHECK(x86p_x87_push(&f, (long double)NAN));
    CHECK(x86p_x87_compare(&f, 1.0L));
    CHECK_EQ_U(x86p_x87_status(&f) & mask, mask);
    CHECK_EQ_U(hw_compare((long double)NAN, 1.0L) & mask, mask);
  }
}

/*
 * FST m32 / FST m64 must round the stored value by the control word's RC field,
 * not always to nearest. `x86p_x87_to_int` already does this; the float stores
 * were a bare `(float)v` cast until this was fixed.
 *
 * Two checks. First, hand-computed anchors on a value exactly halfway between
 * two floats (1 + 1/2 ulp): nearest-even keeps the even mantissa, and up/down/
 * truncate each go their own way with the sign flipping which of up/down rounds
 * away. These do not depend on the host FPU. Second, the routing sweep against
 * the real unit: like test_hw_arithmetic it cannot fail on the rounding itself
 * on an x86 host, but it fails loudly if the control word never reaches the
 * narrowing or the width is picked wrong.
 */
static void test_fst_rounds_by_the_control_word(void) {
  const long double up = 1.0L + ldexpl(1.0L, -24); /* exactly between 1.0f and nextafterf(1.0f,2) */
  const uint32_t one = 0x3F800000u;                /* 1.0f */
  const uint32_t one_plus = 0x3F800001u;           /* nextafterf(1.0f, 2) */
  const uint16_t base = (uint16_t)(0x007Fu | X86P_X87_PC_EXTENDED);
  X86pX87 f;
  int c, i;
  unsigned long cases = 0, mismatch = 0;

  x86p_x87_reset(&f);

  f.control = (uint16_t)((base & ~X86P_X87_RC_MASK) | X86P_X87_RC_NEAREST);
  CHECK_EQ_U(x86p_x87_to_f32(&f, up), one); /* ties to even */
  CHECK_EQ_U(x86p_x87_to_f32(&f, -up), one | 0x80000000u);
  f.control = (uint16_t)((base & ~X86P_X87_RC_MASK) | X86P_X87_RC_UP);
  CHECK_EQ_U(x86p_x87_to_f32(&f, up), one_plus);
  CHECK_EQ_U(x86p_x87_to_f32(&f, -up), one | 0x80000000u); /* toward +inf */
  f.control = (uint16_t)((base & ~X86P_X87_RC_MASK) | X86P_X87_RC_DOWN);
  CHECK_EQ_U(x86p_x87_to_f32(&f, up), one);
  CHECK_EQ_U(x86p_x87_to_f32(&f, -up), one_plus | 0x80000000u); /* toward -inf */
  f.control = (uint16_t)((base & ~X86P_X87_RC_MASK) | X86P_X87_RC_TRUNCATE);
  CHECK_EQ_U(x86p_x87_to_f32(&f, up), one);
  CHECK_EQ_U(x86p_x87_to_f32(&f, -up), one | 0x80000000u);

  for (c = 0; c < NCONTROLS; c++) {
    for (i = 0; i < NVALS; i++) {
      x86p_x87_reset(&f);
      f.control = kControls[c].cw;
      cases += 2;
      if (x86p_x87_to_f32(&f, g_vals[i]) != hw_narrow32(g_vals[i], kControls[c].cw)) {
        mismatch++;
      }
      if (x86p_x87_to_f64(&f, g_vals[i]) != hw_narrow64(g_vals[i], kControls[c].cw)) {
        mismatch++;
      }
    }
  }
  printf("    FST m32/m64  %6lu store(s) vs the host unit, %lu mismatch(es)\n", cases, mismatch);
  g_checks++;
  if (mismatch) {
    g_failed++;
  }
  CHECK_EQ_U(mismatch, 0u);
}

/* The oracle must be shown lying: FSUB modelled without the reverse flag. */
static void test_the_oracle_can_fail(void) {
  int i, j;
  unsigned long caught = 0, compared = 0;
  for (i = 0; i < NVALS; i++) {
    for (j = 0; j < NVALS; j++) {
      X86pX87 f;
      long double us, them;
      x86p_x87_reset(&f);
      x86p_x87_push(&f, g_vals[i]);
      x86p_x87_arith(&f, kX86pX87Sub, 0, g_vals[j], 0); /* forward */
      x86p_x87_get(&f, 0, &us);
      them = hw_subr(g_vals[i], g_vals[j], X86P_X87_CW_INIT); /* reverse */
      compared++;
      if (!(us == them || (isnan(us) && isnan(them)))) {
        caught++;
      }
    }
  }
  printf("    deliberate defect (FSUB where FSUBR was meant): %lu/%lu caught\n", caught, compared);
  CHECK(caught > (unsigned long)(NVALS * NVALS) / 2);
}
#else
#define HAVE_HW_ORACLE 0
#endif

int main(void) {
  /* Stated up front, because every arithmetic number below is only a parity
     claim when this holds. */
  printf("host long double is x87 extended (64-bit mantissa): %s\n", x86p_x87_precision_is_exact() ? "yes" : "NO");
  RUN(test_st_is_a_position_not_a_register);
  RUN(test_top_appears_in_the_status_word);
  RUN(test_clear_exceptions_preserves_unrelated_state);
  RUN(test_stack_overflow_and_underflow_are_reported);
  RUN(test_the_op_census_counts_what_a_run_performs);
  RUN(test_the_fused_arith_path_matches_the_long_one);
  RUN(test_empty_register_is_not_zero);
  RUN(test_precision_control_rounds_results);
  RUN(test_fist_rounds_by_the_control_word);
  if (x86p_x87_precision_is_exact()) {
    RUN(test_f80_round_trip);
  } else {
    printf("test f80_round_trip\n  SKIP -- host long double cannot represent "
           "the x87 80-bit format exactly; the software conversion is "
           "covered by test_x87_binary128 and test_wasm_x87.\n");
  }
#if HAVE_HW_ORACLE
  build_vals();
  if (!x86p_x87_precision_is_exact()) {
    /* An x86 host whose long double is not extended would make every
       comparison below meaningless. Fail rather than skip: on this
       architecture it should not be possible, so it is a defect, not a
       platform limitation. */
    printf("  FAIL: x86 host but long double is not the extended format\n");
    g_failed++;
    g_test_failed++;
  }
  RUN(test_hw_arithmetic);
  RUN(test_portable_path_divergence);
  RUN(test_hw_compare_flags);
  RUN(test_fst_rounds_by_the_control_word);
  RUN(test_the_oracle_can_fail);
#else
  printf("test hardware_oracle\n  SKIP -- host is not x86, so no x87 result or "
         "comparison flag was checked against a real FPU. On such a host the\n"
         "  80-bit arithmetic here is an APPROXIMATION of the guest's (see\n"
         "  x86p_x87_precision_is_exact); the stack cases above still hold.\n");
#endif
  printf("%d check(s), %d failed, %d failing test(s)%s\n",
         g_checks,
         g_failed,
         g_test_failed,
         HAVE_HW_ORACLE ? "" : "  [NO FPU ORACLE]");
  return g_test_failed ? 1 : 0;
}
