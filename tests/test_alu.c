/*
 * test_alu -- every ALU operation against a real CPU, RESULT and flags.
 *
 * test_flags proved the flag derivation given a result. It could not prove the
 * results themselves, because it fed hardware's own result back into the model
 * -- correct for a flags module, and a gap the moment there is an ALU. This
 * closes it: the model computes the result AND the flags from the inputs alone,
 * and both are compared against what the instruction actually did.
 *
 * Byte width is exhaustive; 32-bit is deterministic boundary-crossed sampling.
 * Shift and rotate counts run past the operand width on purpose, because the
 * 5-bit masking is architectural and is exactly the kind of rule that is
 * remembered for SHL and forgotten for RCR.
 */
#include "alu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;
static int g_test_failed;
static unsigned long g_hw_cases;
static unsigned long g_hw_mismatch;

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
/* Hermetic: true on every host.                                               */
/* ------------------------------------------------------------------------- */

static void test_every_op_is_named(void) {
  int i, named = 0;
  for (i = 0; i < (int)kX86pAluOpCount; i++) {
    const char *n = x86p_alu_name((X86pAluOp)i);
    CHECK(n != NULL && n[0] != '\0');
    CHECK(strcmp(n, "unknown") != 0);
    named++;
  }
  CHECK_EQ_U(named, (int)kX86pAluOpCount);
  for (i = 0; i < (int)kX86pAluUnOpCount; i++) {
    CHECK(strcmp(x86p_alu_unary_name((X86pAluUnOp)i), "unknown") != 0);
  }
  /* Out of range names itself rather than indexing off the end. */
  CHECK(strcmp(x86p_alu_name((X86pAluOp)kX86pAluOpCount), "unknown") == 0);
  CHECK(strcmp(x86p_alu_name((X86pAluOp)999), "unknown") == 0);
  CHECK(strcmp(x86p_alu_unary_name((X86pAluUnOp)-1), "unknown") == 0);
}

/* The group-1 encoding order is load-bearing: a decoder will index this enum
   with the ModRM reg field directly. If someone reorders the enum for
   tidiness, this fails rather than the decoder silently running OR for ADC. */
static void test_group1_order_matches_the_manual(void) {
  static const char *manual[] = {"ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP"};
  int i;
  for (i = 0; i < 8; i++) {
    CHECK(strcmp(x86p_alu_name((X86pAluOp)i), manual[i]) == 0);
  }
}

/* NOT writes no flags -- which is why it is an operation here rather than
   `XOR a, -1`, an expression that would clear CF and OF. */
static void test_not_writes_no_flags(void) {
  X86pFlags f;
  uint32_t before;
  memset(&f, 0, sizeof f);
  x86p_alu(kX86pAluSub, 0u, 1u, 1, &f); /* set CF, SF, AF, PF */
  before = x86p_eflags(&f);
  CHECK_EQ_U(x86p_alu_unary(kX86pAluNot, 0x0Fu, 1, &f), 0xF0u);
  CHECK_EQ_U(x86p_eflags(&f), before);
  /* and the contrast: XOR with all-ones gives the same value, other flags */
  x86p_alu(kX86pAluXor, 0x0Fu, 0xFFu, 1, &f);
  CHECK(x86p_eflags(&f) != before);
}

/* CMP and TEST derive flags identically to SUB and AND. Modelling them any
   other way is how a compare and a subtract come to disagree. */
static void test_cmp_and_test_match_sub_and_and(void) {
  X86pFlags a, b;
  unsigned x, y;
  int mismatches = 0, compared = 0;
  for (x = 0; x < 256; x++) {
    for (y = 0; y < 256; y++) {
      memset(&a, 0, sizeof a);
      memset(&b, 0, sizeof b);
      x86p_alu(kX86pAluSub, x, y, 1, &a);
      x86p_alu(kX86pAluCmp, x, y, 1, &b);
      compared++;
      if (x86p_eflags(&a) != x86p_eflags(&b)) {
        mismatches++;
      }
      memset(&a, 0, sizeof a);
      memset(&b, 0, sizeof b);
      x86p_alu(kX86pAluAnd, x, y, 1, &a);
      x86p_alu(kX86pAluTest, x, y, 1, &b);
      compared++;
      if (x86p_eflags(&a) != x86p_eflags(&b)) {
        mismatches++;
      }
    }
  }
  CHECK_EQ_U(compared, 131072);
  CHECK_EQ_U(mismatches, 0);
}

