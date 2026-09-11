#include "decode.h"
#include "jit_engine.h"
#include "jit_wasm_lower.h"
#include "memory_sparse.h"
#include "stack_ops.h"
#include "wasm_differential.h"
#include <stdio.h>
#include <string.h>

static WasmTest suite;

static void shifts(void) {
  static const uint8_t counts[] = {0, 1, 7, 16, 17, 31, 32, 255};
  unsigned width, direction, memory, i;
  for (width = 0; width < 2; ++width) {
    for (direction = 0; direction < 2; ++direction) {
      for (memory = 0; memory < 2; ++memory) {
        for (i = 0; i < sizeof counts; ++i) {
          uint8_t code[] = {0x66, 0x0f, direction ? 0xad : 0xa5, memory ? 0x1f : 0xd8};
          X86pCpu cpu = wasm_test_initial(&suite);
          cpu.reg[kX86pEax] = 0x87651234u;
          cpu.reg[kX86pEbx] = 0xfedcba98u;
          cpu.reg[kX86pEcx] = counts[i];
          wasm_test_case(&suite, "double shift width/count/direction", code + width, sizeof code - width, cpu, 0);
        }
      }
    }
  }
  {
    const uint8_t code[] = {0x0f, 0xa4, 0x1f, 1};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 2;
    wasm_test_case(&suite, "SHLD memory read fault", code, sizeof code, cpu, 0);
  }
}

static void bits(void) {
  static const uint8_t opcodes[] = {0xa3, 0xab, 0xb3, 0xbb};
  static const uint32_t offsets[] = {0, 1, 15, 16, 31, 32, 65, UINT32_MAX, UINT32_MAX - 32, UINT32_MAX - 64};
  unsigned op, i;
  for (op = 0; op < sizeof opcodes; ++op) {
    for (i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
      uint8_t reg[] = {0x66, 0x0f, opcodes[op], 0xd8};
      uint8_t mem[] = {0x0f, opcodes[op], 0x1f};
      uint8_t imm[] = {0x66, 0x0f, 0xba, (uint8_t)(0x27 + op * 8), (uint8_t)offsets[i]};
      X86pCpu cpu = wasm_test_initial(&suite);
      cpu.reg[kX86pEbx] = offsets[i];
      cpu.reg[kX86pEax] = 0xa5a56969u;
      wasm_test_case(&suite, "bit word register index", reg, sizeof reg, cpu, 0);
      cpu = wasm_test_initial(&suite);
      cpu.reg[kX86pEbx] = offsets[i];
      wasm_test_case(&suite, "bit signed memory index", mem, sizeof mem, cpu, 0);
      cpu = wasm_test_initial(&suite);
      wasm_test_case(&suite, "bit word memory immediate index", imm, sizeof imm, cpu, 0);
    }
  }
  {
    const uint8_t code[] = {0x0f, 0xab, 0x1f};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 4;
    cpu.reg[kX86pEbx] = 32;
    wasm_test_case(&suite, "BTS adjusted memory address faults", code, sizeof code, cpu, 0);
  }
}

static void bcd(void) {
  static const uint8_t code[][2] = {
      {0x27, 0}, {0x2f, 0}, {0x37, 0}, {0x3f, 0}, {0xd4, 10}, {0xd5, 10}, {0xd4, 0}, {0xd5, 0}, {0xd4, 16}};
  unsigned i, flags;
  for (i = 0; i < sizeof code / sizeof code[0]; ++i) {
    for (flags = 0; flags < 4; ++flags) {
      X86pCpu cpu = wasm_test_initial(&suite);
      cpu.reg[kX86pEax] = 0x5678ff9bu;
      x86p_flags_set_explicit(&cpu.flags, (flags & 1 ? X86P_AF : 0u) | (flags & 2 ? X86P_CF : 0u));
      wasm_test_case(&suite, "BCD adjustments and zero-base divide", code[i], i < 4 ? 1 : 2, cpu, 0);
    }
  }
}

