/*
 * test_wasm_cond.c -- the inline conditions against the authority that used to
 * be called instead.
 *
 * WHY THE PAIRS MATTER MORE THAN THE COVERAGE. JB and JL both read "less than"
 * in English and test entirely different flags, so a wrong choice works for
 * small positive values and fails across the sign boundary. The corpus below
 * is mostly sign boundaries for that reason: equal operands, zero, the most
 * negative value, the largest positive one, and pairs that differ in sign.
 *
 * WHY THE SECOND INSTRUCTION IS THE POINT. An inline condition exists only
 * when the lowering knows what wrote the flags, which it knows from the
 * PREVIOUS instruction in the same block. A SETcc on its own is the block's
 * first instruction, its predecessor is whatever ran before the block, and it
 * takes the fall-back path -- so a suite of single-instruction cases would
 * pass without executing one line of the code under test. Every case here is
 * a compare followed by the SETcc that reads it.
 *
 * WHY THE COUNTERS ARE CHECKED SEPARATELY. Calling the authority is entirely
 * correct and merely slow, so a lowering that silently stopped inlining would
 * pass every differential above. The counter checks assert which path each
 * shape took, and they assert BOTH answers: a block whose condition is inline
 * and a block whose condition is not.
 */
#include "decode.h"
#include "jit_wasm_cond.h"
#include "jit_wasm_lower.h"
#include "wasm_differential.h"

#include <stdio.h>
#include <string.h>

static WasmTest suite;

/* Junk in the bits above the operand width, so a derivation that forgot to
   normalise cannot pass by accident. */
enum { kHighJunk = 0xDEAD0000u };

/*
 * The flag-writing instructions, reg to reg or on EAX. INC and DEC preserve
 * CF, so each appears after STC and after CLC: a derivation that read CF from
 * the result instead of the carry it kept would agree with one of the two.
 */
typedef struct Form {
  const char *name;
  uint8_t bytes[3];
  unsigned size;
  unsigned width;
  unsigned insns; /* instructions in `bytes` */
} Form;

static const Form kForms[] = {
    {"cmp al, bl", {0x38, 0xD8}, 2u, 1u, 1u},
    {"cmp ax, bx", {0x66, 0x39, 0xD8}, 3u, 2u, 1u},
    {"cmp eax, ebx", {0x39, 0xD8}, 2u, 4u, 1u},
    {"test al, bl", {0x84, 0xD8}, 2u, 1u, 1u},
    {"test ax, bx", {0x66, 0x85, 0xD8}, 3u, 2u, 1u},
    {"test eax, ebx", {0x85, 0xD8}, 2u, 4u, 1u},
    {"add al, bl", {0x00, 0xD8}, 2u, 1u, 1u},
    {"add ax, bx", {0x66, 0x01, 0xD8}, 3u, 2u, 1u},
    {"add eax, ebx", {0x01, 0xD8}, 2u, 4u, 1u},
    {"stc; inc al", {0xF9, 0xFE, 0xC0}, 3u, 1u, 2u},
    {"clc; inc ax", {0xF8, 0x66, 0x40}, 3u, 2u, 2u},
    {"stc; inc eax", {0xF9, 0x40}, 2u, 4u, 2u},
    {"clc; inc eax", {0xF8, 0x40}, 2u, 4u, 2u},
    {"clc; dec al", {0xF8, 0xFE, 0xC8}, 3u, 1u, 2u},
    {"stc; dec ax", {0xF9, 0x66, 0x48}, 3u, 2u, 2u},
    {"stc; dec eax", {0xF9, 0x48}, 2u, 4u, 2u},
    {"clc; dec eax", {0xF8, 0x48}, 2u, 4u, 2u},
};

static const uint32_t kOperands[][2] = {
    {0u, 0u},
    {1u, 0u},
    {0u, 1u},
    {5u, 5u},
    {0x7FFFFFFFu, 0xFFFFFFFFu},
    {0x80000000u, 1u},
    {0x80000000u, 0x7FFFFFFFu},
    {0xFFFFFFFFu, 0xFFFFFFFFu},
    {0xFFFFFFFFu, 1u},
    {0x0000007Fu, 0x00000080u},
    {0x00000080u, 0x00000080u},
    {0x000000FFu, 0x00000001u},
    {0x00008000u, 0x00007FFFu},
    {0x00001234u, 0x00001234u},
    {0x0000AA55u, 0x000055AAu},
    {0x00000003u, 0x80000000u},
    {0x12345678u, 0x12345679u},
    {0x0000FF00u, 0x000000FFu},
};

static uint32_t seeded(uint32_t value, unsigned width) {
  /* At 32 bits the value IS the operand; narrower, the high bits are junk the
     comparison must ignore and the sign must come from the operand's own top
     bit rather than from bit 31. */
  if (width == 4u) {
    return value;
  }
  return kHighJunk | (value & (width == 1u ? 0xFFu : 0xFFFFu));
}