/* Divide reports its exception rather than aborting: the guest has to receive
   it, and it must be distinguishable from a divide that produced zero. */
static void test_divide_error_is_reported(void) {
  X86pFlags f;
  uint32_t q = 0xAAAAu, r = 0xBBBBu;
  memset(&f, 0, sizeof f);

  CHECK_EQ_U(x86p_alu_div(0, 10, 0, 1, &q, &r, &f), 0); /* divide by zero */
  CHECK_EQ_U(q, 0xAAAAu);                               /* nothing written */
  CHECK_EQ_U(r, 0xBBBBu);
  /* Quotient overflow: 0x0100 / 1 does not fit in 8 bits. The one people
     forget, and truncating instead would be silently wrong arithmetic. */
  CHECK_EQ_U(x86p_alu_div(1, 0, 1, 1, &q, &r, &f), 0);
  CHECK_EQ_U(x86p_alu_idiv(0, 0, 0, 4, &q, &r, &f), 0);
  /* INT_MIN / -1 overflows the signed quotient. */
  CHECK_EQ_U(x86p_alu_idiv(0xFFFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 4, &q, &r, &f), 0);
  CHECK_EQ_U(q, 0xAAAAu);

  /* And a legitimate zero quotient is NOT a failure. */
  CHECK_EQ_U(x86p_alu_div(0, 3, 10, 1, &q, &r, &f), 1);
  CHECK_EQ_U(q, 0u);
  CHECK_EQ_U(r, 3u);
}

/* ------------------------------------------------------------------------- */
/* Hardware oracle.                                                            */
/* ------------------------------------------------------------------------- */

#if defined(__x86_64__) || defined(__i386__)
#define HAVE_HW_ORACLE 1

static uint64_t hw_in(uint32_t f) {
  return (uint64_t)((f & X86P_ARITH_FLAGS) | X86P_EFLAGS_FIXED);
}

typedef struct Tally {
  const char *op;
  unsigned long cases, bad_result, bad_flags;
  uint32_t fa, fb, ffin, fhw_r, fus_r, fhw_f, fus_f;
  int have_first;
} Tally;

static void
note(Tally *t, uint32_t a, uint32_t b, uint32_t fin, uint32_t hw_r, uint32_t us_r, uint32_t hw_f, uint32_t us_f) {
  int bad = 0;
  t->cases++;
  g_hw_cases++;
  if (hw_r != us_r) {
    t->bad_result++;
    bad = 1;
  }
  if ((hw_f & X86P_ARITH_FLAGS) != (us_f & X86P_ARITH_FLAGS)) {
    t->bad_flags++;
    bad = 1;
  }
  if (bad) {
    g_hw_mismatch++;
    if (!t->have_first) {
      t->have_first = 1;
      t->fa = a;
      t->fb = b;
      t->ffin = fin;
      t->fhw_r = hw_r;
      t->fus_r = us_r;
      t->fhw_f = hw_f & X86P_ARITH_FLAGS;
      t->fus_f = us_f & X86P_ARITH_FLAGS;
    }
  }
}

static void report(const Tally *t) {
  printf("    %-5s %9lu case(s), %lu wrong result(s), %lu wrong flag word(s)\n",
         t->op,
         t->cases,
         t->bad_result,
         t->bad_flags);
  if (t->have_first) {
    printf("      first: a=%08x b=%08x fin=%03x | result hw=%08x us=%08x | flags hw=%03x us=%03x\n",
           t->fa,
           t->fb,
           t->ffin,
           t->fhw_r,
           t->fus_r,
           t->fhw_f,
           t->fus_f);
  }
  g_checks++;
  if (t->bad_result || t->bad_flags) {
    g_failed++;
  }
  /* A sweep whose loops never ran also reports zero wrong. */
  g_checks++;
  if (t->cases == 0) {
    g_failed++;
    printf("    FAIL %s: compared NOTHING\n", t->op);
  }
}

