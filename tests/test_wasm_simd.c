/* Actual WASM modules versus separately linked test-oracle instruction dispatch. */
#include "cpu.h"
#include "exec.h"
#include "jit_engine.h"
#include "jit_wasm_lower.h"
#include "memory_sparse.h"
#include "simd.h"
#include "wasm_differential.h"

#include <stdio.h>
#include <string.h>

static WasmTest suite;

static void check(int ok, const char *why) {
  wasm_test_check(&suite, ok, why);
}

static X86pCpu initial(unsigned pattern) {
  X86pCpu cpu;
  unsigned i, j;
  static const uint32_t values[] = {
      0u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0x3fa00000u, 0xc0600000u, 0x7fc00042u, 0x7f800000u};
  x86p_cpu_reset(&cpu);
  cpu.eip = GUEST_BASE;
  cpu.reg[kX86pEax] = 0x8ace1234u;
  cpu.reg[kX86pEcx] = 0x76543210u;
  cpu.reg[kX86pEdi] = GUEST_BASE + 512;
  cpu.x87.tag[3] = kX86pX87TagValid;
  cpu.x87.top = 3;
  x86p_flags_set_explicit(&cpu.flags, X86P_CF | X86P_OF | X86P_AF);
  for (i = 0; i < 8; ++i) {
    for (j = 0; j < 4; ++j) {
      uint32_t v = values[(i + j + pattern) % 8u];
      memcpy(cpu.xmm[i] + j * 4u, &v, 4);
    }
  }
  for (i = 0; i < GUEST_SIZE; ++i) {
    suite.guest[i] = (uint8_t)(i * 7u + pattern);
  }
  memcpy(suite.guest + 512, cpu.xmm[1], 16);
  return cpu;
}

/* Sparse mode splits the operand mid-dword, independently of its access width.
 * Mode 2 additionally denies writes to the last seven bytes of the operand. */
static X86pCpu run(const char *name, const uint8_t *code, size_t size, X86pCpu cpu, unsigned sparse_mode) {
  X86pMem mem = {.host = suite.guest, .lo = GUEST_BASE, .size = GUEST_SIZE};
  X86pMem oracle_mem = {.host = suite.reference, .lo = GUEST_BASE, .size = GUEST_SIZE};
  suite.current = name;
  if (sparse_mode) {
    mem.sparse = x86p_sparse_create();
    oracle_mem.sparse = x86p_sparse_create();
    check(mem.sparse && oracle_mem.sparse, "sparse owners unavailable");
    check(x86p_sparse_map(mem.sparse, GUEST_BASE, suite.guest, 517) &&
              x86p_sparse_map(mem.sparse, GUEST_BASE + 517, suite.guest + 517, GUEST_SIZE - 517) &&
              x86p_sparse_map(oracle_mem.sparse, GUEST_BASE, suite.reference, 517) &&
              x86p_sparse_map(oracle_mem.sparse, GUEST_BASE + 517, suite.reference + 517, GUEST_SIZE - 517),
          "sparse fixture mapping failed");
    if (sparse_mode == 2) {
      check(x86p_sparse_protect(mem.sparse, GUEST_BASE + 521, 7, kX86pMemRead) &&
                x86p_sparse_protect(oracle_mem.sparse, GUEST_BASE + 521, 7, kX86pMemRead),
            "sparse write denial failed");
    }
  }
  wasm_test_case_mem(&suite, name, code, size, cpu, 0, &mem, &oracle_mem);
  x86p_sparse_destroy(mem.sparse);
  x86p_sparse_destroy(oracle_mem.sparse);
  return suite.last_cpu;
}