static void cmov_flags(void) {
  unsigned condition, flags;
  static const uint8_t flag_ops[] = {0x9e, 0x9f, 0xf8, 0xf9, 0xf5, 0xd6};
  for (condition = 0; condition < 16; ++condition) {
    for (flags = 0; flags < 2; ++flags) {
      uint8_t code[] = {0x0f, (uint8_t)(0x40 + condition), 0x07};
      X86pCpu cpu = wasm_test_initial(&suite);
      x86p_flags_set_explicit(&cpu.flags, flags ? 0x8d5u : 0u);
      wasm_test_case(&suite, "CMOV all conditions", code, sizeof code, cpu, 0);
    }
  }
  for (flags = 0; flags < 2; ++flags) {
    uint8_t code[] = {0x66, 0x0f, 0x44, 0x07};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 1;
    x86p_flags_set_explicit(&cpu.flags, flags ? X86P_ZF : 0u);
    wasm_test_case(&suite, "CMOV fault independent of condition", code, sizeof code, cpu, 0);
  }
  for (condition = 0; condition < sizeof flag_ops; ++condition) {
    for (flags = 0; flags < 2; ++flags) {
      X86pCpu cpu = wasm_test_initial(&suite);
      cpu.reg[kX86pEax] = 0x1234d501u;
      cpu.df = 1;
      x86p_flags_set_explicit(&cpu.flags, flags ? 0x8d5u : 0u);
      wasm_test_case(&suite, "status flags transfers preserve DF", &flag_ops[condition], 1, cpu, 0);
    }
  }
}

static void stack(void) {
  unsigned op, nesting;
  for (op = 0x60; op <= 0x61; ++op) {
    uint8_t code = (uint8_t)op;
    X86pCpu cpu = wasm_test_initial(&suite);
    wasm_test_case(&suite, "PUSHAD/POPAD valid", &code, 1, cpu, 0);
    wasm_test_check(&suite,
                    suite.last_cpu.reg[kX86pEsp] == GUEST_BASE + (op == 0x60 ? 992u : 1056u),
                    "stack pointer must advance by eight slots");
    if (op == 0x60) {
      uint32_t saved_esp;
      memcpy(&saved_esp, suite.guest + 1004, 4);
      wasm_test_check(&suite, saved_esp == GUEST_BASE + 1024, "PUSHAD saved a decremented ESP");
    }
    cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEsp] = op == 0x60 ? GUEST_BASE + 12 : GUEST_BASE + GUEST_SIZE - 12;
    wasm_test_case(&suite, "PUSHAD/POPAD partial fault", &code, 1, cpu, 0);
    wasm_test_check(&suite,
                    suite.last_cpu.reg[kX86pEsp] == (op == 0x60 ? GUEST_BASE : GUEST_BASE + GUEST_SIZE),
                    "stack fault lost completed accesses");
  }
  for (nesting = 0; nesting < 4; ++nesting) {
    uint8_t code[] = {0xc8, 0x20, 0, (uint8_t)(nesting == 3 ? 31 : nesting)};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEbp] = GUEST_BASE + 2048;
    wasm_test_case(&suite, "ENTER nesting", code, sizeof code, cpu, 0);
    wasm_test_check(&suite, suite.last_cpu.reg[kX86pEbp] == GUEST_BASE + 1020, "ENTER frame pointer differs");
  }
  {
    const uint8_t code[] = {0xc8, 0x20, 0, 3};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEbp] = GUEST_BASE + 4;
    wasm_test_case(&suite, "ENTER nesting read fault", code, sizeof code, cpu, 0);
  }
  {
    X86pCpu cpu = wasm_test_initial(&suite);
    X86pMem mem = {.host = suite.guest, .lo = GUEST_BASE, .size = sizeof suite.guest};
    uint32_t fault = 0;
    cpu.reg[kX86pEbp] = GUEST_BASE + 2048;
    cpu.reg[kX86pEsp] = GUEST_BASE + 4;
    suite.current = "ENTER failed nesting push diagnostic";
    wasm_test_check(&suite, !x86p_stack_enter(&cpu, &mem, 0, 2, &fault), "ENTER accepted an unmapped stack push");
    wasm_test_check(&suite, fault == GUEST_BASE - 4, "failed nested push must report stack destination");
  }
}

static void architectural_exits(void) {
  static const struct {
    uint8_t code[2];
    size_t size;
  } forms[] = {{{0xcc, 0}, 1},
               {{0xf1, 0}, 1},
               {{0xcd, 0x80}, 2},
               {{0xce, 0}, 1},
               {{0xf4, 0}, 1},
               {{0xfa, 0}, 1},
               {{0xfb, 0}, 1},
               {{0x0f, 0x09}, 2},
               {{0xe4, 0x80}, 2},
               {{0xe6, 0x80}, 2}};
  unsigned i, flags;
  for (i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
    for (flags = 0; flags < 2; ++flags) {
      X86pCpu cpu = wasm_test_initial(&suite);
      x86p_flags_set_explicit(&cpu.flags, flags ? X86P_OF : 0u);
      wasm_test_case(&suite, "architectural interrupt/protection", forms[i].code, forms[i].size, cpu, 1);
    }
  }
  {
    const uint8_t cpuid[] = {0x0f, 0xa2}, rdtsc[] = {0x0f, 0x31};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEax] = 0;
    wasm_test_case(&suite, "CPUID", cpuid, sizeof cpuid, cpu, 0);
    cpu = wasm_test_initial(&suite);
    wasm_test_case(&suite, "RDTSC", rdtsc, sizeof rdtsc, cpu, 0);
  }
}

