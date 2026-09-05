/*
 * test_flags -- the flag model against REAL HARDWARE.
 *
 * A flag model can only be checked against something that already knows the
 * answer, and reasoning about the manual is not that: every flag bug in a
 * translator is a case someone reasoned through and got wrong. So where the
 * host is x86 this test EXECUTES the instruction -- with a controlled incoming
 * EFLAGS -- captures the real EFLAGS, and compares every architecturally
 * defined bit. Undefined bits are still measured and reported, but cannot be
 * a portable pass/fail contract because x86 implementations may leave
 * different values there.
 *
 * It is exhaustive at byte width: all 256x256 operand pairs, both carry-in
 * values, for every operation modelled. 8 bits is small enough to enumerate and
 * wide enough to contain every interesting case -- the sign boundary, the carry
 * boundary, the nibble boundary AF turns on, and zero.
 *
 * ON A NON-x86 HOST there is no oracle, and this says so loudly and skips,
 * rather than passing quietly on the hermetic cases and leaving a reader to
 * believe hardware agreement was demonstrated.
 */
#include "flags.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

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
      printf("    FAIL %s:%d: %s: got %llu want %llu\n", __FILE__, __LINE__, #got, g_, w_);                            \
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

/* ------------------------------------------------------------------------- */
/* Hermetic cases: true on every host, oracle or not.                          */
/* ------------------------------------------------------------------------- */

static void set(X86pFlags *f, X86pFlagKind k, uint32_t a, uint32_t b, uint32_t r, int w) {
  x86p_flags_set(f, k, a, b, r, w);
}

static void test_width_mask_refuses_bad_widths(void) {
  CHECK_EQ_U(x86p_width_mask(1), 0xFFu);
  CHECK_EQ_U(x86p_width_mask(2), 0xFFFFu);
  CHECK_EQ_U(x86p_width_mask(4), 0xFFFFFFFFu);
  /* NOT 0xFFFFFFFF: a bad width that silently means "32 bits" produces flags
     that are plausible for the wrong operand size. */
  CHECK_EQ_U(x86p_width_mask(0), 0u);
  CHECK_EQ_U(x86p_width_mask(3), 0u);
  CHECK_EQ_U(x86p_width_mask(8), 0u);
  CHECK_EQ_U(x86p_width_mask(-1), 0u);
}

/* Width is part of the answer, not decoration. The same bit pattern is
   negative at byte width and positive at dword width. */
static void test_width_changes_the_answer(void) {
  X86pFlags f;
  memset(&f, 0, sizeof f);
  set(&f, kX86pFlagsLogic, 0, 0, 0x80u, 1);
  CHECK_EQ_U(x86p_flag_sf(&f), 1);
  CHECK_EQ_U(x86p_flag_zf(&f), 0);
  set(&f, kX86pFlagsLogic, 0, 0, 0x80u, 4);
  CHECK_EQ_U(x86p_flag_sf(&f), 0);
  set(&f, kX86pFlagsLogic, 0, 0, 0xFFFFFF00u, 1);
  CHECK_EQ_U(x86p_flag_zf(&f), 1); /* the byte really is zero */
  set(&f, kX86pFlagsLogic, 0, 0, 0xFFFFFF00u, 4);
  CHECK_EQ_U(x86p_flag_zf(&f), 0);
}

/* The bug this model exists to prevent: INC must not clear CF. */
static void test_inc_dec_preserve_carry(void) {
  X86pFlags f;
  memset(&f, 0, sizeof f);
  set(&f, kX86pFlagsAdd, 0xFFu, 0x01u, 0x00u, 1); /* carry out */
  CHECK_EQ_U(x86p_flag_cf(&f), 1);
  set(&f, kX86pFlagsInc, 0x10u, 1u, 0x11u, 1);
  CHECK_EQ_U(x86p_flag_cf(&f), 1); /* still set -- INC does not write CF */
  set(&f, kX86pFlagsDec, 0x11u, 1u, 0x10u, 1);
  CHECK_EQ_U(x86p_flag_cf(&f), 1);
  /* and a real carry-writing op clears it again */
  set(&f, kX86pFlagsAdd, 0x01u, 0x01u, 0x02u, 1);
  CHECK_EQ_U(x86p_flag_cf(&f), 0);
  set(&f, kX86pFlagsInc, 0x10u, 1u, 0x11u, 1);
  CHECK_EQ_U(x86p_flag_cf(&f), 0);
}

/*
 * A masked shift count of zero writes NO flags, and the rule belongs to the
 * instruction: the caller does not record a flag update at all. Recording one
 * is refused, because there is no correct value for four of the six flags and
 * the naive CF formula shifts by -1 -- undefined behaviour whose result on one
 * host is not a specification.
 *
 * Tested in a child process, since the refusal aborts. A refusal nobody
 * executes is a comment.
 */
static void test_shift_by_zero_is_refused(void) {
#if defined(__unix__) || defined(__APPLE__)
  pid_t pid = fork();
  if (pid == 0) {
    X86pFlags f;
    memset(&f, 0, sizeof f);
    fclose(stderr); /* the refusal text is expected; do not clutter the run */
    x86p_flags_set(&f, kX86pFlagsShl, 0x12u, 0u, 0x12u, 1);
    _exit(0); /* reached only if the refusal did NOT happen */
  }
  CHECK(pid > 0);
  if (pid > 0) {
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    /* Must have died, and not by returning 0 as though the call were fine. */
    CHECK(!WIFEXITED(status) || WEXITSTATUS(status) != 0);
  }
#else
  printf("    (no fork on this host: the shift-by-zero refusal was not executed)\n");
#endif
  /* And the contract the refusal protects: a correct caller skips the update,
     so every flag survives untouched. */
  {
    X86pFlags f;
    uint32_t before;
    memset(&f, 0, sizeof f);
    set(&f, kX86pFlagsSub, 0x00u, 0x01u, 0xFFu, 1); /* CF, SF, AF, PF set */
    before = x86p_eflags(&f);
    /* ... a SHL by 0 happens here, recording nothing ... */
    CHECK_EQ_U(x86p_eflags(&f), before);
  }
}

/* An explicit word (POPFD) must round-trip every flag it carries. */
static void test_explicit_round_trip(void) {
  X86pFlags f;
  uint32_t all = X86P_EFLAGS_FIXED | X86P_ARITH_FLAGS;
  memset(&f, 0, sizeof f);
  x86p_flags_set_explicit(&f, all);
  CHECK_EQ_U(x86p_flag_cf(&f), 1);
  CHECK_EQ_U(x86p_flag_pf(&f), 1);
  CHECK_EQ_U(x86p_flag_af(&f), 1);
  CHECK_EQ_U(x86p_flag_zf(&f), 1);
  CHECK_EQ_U(x86p_flag_sf(&f), 1);
  CHECK_EQ_U(x86p_flag_of(&f), 1);
  CHECK_EQ_U(x86p_eflags(&f) & (X86P_ARITH_FLAGS | X86P_EFLAGS_FIXED), all);

  x86p_flags_set_explicit(&f, X86P_EFLAGS_FIXED);
  CHECK_EQ_U(x86p_eflags(&f) & X86P_ARITH_FLAGS, 0u);
  /* And the preserved copies must AGREE with the word, or the next INC would
     restore a carry the POPFD just cleared. */
  set(&f, kX86pFlagsInc, 0x10u, 1u, 0x11u, 1);
  CHECK_EQ_U(x86p_flag_cf(&f), 0);
}

/* PF is the parity of the LOW BYTE only, whatever the width. Reads like a bug,
   is architectural. */
static void test_parity_is_low_byte_only(void) {
  X86pFlags f;
  memset(&f, 0, sizeof f);
  set(&f, kX86pFlagsLogic, 0, 0, 0x0000FF03u, 4);
  CHECK_EQ_U(x86p_flag_pf(&f), 1); /* 0x03 has two bits: even */
  set(&f, kX86pFlagsLogic, 0, 0, 0x0000FF07u, 4);
  CHECK_EQ_U(x86p_flag_pf(&f), 0); /* 0x07 has three: odd */
}

/* Nothing has run yet: every flag reads 0, and nothing crashes on a null. */
static void test_none_and_null(void) {
  X86pFlags f;
  memset(&f, 0, sizeof f);
  CHECK_EQ_U(f.kind, (uint8_t)kX86pFlagsNone);
  CHECK_EQ_U(x86p_eflags(&f) & X86P_ARITH_FLAGS, 0u);
  CHECK_EQ_U(x86p_flag_cf(NULL), 0);
  CHECK_EQ_U(x86p_flag_of(NULL), 0);
  CHECK_EQ_U(x86p_eflags(NULL), X86P_EFLAGS_FIXED);
}

/* ------------------------------------------------------------------------- */
/* The hardware oracle.                                                        */
/* ------------------------------------------------------------------------- */

#if (defined(__x86_64__) || defined(__i386__)) && !defined(_WIN32)
#define HAVE_HW_ORACLE 1

/* Only the arithmetic flags are settable going in; the rest of the word is
   left at its architectural fixed value so POPF cannot change anything else. */
static uint64_t hw_in(uint32_t f) {
  return (uint64_t)((f & X86P_ARITH_FLAGS) | X86P_EFLAGS_FIXED);
}

#define HW_BINOP8(fname, insn)                                                                                         \
  static uint32_t fname(uint8_t a, uint8_t b, uint32_t fin, uint8_t *out) {                                            \
    uint64_t fout;                                                                                                     \
    uint8_t r = a;                                                                                                     \
    __asm__ volatile("push %[fi]\n\t"                                                                                  \
                     "popf\n\t" insn " %[bb], %[rr]\n\t"                                                               \
                     "pushf\n\t"                                                                                       \
                     "pop %[fo]\n\t"                                                                                   \
                     : [rr] "+q"(r), [fo] "=r"(fout)                                                                   \
                     : [bb] "q"(b), [fi] "r"(hw_in(fin))                                                               \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fout;                                                                                             \
  }

HW_BINOP8(hw_add8, "addb")
HW_BINOP8(hw_sub8, "subb")
HW_BINOP8(hw_and8, "andb")
HW_BINOP8(hw_or8, "orb")
HW_BINOP8(hw_xor8, "xorb")
HW_BINOP8(hw_adc8, "adcb")
HW_BINOP8(hw_sbb8, "sbbb")
HW_BINOP8(hw_cmp8, "cmpb")

#define HW_UNOP8(fname, insn)                                                                                          \
  static uint32_t fname(uint8_t a, uint32_t fin, uint8_t *out) {                                                       \
    uint64_t fout;                                                                                                     \
    uint8_t r = a;                                                                                                     \
    __asm__ volatile("push %[fi]\n\t"                                                                                  \
                     "popf\n\t" insn " %[rr]\n\t"                                                                      \
                     "pushf\n\t"                                                                                       \
                     "pop %[fo]\n\t"                                                                                   \
                     : [rr] "+q"(r), [fo] "=r"(fout)                                                                   \
                     : [fi] "r"(hw_in(fin))                                                                            \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fout;                                                                                             \
  }

HW_UNOP8(hw_inc8, "incb")
HW_UNOP8(hw_dec8, "decb")

/* Shifts take the count in CL, so the count cannot be an immediate here. */
#define HW_SHIFT8(fname, insn)                                                                                         \
  static uint32_t fname(uint8_t a, uint8_t count, uint32_t fin, uint8_t *out) {                                        \
    uint64_t fout;                                                                                                     \
    uint8_t r = a;                                                                                                     \
    __asm__ volatile("push %[fi]\n\t"                                                                                  \
                     "popf\n\t" insn " %%cl, %[rr]\n\t"                                                                \
                     "pushf\n\t"                                                                                       \
                     "pop %[fo]\n\t"                                                                                   \
                     : [rr] "+q"(r), [fo] "=r"(fout)                                                                   \
                     : [fi] "r"(hw_in(fin)), "c"(count)                                                                \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fout;                                                                                             \
  }

HW_SHIFT8(hw_shl8, "shlb")
HW_SHIFT8(hw_shr8, "shrb")
HW_SHIFT8(hw_sar8, "sarb")

/*
 * One comparison, reported with a denominator.
 *
 * `defined_mask` is the flags the architecture DEFINES for this operation. The
 * rest are compared too, but only counted -- see the undefined-flag report at
 * the end, which is a measurement of what this CPU actually does rather than a
 * pass/fail on something the manual declines to specify.
 */
static unsigned long g_hw_cases; /* every hardware comparison this run made */
static unsigned long g_hw_mismatch;

typedef struct Tally {
  const char *op;
  unsigned long cases;
  unsigned long mismatch;      /* on DEFINED flags -- these are failures */
  unsigned long undef_differ;  /* on undefined flags -- these are information */
  unsigned long undef_checked; /* the denominator for the line above */
  uint32_t first_a, first_b, first_fin, first_hw, first_us;
  int have_first;
  /* WHICH flag disagreed, per flag, so one run diagnoses instead of one run
     per hypothesis. A count of mismatches without this says only that
     something is wrong. */
  unsigned long per_flag[12];
  unsigned long undef_per_flag[12];
} Tally;

static const struct {
  const char *name;
  uint32_t bit;
  int idx;
} kFlagBits[] = {{"CF", X86P_CF, 0},
                 {"PF", X86P_PF, 2},
                 {"AF", X86P_AF, 4},
                 {"ZF", X86P_ZF, 6},
                 {"SF", X86P_SF, 7},
                 {"OF", X86P_OF, 11}};
enum { kFlagBitCount = (int)(sizeof kFlagBits / sizeof kFlagBits[0]) };

static void tally(Tally *t, uint32_t a, uint32_t b, uint32_t fin, uint32_t hw, uint32_t us, uint32_t defined_mask) {
  uint32_t undefined_mask = X86P_ARITH_FLAGS & ~defined_mask;
  hw &= X86P_ARITH_FLAGS;
  us &= X86P_ARITH_FLAGS;
  int i;
  t->cases++;
  for (i = 0; i < kFlagBitCount; i++) {
    uint32_t bit = kFlagBits[i].bit;
    if ((hw & bit) == (us & bit)) {
      continue;
    }
    if (bit & defined_mask) {
      t->per_flag[kFlagBits[i].idx]++;
    } else {
      t->undef_per_flag[kFlagBits[i].idx]++;
    }
  }
  if (undefined_mask) {
    t->undef_checked++;
    if ((hw & undefined_mask) != (us & undefined_mask)) {
      t->undef_differ++;
    }
  }
  if ((hw & defined_mask) != (us & defined_mask)) {
    t->mismatch++;
    if (!t->have_first) {
      t->have_first = 1;
      t->first_a = a;
      t->first_b = b;
      t->first_fin = fin;
      t->first_hw = hw;
      t->first_us = us;
    }
  }
}

static void report(const Tally *t) {
  /* The denominator is printed whether or not anything failed: "0 mismatches"
     is only a measurement when the number of comparisons is beside it. */
  g_hw_cases += t->cases;
  g_hw_mismatch += t->mismatch;
  printf("    %-6s %8lu case(s), %lu mismatch(es) on defined flags", t->op, t->cases, t->mismatch);
  if (t->undef_checked) {
    printf("; %lu/%lu differ on flags the ISA leaves undefined", t->undef_differ, t->undef_checked);
  }
  printf("\n");
  if (t->have_first) {
    printf("      first: a=%02x b=%02x fin=%03x  hw=%03x us=%03x  (differ: %03x)\n",
           t->first_a,
           t->first_b,
           t->first_fin,
           t->first_hw,
           t->first_us,
           t->first_hw ^ t->first_us);
  }
  {
    int i;
    int any = 0;
    for (i = 0; i < kFlagBitCount; i++) {
      unsigned long d = t->per_flag[kFlagBits[i].idx], u = t->undef_per_flag[kFlagBits[i].idx];
      if (!d && !u) {
        continue;
      }
      if (!any) {
        printf("      by flag:");
        any = 1;
      }
      printf(" %s=%lu%s", kFlagBits[i].name, d + u, u ? "(undef)" : "");
    }
    if (any) {
      printf("\n");
    }
  }
  g_checks++;
  if (t->mismatch) {
    g_failed++;
  }
  /* A run that compared nothing must fail, not pass silently. An exhaustive
     sweep whose loops never executed reports "0 mismatches" too. */
  g_checks++;
  if (t->cases == 0) {
    g_failed++;
    printf("    FAIL %s: compared NOTHING\n", t->op);
  }
}

/*
 * Undefined flags are not discarded: tally() records their differences with a
 * denominator and report() names each varying bit. They are observational
 * evidence about the current host, not a portable correctness requirement.
 */
#define ALL_DEFINED X86P_ARITH_FLAGS
#define LOGIC_DEFINED (X86P_ARITH_FLAGS & ~X86P_AF)

static uint32_t shift_defined(X86pFlagKind kind, unsigned count, unsigned bits) {
  uint32_t defined = X86P_PF | X86P_ZF | X86P_SF;
  /* SAR's sign bit remains the last bit shifted out for counts >= width. */
  if (kind == kX86pFlagsSar || count < bits) {
    defined |= X86P_CF;
  }
  if (count == 1u) {
    defined |= X86P_OF;
  }
  return defined;
}

static void test_hw_binops(void) {
  /* CF *and* AF. The first version of this sweep varied only CF and reported
     "0/131072 differ on undefined flags" for the logic ops -- a clean bill of
     health it had no power to give, since it never presented an incoming AF of
     1. Varying both is what showed that hardware CLEARS AF there. */
  static const uint32_t carries[] = {0, X86P_CF, X86P_AF, X86P_CF | X86P_AF};
  int ci;
  unsigned ai, bi;
  Tally t_add = {"ADD", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}},
        t_sub = {"SUB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
  Tally t_cmp = {"CMP", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}},
        t_and = {"AND", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
  Tally t_or = {"OR", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}}, t_xor = {"XOR", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
  Tally t_adc = {"ADC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}},
        t_sbb = {"SBB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};

  for (ci = 0; ci < (int)(sizeof carries / sizeof carries[0]); ci++) {
    uint32_t fin = carries[ci];
    for (ai = 0; ai < 256; ai++) {
      for (bi = 0; bi < 256; bi++) {
        uint8_t a = (uint8_t)ai, b = (uint8_t)bi, r;
        uint32_t hw;
        X86pFlags f;

        /* Every case starts from the same incoming flag state on BOTH sides,
           so a preserved flag is compared rather than assumed. */
        memset(&f, 0, sizeof f);
        x86p_flags_set_explicit(&f, fin);

        hw = hw_add8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsAdd, a, b, (uint32_t)r, 1);
        tally(&t_add, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);

        hw = hw_sub8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsSub, a, b, (uint32_t)r, 1);
        tally(&t_sub, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);

        /* CMP is SUB that discards its result: same flags, and modelling it any
           other way is how a compare and a subtract come to disagree. */
        hw = hw_cmp8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsSub, a, b, (uint32_t)(uint8_t)(a - b), 1);
        tally(&t_cmp, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);

        hw = hw_and8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsLogic, a, b, (uint32_t)r, 1);
        tally(&t_and, a, b, fin, hw, x86p_eflags(&f), LOGIC_DEFINED);

        hw = hw_or8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsLogic, a, b, (uint32_t)r, 1);
        tally(&t_or, a, b, fin, hw, x86p_eflags(&f), LOGIC_DEFINED);

        hw = hw_xor8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsLogic, a, b, (uint32_t)r, 1);
        tally(&t_xor, a, b, fin, hw, x86p_eflags(&f), LOGIC_DEFINED);

        /* ADC/SBB are the eager path: the reason it exists is that the lazy
           triple cannot carry a carry-in. */
        hw = hw_adc8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, x86p_flags_adc(a, b, fin & X86P_CF, (uint32_t)r, 1));
        tally(&t_adc, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);

        hw = hw_sbb8(a, b, fin, &r);
        x86p_flags_set_explicit(&f, x86p_flags_sbb(a, b, fin & X86P_CF, (uint32_t)r, 1));
        tally(&t_sbb, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
      }
    }
  }
  report(&t_add);
  report(&t_sub);
  report(&t_cmp);
  report(&t_and);
  report(&t_or);
  report(&t_xor);
  report(&t_adc);
  report(&t_sbb);
}

