/* Differential cases enter the shipping WASM module, with the interpreter
 * linked only to this test as an independent decode/execution driver. */
#include "cpu.h"
#include "exec.h"
#include "jit_engine.h"
#include "jit_wasm_lower.h"

#include <stdio.h>
#include <string.h>

#define GUEST_BASE 0x400000u
#define GUEST_SIZE 4096u
static uint8_t guest[GUEST_SIZE], reference[GUEST_SIZE];
static unsigned checks, failures, cases, entered, faults;
static const char *current;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s: %s\n", current, message);
  }
}

static X86pCpu initial(void) {
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
    guest[i] = (uint8_t)(i * 17u + 3u);
  }
  return cpu;
}

static void run_case(const char *name, const uint8_t *code, size_t size, X86pCpu cpu, int branch) {
  X86pMem mem = {.host = guest, .lo = GUEST_BASE, .size = sizeof guest};
  X86pMem oracle_mem = {.host = reference, .lo = GUEST_BASE, .size = sizeof reference};
  X86pCpu oracle = cpu;
  X86pJitEngineStats stats;
  X86pStepReport report;
  X86pStepStatus expected;
  X86pJitRunStatus actual;
  X86pJitEngine *engine;
  char reason[256] = {0};
  current = name;
  ++cases;
  memcpy(guest, code, size);
  guest[size] = 0xeb; /* terminate every nonbranch case with JMP $ */
  guest[size + 1u] = 0xfe;
  memcpy(reference, guest, sizeof reference);
  expected = x86p_step(&oracle, &oracle_mem, &report);
  if (expected == kX86pStepOk && !branch) {
    expected = x86p_step(&oracle, &oracle_mem, &report);
  }
  engine = x86p_jit_engine_create(&mem, 65536u, 128u, reason, sizeof reason);
  check(engine != NULL, reason);
  if (!engine) {
    return;
  }
  actual = x86p_jit_engine_run(engine, &cpu, 1u, reason, sizeof reason);
  check((expected == kX86pStepOk && actual == kX86pRunBudget) ||
            (expected == kX86pStepMemoryFault && actual == kX86pRunMemoryFault) ||
            (expected == kX86pStepDivideError && actual == kX86pRunDivideError),
        reason[0] ? reason : "exit status disagrees with the oracle");
  if (expected != kX86pStepOk) {
    ++faults;
  }
  check(cpu.eip == oracle.eip, "EIP differs");
  check(memcmp(cpu.reg, oracle.reg, sizeof cpu.reg) == 0, "registers differ");
  check(x86p_eflags(&cpu.flags) == x86p_eflags(&oracle.flags), "arithmetic flags differ");
  check(cpu.df == oracle.df, "direction flag differs");
  check(memcmp(guest, reference, sizeof guest) == 0, "guest memory differs");
  x86p_jit_engine_stats(engine, &stats);
  check(stats.blocks_entered == 1u && stats.blocks_translated == 1u && stats.translate_refusals == 0u,
        "case did not execute exactly one translated block");
  entered += (unsigned)stats.blocks_entered;
  x86p_jit_engine_destroy(engine);
}

static void arithmetic(void) {
  static const struct {
    const char *name;
    uint8_t bytes[4];
    size_t size;
  } forms[] = {{"mul8", {0xf6, 0xe3}, 2},
               {"mul16", {0x66, 0xf7, 0xe3}, 3},
               {"mul32", {0xf7, 0xe3}, 2},
               {"imul8", {0xf6, 0xeb}, 2},
               {"imul16", {0x66, 0xf7, 0xeb}, 3},
               {"imul32", {0xf7, 0xeb}, 2},
               {"div8", {0xf6, 0xf3}, 2},
               {"div16", {0x66, 0xf7, 0xf3}, 3},
               {"div32", {0xf7, 0xf3}, 2},
               {"idiv8", {0xf6, 0xfb}, 2},
               {"idiv16", {0x66, 0xf7, 0xfb}, 3},
               {"idiv32", {0xf7, 0xfb}, 2},
               {"imul16 pair", {0x66, 0x0f, 0xaf, 0xc3}, 4},
               {"imul32 pair", {0x0f, 0xaf, 0xc3}, 3},
               {"imul16 immediate", {0x66, 0x6b, 0xc3, 0xfb}, 4},
               {"imul32 immediate", {0x6b, 0xc3, 0xfb}, 3},
               {"mul memory", {0xf7, 0x27}, 2},
               {"div memory", {0xf7, 0x37}, 2}};
  static const uint32_t values[][3] = {{23u, 7u, 0u},
                                       {0u, 0u, 0u},
                                       {0xffffffffu, 2u, 0u},
                                       {0x80000000u, 0xffffffffu, 0xffffffffu},
                                       {0x8765ff80u, 0x7654ffffu, 0x9876ffffu},
                                       {0xffffffffu, 1u, 1u},
                                       {0xfffffff9u, 3u, 0xffffffffu}};
  size_t f, v;
  for (f = 0; f < sizeof forms / sizeof forms[0]; ++f) {
    for (v = 0; v < sizeof values / sizeof values[0]; ++v) {
      X86pCpu cpu = initial();
      cpu.reg[kX86pEax] = values[v][0];
      cpu.reg[kX86pEbx] = values[v][1];
      cpu.reg[kX86pEdx] = values[v][2];
      memcpy(guest + 512u, &values[v][1], sizeof(uint32_t));
      run_case(forms[f].name, forms[f].bytes, forms[f].size, cpu, 0);
    }
  }
  {
    static const uint8_t mul[] = {0xf7, 0x27}, div[] = {0xf7, 0x37};
    X86pCpu cpu = initial();
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 2u;
    run_case("multiply read fault", mul, sizeof mul, cpu, 0);
    cpu = initial();
    cpu.reg[kX86pEdi] = GUEST_BASE - 1u;
    run_case("divide read fault", div, sizeof div, cpu, 0);
  }
}

