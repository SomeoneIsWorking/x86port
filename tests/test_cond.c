/*
 * test_cond -- all 16 conditions against hardware SETcc, exhaustively.
 *
 * The whole input space is 64 flag states (six arithmetic flags) by 16
 * conditions = 1024 cases, so there is no excuse for sampling. Each is compared
 * against a real SETcc executed with that exact flag state loaded.
 *
 * This matters more than its size suggests. The signed and unsigned pairs read
 * identically in English and test different flags, and choosing wrong produces
 * a comparison that is right for small positive values and wrong across the
 * sign boundary -- which is precisely the case a hand-written expectation
 * tends to omit. Enumerating the space removes the judgement.
 */
#include "cond.h"

#include <stdint.h>
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

static void test_every_condition_is_named(void) {
  static const char *manual[] = {
      "O", "NO", "B", "NB", "Z", "NZ", "BE", "A", "S", "NS", "P", "NP", "L", "GE", "LE", "G"};
  int i;
  CHECK_EQ_U(kX86pCondCount, 16);
  for (i = 0; i < (int)kX86pCondCount; i++) {
    CHECK(strcmp(x86p_cond_name((X86pCond)i), manual[i]) == 0);
  }
  /* Out of range names itself and is false, rather than indexing off the end. */
  CHECK(strcmp(x86p_cond_name((X86pCond)16), "??") == 0);
  CHECK(strcmp(x86p_cond_name((X86pCond)-1), "??") == 0);
  CHECK_EQ_U(x86p_cond_eflags((X86pCond)16, 0xFFFFFFFFu), 0);
}

/* Complements must be exact complements, over the whole flag space. A pair
   that agrees somewhere is a condition that would both take and not take a
   branch, and it would show up as an impossible control-flow divergence. */
static void test_complement_pairs(void) {
  static const int pairs[][2] = {{kX86pCondO, kX86pCondNO},
                                 {kX86pCondB, kX86pCondNB},
                                 {kX86pCondZ, kX86pCondNZ},
                                 {kX86pCondBE, kX86pCondA},
                                 {kX86pCondS, kX86pCondNS},
                                 {kX86pCondP, kX86pCondNP},
                                 {kX86pCondL, kX86pCondGE},
                                 {kX86pCondLE, kX86pCondG}};
  const int npairs = (int)(sizeof pairs / sizeof pairs[0]);
  int i;
  unsigned bits;
  int compared = 0;
  /* Eight pairs is all sixteen conditions: none is left unpaired. */
  CHECK_EQ_U(npairs * 2, (int)kX86pCondCount);
  for (i = 0; i < npairs; i++) {
    for (bits = 0; bits < 64; bits++) {
      uint32_t fl = ((bits & 1) ? X86P_CF : 0) | ((bits & 2) ? X86P_PF : 0) | ((bits & 4) ? X86P_AF : 0) |
                    ((bits & 8) ? X86P_ZF : 0) | ((bits & 16) ? X86P_SF : 0) | ((bits & 32) ? X86P_OF : 0);
      compared++;
      CHECK_EQ_U(x86p_cond_eflags((X86pCond)pairs[i][0], fl), !x86p_cond_eflags((X86pCond)pairs[i][1], fl));
    }
  }
  CHECK_EQ_U(compared, 8 * 64);
}

#if defined(__x86_64__) || defined(__i386__)
#define HAVE_HW_ORACLE 1

/*
 * SETcc with a runtime condition number cannot be written as one asm block --
 * the condition is part of the opcode. Sixteen one-line functions is the
 * honest way to say that; a table of them keeps the sweep a loop.
 */
#define SETCC(name, suffix)                                                                                            \
  static int name(uint64_t fl) {                                                                                       \
    uint8_t r;                                                                                                         \
    __asm__ volatile("push %[f]\n\tpopf\n\tset" suffix " %[r]" : [r] "=q"(r) : [f] "r"(fl) : "cc");                    \
    return r ? 1 : 0;                                                                                                  \
  }