static void arithmetic(void) {
  static const uint8_t integer_ops[] = {
      0xdb, 0xdf, 0xeb, 0xef, 0xfc, 0xfd, 0xfe, 0xd4, 0xec, 0xed, 0xdc, 0xdd, 0xf8, 0xf9, 0xfa, 0xfb, 0xe8, 0xe9,
      0xd8, 0xd9, 0x74, 0x75, 0x76, 0x64, 0x65, 0x66, 0x60, 0x61, 0x62, 0x68, 0x69, 0x6a, 0x67, 0x63, 0x6b, 0xf1,
      0xf2, 0xf3, 0xd1, 0xd2, 0xd3, 0xe1, 0xe2, 0xd5, 0xe5, 0xe4, 0xf5, 0xe0, 0xe3, 0xda, 0xde, 0xea, 0xee, 0xf6};
  static const uint8_t float_ops[] = {
      0x58, 0x5c, 0x59, 0x5e, 0x5d, 0x5f, 0x51, 0x54, 0x55, 0x56, 0x57, 0x14, 0x15, 0x2f, 0x2e};
  unsigned i, pattern, form;
  for (i = 0; i < sizeof integer_ops; ++i) {
    for (pattern = 0; pattern < 3; ++pattern) {
      for (form = 0; form < 3; ++form) {
        uint8_t code[] = {0x66, 0x0f, integer_ops[i], form == 0 ? 0xc1 : form == 1 ? 0xc0 : 0x07};
        X86pInsn insn;
        check(x86p_decode(code, sizeof code, &insn) == sizeof code, "integer fixture decode failed");
        run(x86p_simd_op_name((X86pSimdOp)insn.simd), code, sizeof code, initial(pattern), form == 2);
      }
    }
  }
  for (i = 0; i < sizeof float_ops; ++i) {
    for (pattern = 0; pattern < 3; ++pattern) {
      for (form = 0; form < 3; ++form) {
        uint8_t code[] = {0xf3, 0x0f, float_ops[i], form == 0 ? 0xc1 : 0x07};
        const unsigned prefix = form == 2 && i < 7 ? 0u : 1u;
        run("float arithmetic source and alias", code + prefix, sizeof code - prefix, initial(pattern), form != 0);
      }
    }
  }
  for (i = 0; i < 8; ++i) {
    uint8_t code[] = {0x66, 0x0f, 0x70, 0xc1, (uint8_t)(i * 37u)};
    run("PSHUFD selector", code, sizeof code, initial(i), 0);
    code[0] = 0xf3;
    code[2] = 0xc2;
    run("CMPSS predicate", code, sizeof code, initial(i), 0);
    run("CMPPS predicate", code + 1, sizeof code - 1, initial(i), 0);
    code[2] = 0xc6;
    run("SHUFPS selector and source overlap", code + 1, sizeof code - 1, initial(i), 0);
  }
  for (i = 0; i < 3; ++i) {
    for (form = 2; form <= 6; form += 2) {
      static const unsigned counts[] = {0, 1, 15, 16, 31, 32, 63, 64, 255};
      for (pattern = 0; pattern < sizeof counts / sizeof counts[0]; ++pattern) {
        uint8_t code[] = {0x66, 0x0f, (uint8_t)(0x71u + i), (uint8_t)(0xc0u + form * 8u), (uint8_t)counts[pattern]};
        if (i == 2 && form == 4) {
          continue; /* PSRAQ is not an SSE2 instruction. */
        }
        run("packed immediate shift boundary", code, sizeof code, initial(pattern), 0);
      }
    }
  }
}

/*
 * The packed forms the backend emits as WebAssembly SIMD rather than calling
 * its helper for. See jit_wasm_simd_inline.h.
 *
 * WHAT HAS TO BE DRIVEN HERE AND IS NOT DRIVEN BY arithmetic() ABOVE:
 *
 *   - A MEMORY SOURCE ON THE CONTIGUOUS MAPPING. arithmetic() reaches a memory
 *     operand only with a sparse mapping, where the inline form declines by
 *     design, so the emitted v128 load of guest memory would never run.
 *   - ANDNPS WITH DISTINCT OPERANDS. It is the one row whose two sources are
 *     pushed in the other order, because x86 computes ~dst & src and the host
 *     instruction computes a & ~b. With equal operands both orders give zero
 *     and the case proves nothing.
 *   - A SOURCE THAT ALIASES THE DESTINATION, for the same reason the
 *     lane-at-a-time path reads everything before writing: `shufps xmm0, xmm0`
 *     selects lanes of the register it is about to overwrite.
 *   - EVERY OPERAND PATTERN, including the NaN, infinity and sign-bit ones
 *     `initial` builds, because f32x4 arithmetic and a scalar C loop are the
 *     same function only if they are the same function on those too.
 */
static void packed_inline(void) {
  /* The second opcode byte of each two-operand packed form, no prefix. */
  static const uint8_t binary[] = {0x58, 0x5c, 0x59, 0x5e, 0x54, 0x55, 0x56, 0x57};
  static const uint8_t modrm[] = {0xc1, 0xc0, 0x07};
  unsigned i, pattern, form, mode;
  for (i = 0; i < sizeof binary; ++i) {
    for (form = 0; form < sizeof modrm; ++form) {
      for (pattern = 0; pattern < 8; ++pattern) {
        for (mode = 0; mode < 2; ++mode) {
          const uint8_t code[] = {0x0f, binary[i], modrm[form]};
          X86pInsn insn;
          check(x86p_decode(code, sizeof code, &insn) == sizeof code, "packed fixture decode failed");
          run(x86p_simd_op_name((X86pSimdOp)insn.simd), code, sizeof code, initial(pattern), mode);
        }
      }
    }
  }
  for (i = 0; i < 8; ++i) {
    for (form = 0; form < sizeof modrm; ++form) {
      for (mode = 0; mode < 2; ++mode) {
        const uint8_t code[] = {0x0f, 0xc6, modrm[form], (uint8_t)(i * 37u)};
        run("SHUFPS emitted selector", code, sizeof code, initial(i), mode);
      }
    }
  }
}