static void every_condition_after_every_form(void) {
  unsigned f, cc, v;
  for (f = 0; f < sizeof kForms / sizeof kForms[0]; ++f) {
    for (cc = 0; cc < (unsigned)kX86pCondCount; ++cc) {
      for (v = 0; v < sizeof kOperands / sizeof kOperands[0]; ++v) {
        uint8_t code[6];
        char name[96];
        X86pCpu cpu = wasm_test_initial(&suite);
        memcpy(code, kForms[f].bytes, kForms[f].size);
        code[kForms[f].size + 0u] = 0x0F;
        code[kForms[f].size + 1u] = (uint8_t)(0x90u + cc); /* SETcc */
        code[kForms[f].size + 2u] = 0xC1;                  /* ...cl */
        cpu.reg[kX86pEax] = seeded(kOperands[v][0], kForms[f].width);
        cpu.reg[kX86pEbx] = seeded(kOperands[v][1], kForms[f].width);
        cpu.reg[kX86pEcx] = 0xC0FFEE00u; /* so a SETcc that wrote nothing shows */
        snprintf(name,
                 sizeof name,
                 "%s then SET%s, %08x/%08x",
                 kForms[f].name,
                 x86p_cond_name((X86pCond)cc),
                 kOperands[v][0],
                 kOperands[v][1]);
        wasm_test_case_insns(&suite, name, code, kForms[f].size + 3u, cpu, 0, kForms[f].insns + 1u);
      }
    }
  }
}

/* The same shapes as a branch rather than a materialised 0/1: Jcc reads the
   condition through the same owner, and a sign error there changes control
   flow rather than one byte of a register. */
static void every_condition_as_a_branch(void) {
  unsigned f, cc, v;
  for (f = 0; f < sizeof kForms / sizeof kForms[0]; ++f) {
    for (cc = 0; cc < (unsigned)kX86pCondCount; ++cc) {
      for (v = 0; v < 6u; ++v) {
        uint8_t code[6];
        char name[96];
        X86pCpu cpu = wasm_test_initial(&suite);
        memcpy(code, kForms[f].bytes, kForms[f].size);
        code[kForms[f].size + 0u] = (uint8_t)(0x70u + cc); /* Jcc rel8 */
        code[kForms[f].size + 1u] = 0x02;
        cpu.reg[kX86pEax] = seeded(kOperands[v][0], kForms[f].width);
        cpu.reg[kX86pEbx] = seeded(kOperands[v][1], kForms[f].width);
        snprintf(name,
                 sizeof name,
                 "%s then J%s, %08x/%08x",
                 kForms[f].name,
                 x86p_cond_name((X86pCond)cc),
                 kOperands[v][0],
                 kOperands[v][1]);
        wasm_test_case_insns(&suite, name, code, kForms[f].size + 2u, cpu, 1, kForms[f].insns + 1u);
      }
    }
  }
}

/* ---- which path was taken, asserted in both directions ------------------- */

enum { kGuestLo = 0x400000u, kArenaSize = 4096u, kModuleSize = 65536u };
static uint8_t g_arena[kArenaSize];
static uint8_t g_module[kModuleSize];

static int lower_counts(const uint8_t *code, unsigned size, X86pJitBlock *out) {
  X86pMem mem = {0};
  X86pWasmModule module;
  X86pWasmPlan plan = {0};
  char reason[256] = {0};
  memset(g_arena, 0, sizeof g_arena);
  memcpy(g_arena, code, size);
  g_arena[size] = 0xEB; /* JMP $, so the block terminates */
  g_arena[size + 1u] = 0xFE;
  mem.host = g_arena;
  mem.lo = kGuestLo;
  mem.size = sizeof g_arena;
  plan.base = 0;
  plan.lo = kGuestLo;
  plan.size = sizeof g_arena;
  x86p_wasm_module_init(&module, g_module, sizeof g_module, 1u);
  if (x86p_wasm_lower_block(&module, &mem, &plan, kGuestLo, NULL, NULL, NULL, NULL, out, reason, sizeof reason) !=
      kX86pJitOk) {
    printf("FAIL: lowering refused: %s\n", reason);
    return 0;
  }
  return 1;
}

