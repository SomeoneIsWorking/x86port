#include "wasm_differential.h"
#include "exec.h"
#include "jit_engine.h"
#include <stdio.h>
#include <string.h>

void wasm_test_check(WasmTest *suite, int value, const char *message) {
  ++suite->checks;
  if (!value) {
    ++suite->failures;
    printf("FAIL: %s: %s\n", suite->current, message);
  }
}

X86pCpu wasm_test_initial(WasmTest *suite) {
  X86pCpu cpu;
  unsigned i;
  x86p_cpu_reset(&cpu);
  cpu.eip = GUEST_BASE;
  cpu.reg[kX86pEax] = 23u;
  cpu.reg[kX86pEbx] = 7u;
  cpu.reg[kX86pEcx] = 4u;
  cpu.reg[kX86pEdx] = 0u;
  cpu.reg[kX86pEsi] = GUEST_BASE + 256u;
  cpu.reg[kX86pEdi] = GUEST_BASE + 512u;
  cpu.reg[kX86pEsp] = GUEST_BASE + 1024u;
  x86p_flags_set_explicit(&cpu.flags, X86P_CF | X86P_OF | X86P_ZF);
  for (i = 0; i < GUEST_SIZE; ++i) {
    suite->guest[i] = (uint8_t)(i * 17u + 3u);
  }
  return cpu;
}

void wasm_test_case(WasmTest *suite, const char *name, const uint8_t *code, size_t size, X86pCpu cpu, int branch) {
  X86pMem mem = {.host = suite->guest, .lo = GUEST_BASE, .size = sizeof suite->guest};
  X86pMem oracle_mem = {.host = suite->reference, .lo = GUEST_BASE, .size = sizeof suite->reference};
  wasm_test_case_mem(suite, name, code, size, cpu, branch, &mem, &oracle_mem);
}

void wasm_test_case_mem(WasmTest *suite,
                        const char *name,
                        const uint8_t *code,
                        size_t size,
                        X86pCpu cpu,
                        int branch,
                        const X86pMem *mem,
                        const X86pMem *oracle_mem) {
  wasm_test_case_mem_insns(suite, name, code, size, cpu, branch, 1u, mem, oracle_mem);
}

void wasm_test_case_insns(
    WasmTest *suite, const char *name, const uint8_t *code, size_t size, X86pCpu cpu, int branch, unsigned insns) {
  X86pMem mem = {.host = suite->guest, .lo = GUEST_BASE, .size = sizeof suite->guest};
  X86pMem oracle_mem = {.host = suite->reference, .lo = GUEST_BASE, .size = sizeof suite->reference};
  wasm_test_case_mem_insns(suite, name, code, size, cpu, branch, insns, &mem, &oracle_mem);
}

void wasm_test_case_mem_insns(WasmTest *suite,
                              const char *name,
                              const uint8_t *code,
                              size_t size,
                              X86pCpu cpu,
                              int branch,
                              unsigned insns,
                              const X86pMem *mem,
                              const X86pMem *oracle_mem) {
  X86pCpu oracle = cpu;
  X86pJitEngineStats stats;
  X86pStepReport report;
  X86pStepStatus expected;
  X86pJitRunStatus actual;
  X86pJitEngine *engine;
  char reason[256] = {0};
  suite->current = name;
  ++suite->cases;
  memcpy(suite->guest, code, size);
  suite->guest[size] = 0xeb; /* terminate every nonbranch case with JMP $ */
  suite->guest[size + 1u] = 0xfe;
  memcpy(suite->reference, suite->guest, sizeof suite->reference);
  /*
   * The oracle steps the guest instructions in `code`, then the appended
   * JMP $ that a non-branch case ends on. A case whose whole point is what the
   * SECOND instruction does with the first one's flags needs more than one, so
   * the count is the caller's -- but it stops at the first non-Ok result, so a
   * fault in the middle is still reported as the fault and not stepped past.
   */
  {
    unsigned remaining = insns + (branch ? 0u : 1u);
    expected = kX86pStepOk;
    while (remaining-- > 0u && expected == kX86pStepOk) {
      expected = x86p_step(&oracle, oracle_mem, &report);
    }
  }
  engine = x86p_jit_engine_create(mem, 65536u, 128u, reason, sizeof reason);
  wasm_test_check(suite, engine != NULL, reason);
  if (!engine) {
    return;
  }
  actual = x86p_jit_engine_run(engine, &cpu, NULL, 1u, reason, sizeof reason);
  wasm_test_check(suite,
                  (expected == kX86pStepOk && actual == kX86pRunBudget) ||
                      (expected == kX86pStepMemoryFault && actual == kX86pRunMemoryFault) ||
                      (expected == kX86pStepDivideError && actual == kX86pRunDivideError) ||
                      (expected == kX86pStepUnsupported && actual == kX86pRunUnsupported) ||
                      (expected == kX86pStepInterrupt && actual == kX86pRunInterrupt) ||
                      (expected == kX86pStepProtectionFault && actual == kX86pRunProtectionFault),
                  reason[0] ? reason : "exit status disagrees with the oracle");
  if (expected != kX86pStepOk) {
    ++suite->faults;
  }
  wasm_test_check(suite, cpu.eip == oracle.eip, "EIP differs");
  wasm_test_check(suite, memcmp(cpu.reg, oracle.reg, sizeof cpu.reg) == 0, "registers differ");
  wasm_test_check(suite, x86p_eflags(&cpu.flags) == x86p_eflags(&oracle.flags), "arithmetic flags differ");
  wasm_test_check(suite, cpu.df == oracle.df, "direction flag differs");
  wasm_test_check(suite, memcmp(suite->guest, suite->reference, sizeof suite->guest) == 0, "guest memory differs");
  wasm_test_check(suite, memcmp(cpu.xmm, oracle.xmm, sizeof cpu.xmm) == 0, "XMM lanes differ");
  wasm_test_check(suite, cpu.mxcsr == oracle.mxcsr, "MXCSR differs");
  wasm_test_check(suite,
                  cpu.x87.top == oracle.x87.top && memcmp(cpu.x87.tag, oracle.x87.tag, sizeof cpu.x87.tag) == 0,
                  "x87 stack identity/tags differ");
  x86p_jit_engine_stats(engine, &stats);
  wasm_test_check(suite,
                  stats.blocks_entered == 1u && stats.blocks_translated == 1u,
                  "case did not execute exactly one translated block");
  wasm_test_check(suite,
                  stats.translate_refusals == (expected == kX86pStepUnsupported ? 1u : 0u),
                  "runtime refusal count disagrees with the oracle's named unsupported result");
  suite->entered += (unsigned)stats.blocks_entered;
  suite->simd_ops += (unsigned long)stats.simd_translated;
  suite->simd_inline += (unsigned long)stats.simd_inline;
  wasm_test_check(suite, cpu.trap_vector == oracle.trap_vector, "trap vector differs");
  suite->last_cpu = cpu;
  x86p_jit_engine_destroy(engine);
}