static void movement(void) {
  static const struct {
    const char *name;
    uint8_t code[5];
    size_t n;
  } forms[] = {{"MOVUPS load", {0x0f, 0x10, 0x07}, 3},
               {"MOVUPS store", {0x0f, 0x11, 0x07}, 3},
               {"MOVSS register preserve", {0xf3, 0x0f, 0x10, 0xc1}, 4},
               {"MOVSS zeroing load", {0xf3, 0x0f, 0x10, 0x07}, 4},
               {"MOVSS store", {0xf3, 0x0f, 0x11, 0x07}, 4},
               {"MOVQ zeroing load", {0xf3, 0x0f, 0x7e, 0x07}, 4},
               {"MOVQ store", {0x66, 0x0f, 0xd6, 0x07}, 4},
               {"MOVD zeroing register", {0x66, 0x0f, 0x6e, 0xc1}, 4},
               {"MOVD gpr", {0x66, 0x0f, 0x7e, 0xc1}, 4},
               {"MOVLPS load", {0x0f, 0x12, 0x07}, 3},
               {"MOVLPS store", {0x0f, 0x13, 0x07}, 3},
               {"MOVHPS load", {0x0f, 0x16, 0x07}, 3},
               {"MOVHPS store", {0x0f, 0x17, 0x07}, 3},
               {"MOVHLPS alias", {0x0f, 0x12, 0xc0}, 3},
               {"MOVLHPS alias", {0x0f, 0x16, 0xc0}, 3},
               {"MOVMSKPS", {0x0f, 0x50, 0xc1}, 3},
               {"PMOVMSKB", {0x66, 0x0f, 0xd7, 0xc1}, 4},
               {"PEXTRW", {0x66, 0x0f, 0xc5, 0xc1, 0xff}, 5},
               {"PINSRW register", {0x66, 0x0f, 0xc4, 0xc1, 0xff}, 5},
               {"PINSRW memory", {0x66, 0x0f, 0xc4, 0x07, 0xff}, 5},
               {"LDMXCSR", {0x0f, 0xae, 0x17}, 3},
               {"STMXCSR", {0x0f, 0xae, 0x1f}, 3},
               {"EMMS", {0x0f, 0x77}, 2},
               {"MFENCE", {0x0f, 0xae, 0xf0}, 3},
               {"PREFETCHNTA", {0x0f, 0x18, 0x07}, 3}};
  unsigned i, mode;
  for (i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
    for (mode = 0; mode < 3; ++mode) {
      run(forms[i].name, forms[i].code, forms[i].n, initial(i), mode);
    }
    {
      X86pCpu cpu = initial(i);
      cpu.reg[kX86pEdi] = GUEST_BASE + GUEST_SIZE - 1;
      run(forms[i].name, forms[i].code, forms[i].n, cpu, 0);
    }
  }
}

static void conversions(void) {
  static const uint32_t inputs[] = {0,
                                    0x80000000u,
                                    0x3fa00000u,
                                    0xbfc00000u,
                                    0x7f800000u,
                                    0xff800000u,
                                    0x7fc00123u,
                                    0x4f000000u,
                                    0xcf000001u,
                                    0x4effffffu};
  unsigned i, op;
  for (op = 0x2c; op <= 0x2d; ++op) {
    for (i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
      uint8_t code[] = {0xf3, 0x0f, (uint8_t)op, 0xc1};
      X86pCpu cpu = initial(i);
      memcpy(cpu.xmm[1], &inputs[i], 4);
      cpu = run("float integer conversion range", code, sizeof code, cpu, 0);
      if (i >= 4 && i <= 8) {
        check(cpu.reg[kX86pEax] == 0x80000000u, "invalid conversion must return x86 integer-indefinite");
      }
    }
  }
  {
    static const uint8_t code[] = {0xf3, 0x0f, 0x2a, 0xc1};
    run("signed integer to scalar float", code, sizeof code, initial(2), 0);
  }
}

static void refusals(void) {
  static const struct {
    uint8_t code[5];
    size_t n;
  } forms[] = {
      {{0x0f, 0xfc, 0xc1}, 3}, {{0x0f, 0x0f, 0xc1, 0x9e}, 4}, {{0x0f, 0x53, 0xc1}, 3}, {{0x67, 0x0f, 0x10, 0x00}, 4}};
  unsigned i;
  suite.current = "unsupported SIMD shapes";
  for (i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
    X86pInsn insn;
    check(x86p_decode(forms[i].code, forms[i].n, &insn) != 0, "refusal fixture did not decode");
    check(!x86p_wasm_can_lower(&insn), "raw MMX/3DNow, approximate reciprocal or addr16 admitted");
  }
}

int main(void) {
  suite.current = "fixture";
  arithmetic();
  packed_inline();
  movement();
  conversions();
  refusals();
  suite.current = "denominators";
  check(suite.cases == suite.entered && suite.entered > 700, "translation coverage incomplete");
  check(suite.faults > 10, "faulting forms not exercised");
  check(suite.simd_inline > 0, "no SIMD instruction was lowered to host SIMD");
  check(suite.simd_inline < suite.simd_ops, "every SIMD instruction was inlined, so the declined forms went untested");
  printf("WASM SIMD: cases=%u translated_entries=%u faults=%u simd=%lu inline=%lu checks=%u failures=%u\n",
         suite.cases,
         suite.entered,
         suite.faults,
         suite.simd_ops,
         suite.simd_inline,
         suite.checks,
         suite.failures);
  return suite.failures ? 1 : 0;
}