static void the_inline_path_is_the_one_being_tested(void) {
  /* CMP then SETcc: the predecessor is known, so this must be inline. */
  static const uint8_t after_cmp[] = {0x39, 0xD8, 0x0F, 0x9C, 0xC1};
  /* SETcc alone: the block's first instruction, predecessor unknown, so this
     must NOT be -- which is the other answer this check has to be able to
     give. A suite where everything fell back would otherwise look identical. */
  static const uint8_t alone[] = {0x0F, 0x9C, 0xC1};
  /* SHL by one then SETcc: a recorded kind whose derivation is not
     implemented, so the fall-back is reached through a path the block's first
     instruction does not exercise. */
  static const uint8_t after_shl[] = {0xD1, 0xE0, 0x0F, 0x9C, 0xC1};
  /* SHL by CL then SETcc: a count of zero would write no flags, so the shift
     records no kind at translation time. */
  static const uint8_t after_shl_cl[] = {0xD3, 0xE0, 0x0F, 0x9C, 0xC1};
  /* ADD, INC and DEC then SETcc: derivable kinds of their own since #168. */
  static const uint8_t after_add[] = {0x01, 0xD8, 0x0F, 0x9C, 0xC1};
  static const uint8_t after_inc[] = {0x40, 0x0F, 0x92, 0xC1};
  static const uint8_t after_dec[] = {0x48, 0x0F, 0x9F, 0xC1};
  X86pJitBlock block;

  suite.current = "counters";
  if (lower_counts(after_add, sizeof after_add, &block)) {
    wasm_test_check(&suite, block.cond_inline == 1u && block.cond_helper_calls == 0u, "ADD+SETL was not inline");
  }
  if (lower_counts(after_inc, sizeof after_inc, &block)) {
    wasm_test_check(&suite, block.cond_inline == 1u && block.cond_helper_calls == 0u, "INC+SETB was not inline");
  }
  if (lower_counts(after_dec, sizeof after_dec, &block)) {
    wasm_test_check(&suite, block.cond_inline == 1u && block.cond_helper_calls == 0u, "DEC+SETG was not inline");
  }
  if (lower_counts(after_cmp, sizeof after_cmp, &block)) {
    wasm_test_check(&suite, block.conds == 1u, "CMP+SETcc did not count one condition");
    wasm_test_check(&suite, block.cond_inline == 1u, "CMP+SETcc was not lowered inline");
    wasm_test_check(&suite, block.cond_helper_calls == 0u, "CMP+SETcc still called the authority");
    wasm_test_check(&suite, block.cond_unknown_kind == 0u, "CMP+SETcc reported its predecessor as unrecorded");
  }
  if (lower_counts(alone, sizeof alone, &block)) {
    wasm_test_check(&suite, block.conds == 1u, "a lone SETcc did not count one condition");
    wasm_test_check(&suite, block.cond_inline == 0u, "a lone SETcc claimed an inline lowering it cannot have");
    wasm_test_check(&suite, block.cond_helper_calls == 1u, "a lone SETcc did not reach the authority");
    wasm_test_check(&suite, block.cond_unknown_kind == 1u, "a lone SETcc did not report an unrecorded predecessor");
  }
  if (lower_counts(after_shl, sizeof after_shl, &block)) {
    wasm_test_check(&suite, block.cond_inline == 0u, "a shift's flags were treated as a derivable kind");
    wasm_test_check(&suite, block.cond_helper_calls == 1u, "SHL+SETcc did not reach the authority");
    wasm_test_check(&suite, block.cond_unknown_kind == 0u, "an immediate shift's kind was reported as unrecorded");
  }
  if (lower_counts(after_shl_cl, sizeof after_shl_cl, &block)) {
    wasm_test_check(&suite, block.cond_helper_calls == 1u, "SHL CL+SETcc did not reach the authority");
    wasm_test_check(
        &suite, block.cond_unknown_kind == 1u, "a CL shift, which records no kind, was counted as classified");
  }
  wasm_test_check(&suite, !x86p_wasm_cond_is_inline(-1, kX86pCondZ), "an unknown predecessor claimed an inline form");
  wasm_test_check(&suite,
                  x86p_wasm_cond_is_inline((int)kX86pFlagsSub, kX86pCondZ) &&
                      x86p_wasm_cond_is_inline((int)kX86pFlagsLogic, kX86pCondZ) &&
                      x86p_wasm_cond_is_inline((int)kX86pFlagsAdd, kX86pCondZ) &&
                      x86p_wasm_cond_is_inline((int)kX86pFlagsInc, kX86pCondZ) &&
                      x86p_wasm_cond_is_inline((int)kX86pFlagsDec, kX86pCondZ),
                  "the five derivable kinds were not reported as derivable");
  wasm_test_check(&suite,
                  !x86p_wasm_cond_is_inline((int)kX86pFlagsExplicit, kX86pCondZ) &&
                      !x86p_wasm_cond_is_inline((int)kX86pFlagsShl, kX86pCondZ),
                  "a kind with no inline derivation claimed one");
  wasm_test_check(&suite,
                  !x86p_wasm_cond_is_inline((int)kX86pFlagsSub, (X86pCond)kX86pCondCount),
                  "a condition number off the end claimed an inline form");
}

int main(void) {
  every_condition_after_every_form();
  every_condition_as_a_branch();
  the_inline_path_is_the_one_being_tested();
  suite.current = "coverage";
  wasm_test_check(
      &suite, suite.entered == suite.cases && suite.entered > 600u, "translated entry denominator incomplete");
  printf("WASM cond: cases=%u translated_entries=%u checks=%u failures=%u\n",
         suite.cases,
         suite.entered,
         suite.checks,
         suite.failures);
  return suite.failures ? 1 : 0;
}