#define HW_BIN(fname, insn, ty, sfx)                                                                                   \
  static uint32_t fname(ty a, ty b, uint32_t fin, ty *out) {                                                           \
    uint64_t fo;                                                                                                       \
    ty r = a;                                                                                                          \
    __asm__ volatile("push %[fi]\n\tpopf\n\t" insn sfx " %[b], %[r]\n\tpushf\n\tpop %[fo]"                             \
                     : [r] "+r"(r), [fo] "=r"(fo)                                                                      \
                     : [b] "r"(b), [fi] "r"(hw_in(fin))                                                                \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fo;                                                                                               \
  }

#define HW_UN(fname, insn, ty, sfx)                                                                                    \
  static uint32_t fname(ty a, uint32_t fin, ty *out) {                                                                 \
    uint64_t fo;                                                                                                       \
    ty r = a;                                                                                                          \
    __asm__ volatile("push %[fi]\n\tpopf\n\t" insn sfx " %[r]\n\tpushf\n\tpop %[fo]"                                   \
                     : [r] "+r"(r), [fo] "=r"(fo)                                                                      \
                     : [fi] "r"(hw_in(fin))                                                                            \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fo;                                                                                               \
  }

#define HW_SHIFT(fname, insn, ty, sfx)                                                                                 \
  static uint32_t fname(ty a, uint8_t n, uint32_t fin, ty *out) {                                                      \
    uint64_t fo;                                                                                                       \
    ty r = a;                                                                                                          \
    __asm__ volatile("push %[fi]\n\tpopf\n\t" insn sfx " %%cl, %[r]\n\tpushf\n\tpop %[fo]"                             \
                     : [r] "+r"(r), [fo] "=r"(fo)                                                                      \
                     : [fi] "r"(hw_in(fin)), "c"(n)                                                                    \
                     : "cc");                                                                                          \
    *out = r;                                                                                                          \
    return (uint32_t)fo;                                                                                               \
  }

HW_BIN(hw_add8, "add", uint8_t, "b")
HW_BIN(hw_or8, "or", uint8_t, "b")
HW_BIN(hw_adc8, "adc", uint8_t, "b")
HW_BIN(hw_sbb8, "sbb", uint8_t, "b")
HW_BIN(hw_and8, "and", uint8_t, "b")
HW_BIN(hw_sub8, "sub", uint8_t, "b")
HW_BIN(hw_xor8, "xor", uint8_t, "b")
HW_UN(hw_not8, "not", uint8_t, "b")
HW_UN(hw_neg8, "neg", uint8_t, "b")
HW_UN(hw_inc8, "inc", uint8_t, "b")
HW_UN(hw_dec8, "dec", uint8_t, "b")
HW_SHIFT(hw_shl8, "shl", uint8_t, "b")
HW_SHIFT(hw_shr8, "shr", uint8_t, "b")
HW_SHIFT(hw_sar8, "sar", uint8_t, "b")
HW_SHIFT(hw_rol8, "rol", uint8_t, "b")
HW_SHIFT(hw_ror8, "ror", uint8_t, "b")
HW_SHIFT(hw_rcl8, "rcl", uint8_t, "b")
HW_SHIFT(hw_rcr8, "rcr", uint8_t, "b")

HW_BIN(hw_add32, "add", uint32_t, "l")
HW_BIN(hw_adc32, "adc", uint32_t, "l")
HW_BIN(hw_sbb32, "sbb", uint32_t, "l")
HW_BIN(hw_sub32, "sub", uint32_t, "l")
HW_BIN(hw_xor32, "xor", uint32_t, "l")
HW_UN(hw_neg32, "neg", uint32_t, "l")
HW_UN(hw_inc32, "inc", uint32_t, "l")
HW_SHIFT(hw_shl32, "shl", uint32_t, "l")
HW_SHIFT(hw_shr32, "shr", uint32_t, "l")
HW_SHIFT(hw_sar32, "sar", uint32_t, "l")
HW_SHIFT(hw_rol32, "rol", uint32_t, "l")
HW_SHIFT(hw_rcr32, "rcr", uint32_t, "l")

static void model(X86pAluOp op, uint32_t a, uint32_t b, int w, uint32_t fin, uint32_t *r, uint32_t *fl) {
  X86pFlags f;
  memset(&f, 0, sizeof f);
  x86p_flags_set_explicit(&f, fin);
  *r = x86p_alu(op, a, b, w, &f);
  *fl = x86p_eflags(&f);
}

static void model_un(X86pAluUnOp op, uint32_t a, int w, uint32_t fin, uint32_t *r, uint32_t *fl) {
  X86pFlags f;
  memset(&f, 0, sizeof f);
  x86p_flags_set_explicit(&f, fin);
  *r = x86p_alu_unary(op, a, w, &f);
  *fl = x86p_eflags(&f);
}

static const uint32_t kCarries[] = {0, X86P_CF, X86P_AF, X86P_CF | X86P_AF};
#define NCARRY ((int)(sizeof kCarries / sizeof kCarries[0]))

static void test_hw_binary_8bit_exhaustive(void) {
  struct {
    const char *name;
    X86pAluOp op;
    uint32_t (*hw)(uint8_t, uint8_t, uint32_t, uint8_t *);
  } ops[] = {
      {"ADD", kX86pAluAdd, hw_add8},
      {"OR", kX86pAluOr, hw_or8},
      {"ADC", kX86pAluAdc, hw_adc8},
      {"SBB", kX86pAluSbb, hw_sbb8},
      {"AND", kX86pAluAnd, hw_and8},
      {"SUB", kX86pAluSub, hw_sub8},
      {"XOR", kX86pAluXor, hw_xor8},
  };
  const int nops = (int)(sizeof ops / sizeof ops[0]);
  int k, ci;
  unsigned ai, bi;
  for (k = 0; k < nops; k++) {
    Tally t = {ops[k].name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (ci = 0; ci < NCARRY; ci++) {
      for (ai = 0; ai < 256; ai++) {
        for (bi = 0; bi < 256; bi++) {
          uint8_t hr;
          uint32_t hf = ops[k].hw((uint8_t)ai, (uint8_t)bi, kCarries[ci], &hr);
          uint32_t ur, uf;
          model(ops[k].op, ai, bi, 1, kCarries[ci], &ur, &uf);
          note(&t, ai, bi, kCarries[ci], hr, ur, hf, uf);
        }
      }
    }
    report(&t);
  }
}

static void test_hw_unary_8bit_exhaustive(void) {
  struct {
    const char *name;
    X86pAluUnOp op;
    uint32_t (*hw)(uint8_t, uint32_t, uint8_t *);
  } ops[] = {
      {"NOT", kX86pAluNot, hw_not8},
      {"NEG", kX86pAluNeg, hw_neg8},
      {"INC", kX86pAluInc, hw_inc8},
      {"DEC", kX86pAluDec, hw_dec8},
  };
  const int nops = (int)(sizeof ops / sizeof ops[0]);
  int k, ci;
  unsigned ai;
  for (k = 0; k < nops; k++) {
    Tally t = {ops[k].name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (ci = 0; ci < NCARRY; ci++) {
      for (ai = 0; ai < 256; ai++) {
        uint8_t hr;
        uint32_t hf = ops[k].hw((uint8_t)ai, kCarries[ci], &hr);
        uint32_t ur, uf;
        model_un(ops[k].op, ai, 1, kCarries[ci], &ur, &uf);
        note(&t, ai, 0, kCarries[ci], hr, ur, hf, uf);
      }
    }
    report(&t);
  }
}

/*
 * Counts run to 33, past both the operand width and the 5-bit mask, because
 * that masking is architectural and is exactly the rule remembered for SHL and
 * forgotten for RCR. Rotates through carry reduce modulo width+1, which is a
 * different number again.
 */
static void test_hw_shifts_and_rotates_8bit(void) {
  struct {
    const char *name;
    X86pAluOp op;
    uint32_t (*hw)(uint8_t, uint8_t, uint32_t, uint8_t *);
  } ops[] = {
      {"SHL", kX86pAluShl, hw_shl8},
      {"SHR", kX86pAluShr, hw_shr8},
      {"SAR", kX86pAluSar, hw_sar8},
      {"ROL", kX86pAluRol, hw_rol8},
      {"ROR", kX86pAluRor, hw_ror8},
      {"RCL", kX86pAluRcl, hw_rcl8},
      {"RCR", kX86pAluRcr, hw_rcr8},
  };
  const int nops = (int)(sizeof ops / sizeof ops[0]);
  int k, ci;
  unsigned ai, n;
  for (k = 0; k < nops; k++) {
    Tally t = {ops[k].name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (ci = 0; ci < NCARRY; ci++) {
      for (ai = 0; ai < 256; ai++) {
        for (n = 0; n <= 33; n++) {
          uint8_t hr;
          uint32_t hf = ops[k].hw((uint8_t)ai, (uint8_t)n, kCarries[ci], &hr);
          uint32_t ur, uf;
          model(ops[k].op, ai, n, 1, kCarries[ci], &ur, &uf);
          note(&t, ai, n, kCarries[ci], hr, ur, hf, uf);
        }
      }
    }
    report(&t);
  }
}

/* 32 bits is the width the game runs at, and the model is not width-agnostic:
   masks, the sign bit and the carry-out position all move. */
static uint32_t rng_state;
static uint32_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static void values32(uint32_t *v, int *n) {
  int i = 0;
  v[i++] = 0;
  v[i++] = 1;
  v[i++] = 2;
  v[i++] = 0x7FFFFFFFu;
  v[i++] = 0x80000000u;
  v[i++] = 0x80000001u;
  v[i++] = 0xFFFFFFFEu;
  v[i++] = 0xFFFFFFFFu;
  v[i++] = 0x0000000Fu;
  v[i++] = 0x00000010u;
  while (i < 48) {
    v[i++] = rng();
  }
  *n = i;
}

static void test_hw_32bit(void) {
  struct {
    const char *name;
    X86pAluOp op;
    uint32_t (*hw)(uint32_t, uint32_t, uint32_t, uint32_t *);
  } bin[] = {
      {"ADD", kX86pAluAdd, hw_add32},
      {"ADC", kX86pAluAdc, hw_adc32},
      {"SBB", kX86pAluSbb, hw_sbb32},
      {"SUB", kX86pAluSub, hw_sub32},
      {"XOR", kX86pAluXor, hw_xor32},
  };
  struct {
    const char *name;
    X86pAluOp op;
    uint32_t (*hw)(uint32_t, uint8_t, uint32_t, uint32_t *);
  } sh[] = {
      {"SHL", kX86pAluShl, hw_shl32},
      {"SHR", kX86pAluShr, hw_shr32},
      {"SAR", kX86pAluSar, hw_sar32},
      {"ROL", kX86pAluRol, hw_rol32},
      {"RCR", kX86pAluRcr, hw_rcr32},
  };
  uint32_t va[48], vb[48];
  int na, nb, k, ci, i, j;
  unsigned n;

  rng_state = 0x9E3779B9u;
  values32(va, &na);
  values32(vb, &nb);

  for (k = 0; k < (int)(sizeof bin / sizeof bin[0]); k++) {
    Tally t = {bin[k].name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (ci = 0; ci < NCARRY; ci++) {
      for (i = 0; i < na; i++) {
        for (j = 0; j < nb; j++) {
          uint32_t hr, ur, uf;
          uint32_t hf = bin[k].hw(va[i], vb[j], kCarries[ci], &hr);
          model(bin[k].op, va[i], vb[j], 4, kCarries[ci], &ur, &uf);
          note(&t, va[i], vb[j], kCarries[ci], hr, ur, hf, uf);
        }
      }
    }
    report(&t);
  }
  for (k = 0; k < (int)(sizeof sh / sizeof sh[0]); k++) {
    Tally t = {sh[k].name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (ci = 0; ci < NCARRY; ci++) {
      for (i = 0; i < na; i++) {
        for (n = 0; n <= 33; n++) {
          uint32_t hr, ur, uf;
          uint32_t hf = sh[k].hw(va[i], (uint8_t)n, kCarries[ci], &hr);
          model(sh[k].op, va[i], n, 4, kCarries[ci], &ur, &uf);
          note(&t, va[i], n, kCarries[ci], hr, ur, hf, uf);
        }
      }
    }
    report(&t);
  }
  {
    Tally tn = {"NEG", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, ti = {"INC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (ci = 0; ci < NCARRY; ci++) {
      for (i = 0; i < na; i++) {
        uint32_t hr, ur, uf, hf;
        hf = hw_neg32(va[i], kCarries[ci], &hr);
        model_un(kX86pAluNeg, va[i], 4, kCarries[ci], &ur, &uf);
        note(&tn, va[i], 0, kCarries[ci], hr, ur, hf, uf);
        hf = hw_inc32(va[i], kCarries[ci], &hr);
        model_un(kX86pAluInc, va[i], 4, kCarries[ci], &ur, &uf);
        note(&ti, va[i], 0, kCarries[ci], hr, ur, hf, uf);
      }
    }
    report(&tn);
    report(&ti);
  }
}

/*
 * MUL/IMUL/DIV/IDIV, which produce a register PAIR.
 *
 * The divides are run on hardware ONLY where the model says the operation
 * succeeds. That is not a way of avoiding the hard cases -- it is itself the
 * test of the fault predicate: if the model says "fine" where a real CPU
 * raises #DE, this process dies of SIGFPE, which is about as loud as a test
 * failure gets. The predicate being too WIDE is caught the other way, by the
 * refusal cases in test_divide_error_is_reported.
 */
static void test_hw_muldiv_8bit(void) {
  unsigned ai, bi;
  Tally tm = {"MUL", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, ti = {"IMUL", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  Tally td = {"DIV", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, tid = {"IDIV", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  unsigned long div_ok = 0, div_faulted = 0;

  for (ai = 0; ai < 256; ai++) {
    for (bi = 0; bi < 256; bi++) {
      uint16_t hax;
      uint64_t hfo;
      uint32_t lo, hi, uf;
      X86pFlags f;

      /* MUL r/m8: AX = AL * r/m8 */
      __asm__ volatile("push %[fi]\n\tpopf\n\tmulb %[b]\n\tpushf\n\tpop %[fo]"
                       : "=a"(hax), [fo] "=r"(hfo)
                       : "a"((uint8_t)ai), [b] "q"((uint8_t)bi), [fi] "r"(hw_in(0))
                       : "cc");
      memset(&f, 0, sizeof f);
      x86p_alu_mul(ai, bi, 1, &lo, &hi, &f);
      uf = x86p_eflags(&f);
      note(&tm, ai, bi, 0, hax, (uint32_t)((hi << 8) | lo), (uint32_t)hfo, uf);

      /* IMUL r/m8: AX = AL * r/m8, signed */
      __asm__ volatile("push %[fi]\n\tpopf\n\timulb %[b]\n\tpushf\n\tpop %[fo]"
                       : "=a"(hax), [fo] "=r"(hfo)
                       : "a"((uint8_t)ai), [b] "q"((uint8_t)bi), [fi] "r"(hw_in(0))
                       : "cc");
      memset(&f, 0, sizeof f);
      x86p_alu_imul(ai, bi, 1, &lo, &hi, &f);
      uf = x86p_eflags(&f);
      note(&ti, ai, bi, 0, hax, (uint32_t)((hi << 8) | lo), (uint32_t)hfo, uf);

      /* DIV r/m8: AL = AX / r/m8, AH = remainder. `ai` is the 16-bit dividend's
         high byte paired with a fixed low byte, so quotient overflow is
         reachable rather than hypothetical. */
      {
        uint32_t q, r;
        uint16_t dividend = (uint16_t)((ai << 8) | 0x5Au);
        memset(&f, 0, sizeof f);
        if (x86p_alu_div((uint32_t)ai, 0x5Au, bi, 1, &q, &r, &f)) {
          div_ok++;
          __asm__ volatile("push %[fi]\n\tpopf\n\tdivb %[b]\n\tpushf\n\tpop %[fo]"
                           : "=a"(hax), [fo] "=r"(hfo)
                           : "a"(dividend), [b] "q"((uint8_t)bi), [fi] "r"(hw_in(0))
                           : "cc");
          /* Flags are architecturally undefined after a divide and the model
             deliberately leaves them alone, so only the RESULT is compared. */
          note(&td, ai, bi, 0, (uint32_t)hax, (r << 8) | q, 0, 0);
        } else {
          div_faulted++;
        }

        memset(&f, 0, sizeof f);
        if (x86p_alu_idiv((uint32_t)ai, 0x5Au, bi, 1, &q, &r, &f)) {
          __asm__ volatile("push %[fi]\n\tpopf\n\tidivb %[b]\n\tpushf\n\tpop %[fo]"
                           : "=a"(hax), [fo] "=r"(hfo)
                           : "a"(dividend), [b] "q"((uint8_t)bi), [fi] "r"(hw_in(0))
                           : "cc");
          note(&tid, ai, bi, 0, (uint32_t)hax, (r << 8) | q, 0, 0);
        }
      }
    }
  }
  report(&tm);
  report(&ti);
  report(&td);
  report(&tid);
  /* Both outcomes must occur, or the sweep only exercised one arm. */
  printf("    divides: %lu executed on hardware, %lu refused by the model\n", div_ok, div_faulted);
  CHECK(div_ok > 0);
  CHECK(div_faulted > 0);
}

/* The oracle must be shown lying before any of the zeros above mean anything. */
static void test_the_oracle_can_fail(void) {
  unsigned ai, bi;
  unsigned long caught = 0, compared = 0;
  for (ai = 0; ai < 256; ai++) {
    for (bi = 0; bi < 256; bi++) {
      uint8_t hr;
      uint32_t hf = hw_sub8((uint8_t)ai, (uint8_t)bi, 0, &hr);
      uint32_t ur, uf;
      model(kX86pAluAdd, ai, bi, 1, 0, &ur, &uf); /* deliberately the wrong op */
      compared++;
      if (hr != ur || (hf & X86P_ARITH_FLAGS) != (uf & X86P_ARITH_FLAGS)) {
        caught++;
      }
    }
  }
  printf("    deliberate defect (SUB modelled as ADD): %lu/%lu caught\n", caught, compared);
  CHECK_EQ_U(compared, 65536u);
  CHECK(caught > 60000);
}

#else
#define HAVE_HW_ORACLE 0
#endif

int main(void) {
  RUN(test_every_op_is_named);
  RUN(test_group1_order_matches_the_manual);
  RUN(test_not_writes_no_flags);
  RUN(test_cmp_and_test_match_sub_and_and);
  RUN(test_divide_error_is_reported);
#if HAVE_HW_ORACLE
  RUN(test_hw_binary_8bit_exhaustive);
  RUN(test_hw_unary_8bit_exhaustive);
  RUN(test_hw_shifts_and_rotates_8bit);
  RUN(test_hw_32bit);
  RUN(test_hw_muldiv_8bit);
  RUN(test_the_oracle_can_fail);
  printf("hardware oracle: %lu comparison(s) against a real CPU, %lu mismatch(es)\n", g_hw_cases, g_hw_mismatch);
  if (g_hw_cases == 0) {
    printf("    FAIL: the oracle compared NOTHING\n");
    g_failed++;
    g_test_failed++;
  }
#else
  printf("test hardware_oracle\n  SKIP -- host is not x86, so no ALU result or flag "
         "was compared against a real CPU.\n");
#endif
  printf("%d check(s), %d failed, %d failing test(s)%s\n",
         g_checks,
         g_failed,
         g_test_failed,
         HAVE_HW_ORACLE ? "" : "  [NO HARDWARE ORACLE]");
  return g_test_failed ? 1 : 0;
}
