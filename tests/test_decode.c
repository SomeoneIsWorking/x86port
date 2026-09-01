/*
 * test_decode -- the hermetic half of decoder verification.
 *
 * The corpus differential (tools/decode_diff.c) is the evidence that the
 * decoder reads 2.1M real instructions correctly, but it needs a title's Ghidra
 * export and so cannot run here. This file covers what a corpus cannot: the
 * REFUSALS. Every instruction in a real corpus decodes, so a corpus run can
 * never demonstrate that a byte sequence which is not an instruction is
 * reported as one rather than resynchronised into a plausible wrong answer.
 */
#include "decode.h"

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

/*
 * Real encodings taken from pc/xmen2's own Ghidra export, so these are bytes the
 * game actually contains rather than bytes invented to pass. The comment is the
 * corpus's own reading of them.
 */
static void test_known_encodings(void) {
  struct {
    const char *name;
    uint8_t bytes[8];
    size_t n;
    const char *mnemonic;
    uint32_t length;
  } cases[] = {
      /* Gap::Math::igDegreesToRadiansf, the first function in libIGMath */
      {"FLD float ptr [ESP+4]", {0xd9, 0x44, 0x24, 0x04}, 4, "FLD", 4},
      {"FMUL float ptr [abs]", {0xd8, 0x0d, 0x54, 0x98, 0x02, 0x10}, 6, "FMUL", 6},
      {"RET", {0xc3}, 1, "RET", 1},
      /* the four commonest mnemonics in the corpus */
      {"PUSH EBP", {0x55}, 1, "PUSH", 1},
      {"MOV EBP,ESP", {0x8b, 0xec}, 2, "MOV", 2},
      {"POP EBP", {0x5d}, 1, "POP", 1},
      {"TEST EAX,EAX", {0x85, 0xc0}, 2, "TEST", 2},
  };
  const int n = (int)(sizeof cases / sizeof cases[0]);
  int i;
  for (i = 0; i < n; i++) {
    X86pInsn insn;
    memset(&insn, 0, sizeof insn);
    CHECK(x86p_decode(cases[i].bytes, cases[i].n, &insn) == cases[i].length);
    CHECK(strcmp(insn.mnemonic, cases[i].mnemonic) == 0);
    CHECK_EQ_U(insn.length, cases[i].length);
  }
}

/*
 * The family this framework exists to cover first: 3DNow!, which the static
 * translator could not read at all. 0F 0F /r imm8, where the trailing byte
 * selects the operation.
 */
static void test_three_dnow_encodings_decode(void) {
  struct {
    uint8_t bytes[8];
    size_t n;
    const char *mnemonic;
  } cases[] = {
      {{0x0f, 0x0f, 0xc1, 0xb4}, 4, "PFMUL"}, /* PFMUL mm0, mm1 */
      {{0x0f, 0x0f, 0xc1, 0x9e}, 4, "PFADD"}, /* PFADD mm0, mm1 */
      {{0x0f, 0x0f, 0xc1, 0x9a}, 4, "PFSUB"}, /* PFSUB mm0, mm1 */
      {{0x0f, 0x0f, 0xc1, 0xae}, 4, "PFACC"}, /* PFACC mm0, mm1 */
      {{0x0f, 0x0f, 0xc1, 0x96}, 4, "PFRCP"}, /* refused by semantics, still DECODES */
  };
  const int n = (int)(sizeof cases / sizeof cases[0]);
  int i;
  for (i = 0; i < n; i++) {
    X86pInsn insn;
    memset(&insn, 0, sizeof insn);
    CHECK(x86p_decode(cases[i].bytes, cases[i].n, &insn) == 4);
    CHECK(strcmp(insn.mnemonic, cases[i].mnemonic) == 0);
  }
}

/*
 * THE NEGATIVE. Non-instructions must be REPORTED, and *out must be untouched
 * so a caller that ignores the zero cannot act on a stale or invented
 * instruction. This is the case a corpus of real code can never exercise, and
 * the one that separates "decoder" from "static analysis guessing at data".
 */
static void test_refusals(void) {
  static const uint8_t lock_nothing[] = {0xf0, 0xf0, 0xf0, 0xf0};
  static const uint8_t truncated[] = {0x0f, 0x0f}; /* 3DNow! with no modrm/imm */
  static const uint8_t one[] = {0x90};
  X86pInsn insn;
  int refused = 0;

  memset(&insn, 0, sizeof insn);
  insn.length = 0xDEAD;
  insn.mnemonic = "SENTINEL";

  if (!x86p_decode(NULL, 4, &insn)) {
    refused++;
  }
  if (!x86p_decode(one, 0, &insn)) {
    refused++;
  }
  if (!x86p_decode(one, 1, NULL)) {
    refused++;
  }
  if (!x86p_decode(lock_nothing, sizeof lock_nothing, &insn)) {
    refused++;
  }
  if (!x86p_decode(truncated, sizeof truncated, &insn)) {
    refused++;
  }
  CHECK_EQ_U(refused, 5); /* denominator: every refusal probed */

  /* Untouched, in every one of those cases. */
  CHECK_EQ_U(insn.length, 0xDEAD);
  CHECK(strcmp(insn.mnemonic, "SENTINEL") == 0);
}

/* A decode never reports more than the caller said was readable, and never
   reports zero on success -- a zero-length success would spin any interpreter
   loop forever on the same address. */
static void test_length_is_bounded_and_nonzero(void) {
  static const uint8_t nop3[] = {0x0f, 0x1f, 0x00, 0xcc, 0xcc};
  X86pInsn insn;
  size_t avail;
  for (avail = 1; avail <= sizeof nop3; avail++) {
    memset(&insn, 0, sizeof insn);
    if (x86p_decode(nop3, avail, &insn)) {
      CHECK(insn.length > 0);
      CHECK(insn.length <= avail);
      CHECK(insn.length <= X86P_MAX_INSN_LEN);
    }
  }
}

static void test_decoder_is_identified(void) {
  const char *id = x86p_decoder_id();
  CHECK(id != NULL && id[0] != '\0');
  /* A divergence report has to say which decoder produced the evidence. */
  CHECK(strstr(id, "zydis") != NULL);
}

int main(void) {
  RUN(test_known_encodings);
  RUN(test_three_dnow_encodings_decode);
  RUN(test_refusals);
  RUN(test_length_is_bounded_and_nonzero);
  RUN(test_decoder_is_identified);
  printf("%d check(s), %d failed, %d failing test(s)\n", g_checks, g_failed, g_test_failed);
  return g_test_failed ? 1 : 0;
}
