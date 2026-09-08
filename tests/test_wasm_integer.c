#include "decode.h"
#include "jit_wasm_lower.h"
#include "wasm_differential.h"
#include <stdio.h>
#include <string.h>

static WasmTest suite;

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
      X86pCpu cpu = wasm_test_initial(&suite);
      cpu.reg[kX86pEax] = values[v][0];
      cpu.reg[kX86pEbx] = values[v][1];
      cpu.reg[kX86pEdx] = values[v][2];
      memcpy(suite.guest + 512u, &values[v][1], sizeof(uint32_t));
      wasm_test_case(&suite, forms[f].name, forms[f].bytes, forms[f].size, cpu, 0);
    }
  }
  {
    static const uint8_t mul[] = {0xf7, 0x27}, div[] = {0xf7, 0x37};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 2u;
    wasm_test_case(&suite, "multiply read fault", mul, sizeof mul, cpu, 0);
    cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEdi] = GUEST_BASE - 1u;
    wasm_test_case(&suite, "divide read fault", div, sizeof div, cpu, 0);
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
        X86pCpu cpu = wasm_test_initial(&suite);
        cpu.df = (uint8_t)direction;
        if (repeat == 0) {
          cpu.reg[kX86pEcx] = 0;
        }
        wasm_test_case(
            &suite, "string width/repeat/direction", code + (repeat == 2), sizeof code - (repeat == 2), cpu, 0);
      }
    }
  }
  {
    static const uint8_t movs[] = {0xf3, 0xa5}, scas[] = {0xf2, 0xae};
    X86pCpu cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 8;
    wasm_test_case(&suite, "REP MOVSD partial write fault", movs, sizeof movs, cpu, 0);
    cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEsi] = GUEST_BASE + GUEST_SIZE - 8;
    wasm_test_case(&suite, "REP MOVSD partial read fault", movs, sizeof movs, cpu, 0);
    cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEax] = suite.guest[514];
    wasm_test_case(&suite, "REPNE SCAS finds match", scas, sizeof scas, cpu, 0);
    cpu = wasm_test_initial(&suite);
    cpu.reg[kX86pEax] = suite.guest[511];
    wasm_test_case(&suite, "REPNE SCAS exhausts count", scas, sizeof scas, cpu, 0);
  }
}

static void loops_and_flags(void) {
  unsigned op, width, zf, count;
  for (op = 0xe0; op <= 0xe2; ++op) {
    for (width = 0; width < 2; ++width) {
      for (zf = 0; zf < 2; ++zf) {
        for (count = 0; count < 3; ++count) {
          uint8_t code[] = {0x67, (uint8_t)op, 7};
          X86pCpu cpu = wasm_test_initial(&suite);
          cpu.reg[kX86pEcx] = width ? 0xabcd0000u + count : count;
          x86p_flags_set_explicit(&cpu.flags, X86P_CF | (zf ? X86P_ZF : 0u));
          wasm_test_case(&suite, "LOOP condition/counter width", code + !width, sizeof code - !width, cpu, 1);
        }
      }
    }
  }
  for (op = 0x9c; op <= 0x9d; ++op) {
    for (zf = 0; zf < 2; ++zf) {
      uint8_t code = (uint8_t)op;
      uint32_t flags = X86P_CF | X86P_OF | (zf ? X86P_DF : X86P_ZF);
      X86pCpu cpu = wasm_test_initial(&suite);
      cpu.df = (uint8_t)zf;
      memcpy(suite.guest + 1024, &flags, sizeof flags);
      wasm_test_case(&suite, "PUSHFD/POPFD direction and arithmetic flags", &code, 1, cpu, 0);
      cpu = wasm_test_initial(&suite);
      cpu.reg[kX86pEsp] = op == 0x9c ? GUEST_BASE + 2 : GUEST_BASE + GUEST_SIZE - 2;
      wasm_test_case(&suite, "PUSHFD/POPFD memory fault preserves state", &code, 1, cpu, 0);
    }
  }
}

static void refuses_unimplemented_shape(void) {
  static const uint8_t code[] = {0x67, 0xf3, 0xa5};
  X86pInsn insn;
  suite.current = "16-bit string addressing refusal";
  wasm_test_check(&suite, x86p_decode(code, sizeof code, &insn) != 0u, "decoder rejected test bytes");
  wasm_test_check(&suite, !x86p_wasm_can_lower(&insn), "16-bit string addresses were admitted as 32-bit");
}

int main(void) {
  arithmetic();
  strings();
  loops_and_flags();
  refuses_unimplemented_shape();
  suite.current = "coverage";
  wasm_test_check(
      &suite, suite.entered == suite.cases && suite.entered > 200u, "translated entry denominator incomplete");
  wasm_test_check(&suite, suite.faults > 20u, "negative fault discriminator was not exercised");
  printf("WASM integer: cases=%u translated_entries=%u faults=%u checks=%u failures=%u\n",
         suite.cases,
         suite.entered,
         suite.faults,
         suite.checks,
         suite.failures);
  return suite.failures ? 1 : 0;
}