static void strings(void) {
  static const uint8_t operations[] = {0xa4, 0xa5, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xa6, 0xa7};
  size_t i;
  unsigned direction, repeat;
  for (i = 0; i < sizeof operations; ++i) {
    for (direction = 0; direction < 2; ++direction) {
      for (repeat = 0; repeat < 3; ++repeat) {
        uint8_t code[3] = {0xf3, 0x66, operations[i]};
        X86pCpu cpu = initial();
        cpu.df = (uint8_t)direction;
        if (repeat == 0) {
          cpu.reg[kX86pEcx] = 0;
        }
        run_case("string width/repeat/direction", code + (repeat == 2), sizeof code - (repeat == 2), cpu, 0);
      }
    }
  }
  {
    static const uint8_t movs[] = {0xf3, 0xa5}, scas[] = {0xf2, 0xae};
    X86pCpu cpu = initial();
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 8;
    run_case("REP MOVSD partial write fault", movs, sizeof movs, cpu, 0);
    cpu = initial();
    cpu.reg[kX86pEsi] = GUEST_BASE + GUEST_SIZE - 8;
    run_case("REP MOVSD partial read fault", movs, sizeof movs, cpu, 0);
    cpu = initial();
    cpu.reg[kX86pEax] = guest[514];
    run_case("REPNE SCAS finds match", scas, sizeof scas, cpu, 0);
    cpu = initial();
    cpu.reg[kX86pEax] = guest[511];
    run_case("REPNE SCAS exhausts count", scas, sizeof scas, cpu, 0);
  }
}

static void loops_and_flags(void) {
  unsigned op, width, zf, count;
  for (op = 0xe0; op <= 0xe2; ++op) {
    for (width = 0; width < 2; ++width) {
      for (zf = 0; zf < 2; ++zf) {
        for (count = 0; count < 3; ++count) {
          uint8_t code[] = {0x67, (uint8_t)op, 7};
          X86pCpu cpu = initial();
          cpu.reg[kX86pEcx] = width ? 0xabcd0000u + count : count;
          x86p_flags_set_explicit(&cpu.flags, X86P_CF | (zf ? X86P_ZF : 0u));
          run_case("LOOP condition/counter width", code + !width, sizeof code - !width, cpu, 1);
        }
      }
    }
  }
  for (op = 0x9c; op <= 0x9d; ++op) {
    for (zf = 0; zf < 2; ++zf) {
      uint8_t code = (uint8_t)op;
      uint32_t flags = X86P_CF | X86P_OF | (zf ? X86P_DF : X86P_ZF);
      X86pCpu cpu = initial();
      cpu.df = (uint8_t)zf;
      memcpy(guest + 1024, &flags, sizeof flags);
      run_case("PUSHFD/POPFD direction and arithmetic flags", &code, 1, cpu, 0);
      cpu = initial();
      cpu.reg[kX86pEsp] = op == 0x9c ? GUEST_BASE + 2 : GUEST_BASE + GUEST_SIZE - 2;
      run_case("PUSHFD/POPFD memory fault preserves state", &code, 1, cpu, 0);
    }
  }
}

static void refuses_unimplemented_shape(void) {
  static const uint8_t code[] = {0x67, 0xf3, 0xa5};
  X86pInsn insn;
  current = "16-bit string addressing refusal";
  check(x86p_decode(code, sizeof code, &insn) != 0u, "decoder rejected test bytes");
  check(!x86p_wasm_can_lower(&insn), "16-bit string addresses were admitted as 32-bit");
}

int main(void) {
  arithmetic();
  strings();
  loops_and_flags();
  refuses_unimplemented_shape();
  current = "coverage";
  check(entered == cases && entered > 200u, "translated entry denominator incomplete");
  check(faults > 20u, "negative fault discriminator was not exercised");
  printf("WASM integer: cases=%u translated_entries=%u faults=%u checks=%u failures=%u\n",
         cases,
         entered,
         faults,
         checks,
         failures);
  return failures ? 1 : 0;
}