SETCC(hw_o, "o")
SETCC(hw_no, "no")
SETCC(hw_b, "b")
SETCC(hw_nb, "nb")
SETCC(hw_z, "z")
SETCC(hw_nz, "nz")
SETCC(hw_be, "be")
SETCC(hw_a, "a")
SETCC(hw_s, "s")
SETCC(hw_ns, "ns")
SETCC(hw_p, "p")
SETCC(hw_np, "np")
SETCC(hw_l, "l")
SETCC(hw_ge, "ge")
SETCC(hw_le, "le")
SETCC(hw_g, "g")

static void test_hw_all_conditions_exhaustive(void) {
  static int (*const hw[])(uint64_t) = {
      hw_o, hw_no, hw_b, hw_nb, hw_z, hw_nz, hw_be, hw_a, hw_s, hw_ns, hw_p, hw_np, hw_l, hw_ge, hw_le, hw_g};
  unsigned long compared = 0, mismatched = 0;
  int cc;
  unsigned bits;
  /* The table must cover the enum, or the sweep silently checks a subset. */
  CHECK_EQ_U((int)(sizeof hw / sizeof hw[0]), (int)kX86pCondCount);

  for (cc = 0; cc < (int)kX86pCondCount; cc++) {
    for (bits = 0; bits < 64; bits++) {
      uint32_t fl = ((bits & 1) ? X86P_CF : 0) | ((bits & 2) ? X86P_PF : 0) | ((bits & 4) ? X86P_AF : 0) |
                    ((bits & 8) ? X86P_ZF : 0) | ((bits & 16) ? X86P_SF : 0) | ((bits & 32) ? X86P_OF : 0);
      int us = x86p_cond_eflags((X86pCond)cc, fl | X86P_EFLAGS_FIXED);
      int them = hw[cc]((uint64_t)(fl | X86P_EFLAGS_FIXED));
      compared++;
      if (us != them) {
        mismatched++;
        if (mismatched <= 4) {
          printf("    MISMATCH J%-2s flags=%03x: hardware=%d model=%d\n", x86p_cond_name((X86pCond)cc), fl, them, us);
        }
      }
    }
  }
  printf("    %lu case(s) against hardware SETcc (16 conditions x 64 flag states), %lu mismatch(es)\n",
         compared,
         mismatched);
  CHECK_EQ_U(compared, 1024u); /* the whole space, not a sample */
  CHECK_EQ_U(mismatched, 0u);
}

/* The oracle has to be shown lying. JB and JL are the pair that reads the same
   in English and tests different flags, so that is the substitution to make. */
static void test_the_oracle_can_fail(void) {
  unsigned bits;
  unsigned long caught = 0;
  for (bits = 0; bits < 64; bits++) {
    uint32_t fl = ((bits & 1) ? X86P_CF : 0) | ((bits & 2) ? X86P_PF : 0) | ((bits & 4) ? X86P_AF : 0) |
                  ((bits & 8) ? X86P_ZF : 0) | ((bits & 16) ? X86P_SF : 0) | ((bits & 32) ? X86P_OF : 0);
    if (x86p_cond_eflags(kX86pCondB, fl) != hw_l((uint64_t)(fl | X86P_EFLAGS_FIXED))) {
      caught++;
    }
  }
  printf("    deliberate defect (unsigned B where signed L was meant): %lu/64 caught\n", caught);
  CHECK(caught >= 24); /* they agree on plenty of states -- which is the point */
}
#else
#define HAVE_HW_ORACLE 0
#endif

int main(void) {
  RUN(test_every_condition_is_named);
  RUN(test_complement_pairs);
#if HAVE_HW_ORACLE
  RUN(test_hw_all_conditions_exhaustive);
  RUN(test_the_oracle_can_fail);
#else
  printf("test hardware_oracle\n  SKIP -- host is not x86, so no condition was "
         "compared against a real SETcc.\n");
#endif
  printf("%d check(s), %d failed, %d failing test(s)%s\n",
         g_checks,
         g_failed,
         g_test_failed,
         HAVE_HW_ORACLE ? "" : "  [NO HARDWARE ORACLE]");
  return g_test_failed ? 1 : 0;
}