static void permissions(void) {
  static const struct {
    uint8_t code[4];
    unsigned size, count;
    int faults;
  } forms[] = {{{0x0f, 0xa5, 0x1f, 0}, 3, 0, 0},
               {{0x0f, 0xa5, 0x1f, 0}, 3, 1, 1},
               {{0x0f, 0xad, 0x1f, 0}, 3, 0, 0},
               {{0x0f, 0xad, 0x1f, 0}, 3, 1, 1},
               {{0x0f, 0xba, 0x27, 1}, 4, 0, 0},
               {{0x0f, 0xba, 0x2f, 1}, 4, 0, 1},
               {{0x0f, 0xba, 0x37, 1}, 4, 0, 1},
               {{0x0f, 0xba, 0x3f, 1}, 4, 0, 1}};
  unsigned i;
  for (i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
    X86pCpu cpu = wasm_test_initial(&suite), before;
    X86pSparseMem *sparse = x86p_sparse_create();
    X86pMem mem = {.sparse = sparse};
    X86pJitEngine *engine;
    X86pJitEngineStats stats;
    X86pJitRunStatus status;
    char reason[256] = {0};
    suite.current = "read-only shift/bit operand";
    ++suite.cases;
    wasm_test_check(&suite, sparse != NULL, "cannot allocate sparse memory");
    if (!sparse) {
      continue;
    }
    wasm_test_check(&suite, x86p_sparse_map(sparse, GUEST_BASE, suite.guest, GUEST_SIZE), "map failed");
    wasm_test_check(&suite, x86p_sparse_protect(sparse, GUEST_BASE + 512, 4, kX86pMemRead), "protect failed");
    cpu.reg[kX86pEcx] = forms[i].count;
    before = cpu;
    memcpy(suite.guest, forms[i].code, forms[i].size);
    suite.guest[forms[i].size] = 0xeb;
    suite.guest[forms[i].size + 1] = 0xfe;
    memcpy(suite.reference, suite.guest, sizeof suite.guest);
    engine = x86p_jit_engine_create(&mem, 65536, 128, reason, sizeof reason);
    wasm_test_check(&suite, engine != NULL, reason);
    if (!engine) {
      x86p_sparse_destroy(sparse);
      continue;
    }
    status = x86p_jit_engine_run(engine, &cpu, NULL, 1, reason, sizeof reason);
    wasm_test_check(&suite, status == (forms[i].faults ? kX86pRunMemoryFault : kX86pRunBudget), reason);
    wasm_test_check(
        &suite, memcmp(suite.guest, suite.reference, sizeof suite.guest) == 0, "read-only operand changed memory");
    if (forms[i].faults) {
      ++suite.faults;
      wasm_test_check(&suite, memcmp(&cpu, &before, sizeof cpu) == 0, "write refusal changed CPU/flags");
    }
    x86p_jit_engine_stats(engine, &stats);
    suite.entered += (unsigned)stats.blocks_entered;
    wasm_test_check(&suite,
                    stats.blocks_entered == 1 && stats.blocks_translated == 1,
                    "permission case did not enter generated code");
    x86p_jit_engine_destroy(engine);
    x86p_sparse_destroy(sparse);
  }
}

int main(void) {
  shifts();
  bits();
  bcd();
  cmov_flags();
  stack();
  architectural_exits();
  permissions();
  suite.current = "integer tail coverage";
  wasm_test_check(&suite, suite.cases > 250 && suite.entered == suite.cases, "translated case denominator incomplete");
  wasm_test_check(&suite, suite.faults > 20, "fault discriminator did not execute");
  printf("WASM integer tail: cases=%u entries=%u faults=%u checks=%u failures=%u\n",
         suite.cases,
         suite.entered,
         suite.faults,
         suite.checks,
         suite.failures);
  return suite.failures ? 1 : 0;
}