static void test_hw_unops(void) {
  static const uint32_t carries[] = {0, X86P_CF, X86P_AF, X86P_CF | X86P_AF};
  int ci;
  unsigned ai;
  Tally t_inc = {"INC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}},
        t_dec = {"DEC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};

  for (ci = 0; ci < (int)(sizeof carries / sizeof carries[0]); ci++) {
    uint32_t fin = carries[ci];
    for (ai = 0; ai < 256; ai++) {
      uint8_t a = (uint8_t)ai, r;
      uint32_t hw;
      X86pFlags f;

      hw = hw_inc8(a, fin, &r);
      memset(&f, 0, sizeof f);
      x86p_flags_set_explicit(&f, fin);
      set(&f, kX86pFlagsInc, a, 1u, (uint32_t)r, 1);
      /* CF is DEFINED for INC/DEC in the strongest sense: it must not change.
         Comparing it is the whole point of the separate kind. */
      tally(&t_inc, a, 1, fin, hw, x86p_eflags(&f), ALL_DEFINED);

      hw = hw_dec8(a, fin, &r);
      memset(&f, 0, sizeof f);
      x86p_flags_set_explicit(&f, fin);
      set(&f, kX86pFlagsDec, a, 1u, (uint32_t)r, 1);
      tally(&t_dec, a, 1, fin, hw, x86p_eflags(&f), ALL_DEFINED);
    }
  }
  report(&t_inc);
  report(&t_dec);
}

static void test_hw_shifts(void) {
  static const uint32_t carries[] = {0, X86P_CF, X86P_AF, X86P_CF | X86P_AF};
  int ci;
  unsigned ai, ni;
  Tally t_shl = {"SHL", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}},
        t_shr = {"SHR", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
  Tally t_sar = {"SAR", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
  CHECK((shift_defined(kX86pFlagsSar, 8u, 8u) & X86P_CF) != 0u);
  CHECK((shift_defined(kX86pFlagsSar, 31u, 8u) & X86P_CF) != 0u);
  CHECK((shift_defined(kX86pFlagsShl, 8u, 8u) & X86P_CF) == 0u);
  CHECK((shift_defined(kX86pFlagsShr, 31u, 8u) & X86P_CF) == 0u);

  for (ci = 0; ci < (int)(sizeof carries / sizeof carries[0]); ci++) {
    uint32_t fin = carries[ci];
    for (ai = 0; ai < 256; ai++) {
      /* Cover every nonzero masked count, including SAR at and beyond the
         operand width. Zero preserves flags and is exercised separately. */
      for (ni = 1; ni <= 31; ni++) {
        uint8_t a = (uint8_t)ai, n = (uint8_t)ni, r;
        uint32_t hw;
        X86pFlags f;

        hw = hw_shl8(a, n, fin, &r);
        memset(&f, 0, sizeof f);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsShl, a, n, (uint32_t)r, 1);
        tally(&t_shl, a, n, fin, hw, x86p_eflags(&f), shift_defined(kX86pFlagsShl, ni, 8u));

        hw = hw_shr8(a, n, fin, &r);
        memset(&f, 0, sizeof f);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsShr, a, n, (uint32_t)r, 1);
        tally(&t_shr, a, n, fin, hw, x86p_eflags(&f), shift_defined(kX86pFlagsShr, ni, 8u));

        hw = hw_sar8(a, n, fin, &r);
        memset(&f, 0, sizeof f);
        x86p_flags_set_explicit(&f, fin);
        set(&f, kX86pFlagsSar, a, n, (uint32_t)r, 1);
        tally(&t_sar, a, n, fin, hw, x86p_eflags(&f), shift_defined(kX86pFlagsSar, ni, 8u));
      }
    }
  }
  report(&t_shl);
  report(&t_shr);
  report(&t_sar);
}

/* ---- 16- and 32-bit widths ---------------------------------------------
 *
 * Byte width is exhaustive but it is NOT the width the game runs at, and the
 * model's width handling is not width-agnostic: masks, the sign bit, and the
 * carry-out position all move. A model exhaustively right at 8 bits and wrong
 * at 32 would pass everything above.
 *
 * These cannot be exhaustive, so they are deterministic instead: a fixed-seed
 * generator plus every boundary value that matters (0, 1, the sign bit and its
 * neighbours, all-ones), crossed with each other so the interesting pairs are
 * hit on purpose rather than hoped for.
 */
#define X86P_CTYPE_U16 uint16_t
#define X86P_CTYPE_U32 uint32_t

#define HW_BINOP(fname, insn, kind, sfx, con)                                                                          \
  static uint32_t fname(X86P_CTYPE_##kind a, X86P_CTYPE_##kind b, uint32_t fin, X86P_CTYPE_##kind *out) {              \
    uint64_t fout;                                                                                                     \
    X86P_CTYPE_##kind r = a;                                                                                           \
    __asm__ volatile("push %[fi]\n\t"                                                                                  \
                     "popf\n\t" insn sfx " %[bb], %[rr]\n\t"                                                           \
                     "pushf\n\t"                                                                                       \
                     "pop %[fo]"                                                                                       \
                     : [rr] "+" con(r), [fo] "=r"(fout)                                                                \
                     : [bb] con(b), [fi] "r"(hw_in(fin))                                                               \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fout;                                                                                             \
  }

HW_BINOP(hw_add16, "add", U16, "w", "r")
HW_BINOP(hw_sub16, "sub", U16, "w", "r")
HW_BINOP(hw_and16, "and", U16, "w", "r")
HW_BINOP(hw_adc16, "adc", U16, "w", "r")
HW_BINOP(hw_sbb16, "sbb", U16, "w", "r")
HW_BINOP(hw_add32, "add", U32, "l", "r")
HW_BINOP(hw_sub32, "sub", U32, "l", "r")
HW_BINOP(hw_and32, "and", U32, "l", "r")
HW_BINOP(hw_adc32, "adc", U32, "l", "r")
HW_BINOP(hw_sbb32, "sbb", U32, "l", "r")

/* A small deterministic PRNG, written out rather than taken from rand(), so the
   case list does not change with the C library. */
static uint32_t rng_state = 0x2545F491u;
static uint32_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static void wide_values(uint32_t *v, int *n, int w) {
  uint32_t m = x86p_width_mask(w);
  uint32_t sign = 1u << (w * 8 - 1);
  int i = 0;
  v[i++] = 0;
  v[i++] = 1;
  v[i++] = 2;
  v[i++] = sign - 1;
  v[i++] = sign;
  v[i++] = sign + 1;
  v[i++] = m - 1;
  v[i++] = m;
  v[i++] = 0x0F & m;
  v[i++] = 0x10 & m; /* the nibble boundary AF turns on */
  while (i < 40) {
    v[i++] = rng() & m;
  }
  *n = i;
}

static void test_hw_wide_widths(void) {
  static const uint32_t carries[] = {0, X86P_CF, X86P_AF, X86P_CF | X86P_AF};
  const int ncarry = (int)(sizeof carries / sizeof carries[0]);
  uint32_t va[40], vb[40];
  int na, nb, ci, i, j, w;

  for (w = 2; w <= 4; w += 2) {
    Tally t_add = {"ADD", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
    Tally t_sub = {"SUB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
    Tally t_and = {"AND", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
    Tally t_adc = {"ADC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
    Tally t_sbb = {"SBB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};
    rng_state = 0x2545F491u; /* same cases every run, and for both widths */
    wide_values(va, &na, w);
    wide_values(vb, &nb, w);
    printf("    -- %d-bit --\n", w * 8);
    for (ci = 0; ci < ncarry; ci++) {
      uint32_t fin = carries[ci];
      for (i = 0; i < na; i++) {
        for (j = 0; j < nb; j++) {
          uint32_t a = va[i], b = vb[j], hw;
          X86pFlags f;
          if (w == 2) {
            uint16_t r;
            hw = hw_add16((uint16_t)a, (uint16_t)b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, fin);
            set(&f, kX86pFlagsAdd, a, b, r, w);
            tally(&t_add, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
            hw = hw_sub16((uint16_t)a, (uint16_t)b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, fin);
            set(&f, kX86pFlagsSub, a, b, r, w);
            tally(&t_sub, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
            hw = hw_and16((uint16_t)a, (uint16_t)b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, fin);
            set(&f, kX86pFlagsLogic, a, b, r, w);
            tally(&t_and, a, b, fin, hw, x86p_eflags(&f), LOGIC_DEFINED);
            hw = hw_adc16((uint16_t)a, (uint16_t)b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, x86p_flags_adc(a, b, fin & X86P_CF, r, w));
            tally(&t_adc, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
            hw = hw_sbb16((uint16_t)a, (uint16_t)b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, x86p_flags_sbb(a, b, fin & X86P_CF, r, w));
            tally(&t_sbb, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
          } else {
            uint32_t r;
            hw = hw_add32(a, b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, fin);
            set(&f, kX86pFlagsAdd, a, b, r, w);
            tally(&t_add, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
            hw = hw_sub32(a, b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, fin);
            set(&f, kX86pFlagsSub, a, b, r, w);
            tally(&t_sub, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
            hw = hw_and32(a, b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, fin);
            set(&f, kX86pFlagsLogic, a, b, r, w);
            tally(&t_and, a, b, fin, hw, x86p_eflags(&f), LOGIC_DEFINED);
            hw = hw_adc32(a, b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, x86p_flags_adc(a, b, fin & X86P_CF, r, w));
            tally(&t_adc, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
            hw = hw_sbb32(a, b, fin, &r);
            memset(&f, 0, sizeof f);
            x86p_flags_set_explicit(&f, x86p_flags_sbb(a, b, fin & X86P_CF, r, w));
            tally(&t_sbb, a, b, fin, hw, x86p_eflags(&f), ALL_DEFINED);
          }
        }
      }
    }
    report(&t_add);
    report(&t_sub);
    report(&t_and);
    report(&t_adc);
    report(&t_sbb);
  }
}

/*
 * The oracle has to be shown lying before it can be trusted. A deliberately
 * wrong model must produce mismatches here; if it does not, the comparison is
 * not reaching the hardware and every "0 mismatches" above means nothing.
 */
static void test_the_oracle_can_fail(void) {
  unsigned ai, bi;
  unsigned long disagreed = 0, compared = 0;
  for (ai = 0; ai < 256; ai++) {
    for (bi = 0; bi < 256; bi++) {
      uint8_t a = (uint8_t)ai, b = (uint8_t)bi, r;
      uint32_t hw = hw_sub8(a, b, 0, &r);
      X86pFlags f;
      memset(&f, 0, sizeof f);
      /* SUB modelled as ADD: the classic wrong answer. */
      set(&f, kX86pFlagsAdd, a, b, (uint32_t)r, 1);
      compared++;
      if ((hw & X86P_ARITH_FLAGS) != (x86p_eflags(&f) & X86P_ARITH_FLAGS)) {
        disagreed++;
      }
    }
  }
  printf("    deliberate defect: %lu/%lu case(s) caught\n", disagreed, compared);
  CHECK_EQ_U(compared, 65536u);
  CHECK(disagreed > 30000); /* it must catch most of them, not one lucky case */
}

#else /* no oracle on this host */
#define HAVE_HW_ORACLE 0
#endif

int main(void) {
  RUN(test_width_mask_refuses_bad_widths);
  RUN(test_width_changes_the_answer);
  RUN(test_inc_dec_preserve_carry);
  RUN(test_shift_by_zero_is_refused);
  RUN(test_explicit_round_trip);
  RUN(test_parity_is_low_byte_only);
  RUN(test_none_and_null);
#if HAVE_HW_ORACLE
  RUN(test_hw_binops);
  RUN(test_hw_unops);
  RUN(test_hw_shifts);
  RUN(test_hw_wide_widths);
  RUN(test_the_oracle_can_fail);
#else
  /* Loud, and it changes the verdict line, because a reader must not take this
     run as evidence of hardware agreement. */
  printf("test hardware_oracle\n  SKIP -- host is not x86, so the flag model was "
         "NOT compared against a real CPU. The hermetic cases above passed; that "
         "is a weaker claim.\n");
#endif
#if HAVE_HW_ORACLE
  /* The denominator, stated once: "0 mismatches" is a measurement only with
     the number of comparisons beside it. */
  printf("hardware oracle: %lu comparison(s) against a real CPU, %lu mismatch(es) on architecturally defined flags; "
         "undefined-flag differences were measured separately\n",
         g_hw_cases,
         g_hw_mismatch);
  if (g_hw_cases == 0) {
    printf("    FAIL: the oracle compared NOTHING\n");
    g_failed++;
    g_test_failed++;
  }
#endif
  printf("%d check(s), %d failed, %d failing test(s)%s\n",
         g_checks,
         g_failed,
         g_test_failed,
         HAVE_HW_ORACLE ? "" : "  [NO HARDWARE ORACLE]");
  return g_test_failed ? 1 : 0;
}
