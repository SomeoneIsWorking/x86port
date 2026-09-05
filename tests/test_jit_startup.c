/* Differential and ABI controls for integer memory helpers and status transfer. */
#include "code_memory.h"
#include "cpu_compare.h"
#include "exec.h"
#include "jit_x64.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned cases, failures;
static void check_case(const uint8_t *program, size_t length, X86pCpu initial, int fault) {
  uint8_t guest[4096] = {0}, before[4096], expected[4096];
  X86pMem mem = {.host = guest, .lo = 0x10000, .size = sizeof guest};
  X86pCpu oracle = initial, jit = initial;
  X86pJitBlock block;
  JcCodeRegion code;
  char reason[256] = {0};
  unsigned steps = 0;
  X86pStepStatus step = kX86pStepOk;
  memcpy(guest, program, length);
  guest[length] = 0xEB; /* JMP +0 ends the block without changing flags. */
  memset(guest + 0x400, 0xA5, 16);
  memcpy(before, guest, sizeof guest);
  while (step == kX86pStepOk && oracle.eip < 0x10000 + length + 2 && steps++ < 16) {
    step = x86p_step(&oracle, &mem, NULL);
    const unsigned opcode = program[0] == 0x67 ? program[1] : program[0];
    if (opcode >= 0xE0 && opcode <= 0xE2) {
      break; /* a LOOP instruction terminates its basic block */
    }
  }
  memcpy(expected, guest, sizeof guest);
  memcpy(guest, before, sizeof guest);
  cases++;
  if (jc_code_region_create(65536, &code, reason, sizeof reason) != kJcCodeOk) {
    failures++;
    return;
  }
  X86pJitStatus status = x86p_jit_translate(&mem, initial.eip, code.write, code.size, &block, reason, sizeof reason);
  if (status != kX86pJitOk || jc_code_publish(&code, block.host_bytes) != kJcCodeOk) {
    printf("case %u: translation refused: %s\n", cases, reason);
    failures++;
    jc_code_region_destroy(&code);
    return;
  }
  block.entry = code.exec;
#if defined(__aarch64__)
  register uintptr_t sentinel __asm__("x20") = UINT64_C(0x123456789ABCDEF0);
#else
  register uintptr_t sentinel __asm__("r12") = UINT64_C(0x123456789ABCDEF0);
#endif
  __asm__ volatile("" : "+r"(sentinel));
  X86pJitExit exit = x86p_jit_enter(&block, &jit);
  __asm__ volatile("" : "+r"(sentinel));
  if (sentinel != UINT64_C(0x123456789ABCDEF0) || exit != (fault ? kX86pJitExitMemoryFault : kX86pJitExitBlockEnd) ||
      step != (fault ? kX86pStepMemoryFault : kX86pStepOk) || x86p_cpu_diff(&oracle, &jit, NULL, NULL) ||
      memcmp(expected, guest, sizeof guest)) {
    printf("case %u: state/ABI mismatch, exit %d step %d, first byte %02x\n", cases, exit, step, program[0]);
    failures++;
  }
  jc_code_region_destroy(&code);
}

int main(void) {
  X86pCpu cpu;
  if (!x86p_jit_available()) {
    return 77;
  }
  x86p_cpu_reset(&cpu);
  cpu.eip = 0x10000;
  cpu.reg[kX86pEsp] = 0x10800;
  for (unsigned width = 1; width <= 4; width *= 2) {
    for (unsigned direction = 0; direction < 2; direction++) {
      for (unsigned kind = 0; kind < 3; kind++) {
        X86pCpu copy = cpu;
        uint8_t bytes[3];
        size_t n = 0;
        if (width == 2) {
          bytes[n++] = 0x66;
        }
        bytes[n++] = 0xF3;
        bytes[n++] = width == 1 ? 0xA4 : 0xA5;
        copy.df = (uint8_t)direction;
        copy.reg[kX86pEcx] = 16;
        copy.reg[kX86pEsi] = 0x10400;
        copy.reg[kX86pEdi] = kind == 0 ? 0x10600 : kind == 1 ? 0x10401 : 0x11000 - width;
        check_case(bytes, n, copy, kind == 2 && !direction);
      }
    }
  }
  for (unsigned ah = 0; ah < 256; ah++) {
    cpu.reg[kX86pEax] = 0x12340055 | (ah << 8);
    x86p_flags_set(&cpu.flags, kX86pFlagsAdd, 0x7FFFFFFF, 1, 0x80000000, 4);
    const uint8_t sahf[] = {0x9E};
    const uint8_t lahf[] = {0x9F};
    const uint8_t roundtrip[] = {0x9E, 0x9F, 0x83, 0xC0, 1};
    check_case(sahf, sizeof sahf, cpu, 0);
    check_case(lahf, sizeof lahf, cpu, 0);
    check_case(roundtrip, sizeof roundtrip, cpu, 0);
  }
  for (unsigned width = 1; width <= 4; width *= 2) {
    for (unsigned op = 0; op <= 7; op++) {
      if (op == 6) {
        continue;
      }
      for (unsigned count = 0; count <= 33; count++) {
        uint8_t bytes[10];
        size_t n = 0;
        if (width == 2) {
          bytes[n++] = 0x66;
        }
        bytes[n++] = width == 1 ? 0xC0 : 0xC1;
        bytes[n++] = (uint8_t)(0x05 | (op << 3));
        bytes[n++] = 0x00;
        bytes[n++] = 0x04;
        bytes[n++] = 0x01;
        bytes[n++] = 0;
        bytes[n++] = (uint8_t)count;
        check_case(bytes, n, cpu, 0);
        const unsigned bad = 0x11000 - width + 1;
        for (unsigned b = 0; b < 4; b++) {
          bytes[n - 5 + b] = (uint8_t)(bad >> (8 * b));
        }
        check_case(bytes, n, cpu, 1);
      }
    }
    for (unsigned op = 2; op <= 3; op++) {
      for (unsigned carry = 0; carry <= 1; carry++) {
        uint8_t bytes[10];
        size_t n = 0;
        if (width == 2) {
          bytes[n++] = 0x66;
        }
        bytes[n++] = width == 1 ? 0x80 : 0x83;
        bytes[n++] = (uint8_t)(0x05 | (op << 3));
        bytes[n++] = 0;
        bytes[n++] = 4;
        bytes[n++] = 1;
        bytes[n++] = 0;
        bytes[n++] = 0xFF;
        x86p_flags_set_explicit(&cpu.flags, carry);
        check_case(bytes, n, cpu, 0);
      }
    }
  }
  for (unsigned width = 1; width <= 4; width *= 2) {
    for (unsigned signed_op = 0; signed_op < 2; signed_op++) {
      for (unsigned form = 0; form < 3; form++) {
        uint8_t bytes[10];
        size_t n = 0;
        if (width == 2) {
          bytes[n++] = 0x66;
        }
        bytes[n++] = width == 1 ? 0xF6 : 0xF7;
        bytes[n++] = (uint8_t)((form == 0 ? 0xC4 : 0x05) | ((4 + signed_op) << 3));
        if (form) {
          unsigned address = form == 1 ? 0x10400 : 0x11000 - width + 1;
          for (unsigned b = 0; b < 4; b++) {
            bytes[n++] = (uint8_t)(address >> (8 * b));
          }
        }
        cpu.reg[kX86pEax] = 0xFEDC9876;
        check_case(bytes, n, cpu, form == 2);
      }
    }
    for (unsigned subtract = 0; subtract < 2; subtract++) {
      for (unsigned carry = 0; carry < 2; carry++) {
        for (unsigned fault = 0; fault < 2; fault++) {
          uint8_t bytes[10];
          size_t n = 0;
          if (width == 2) {
            bytes[n++] = 0x66;
          }
          bytes[n++] = (uint8_t)(0x12 + subtract * 8 + (width != 1));
          bytes[n++] = 0x25; /* AH/SP/ESP, [absolute] */
          unsigned address = fault ? 0x11000 - width + 1 : 0x10400;
          for (unsigned b = 0; b < 4; b++) {
            bytes[n++] = (uint8_t)(address >> (8 * b));
          }
          x86p_flags_set_explicit(&cpu.flags, carry);
          check_case(bytes, n, cpu, fault);
        }
      }
    }
  }
  for (unsigned left = 0; left < 2; left++) {
    for (unsigned count = 0; count <= 33; count++) {
      for (unsigned variable = 0; variable < 2; variable++) {
        for (unsigned form = 0; form < 3; form++) {
          uint8_t bytes[10] = {0x0F, (uint8_t)((left ? 0xA4 : 0xAC) + variable)};
          size_t n = 2;
          bytes[n++] = form == 0 ? 0xC8 : 0x0D;
          if (form) {
            unsigned address = form == 1 ? 0x10400 : 0x10FFD;
            for (unsigned b = 0; b < 4; b++) {
              bytes[n++] = (uint8_t)(address >> (8 * b));
            }
          }
          if (!variable) {
            bytes[n++] = (uint8_t)count;
          }
          cpu.reg[kX86pEcx] = count;
          check_case(bytes, n, cpu, form == 2);
        }
      }
    }
  }
  const uint32_t loop_counts[] = {0, 1, 2, 0xFFFF, 0x10000, 0x10001, UINT32_MAX};
  for (unsigned width = 0; width < 2; width++) {
    for (unsigned op = 0xE0; op <= 0xE2; op++) {
      for (unsigned count = 0; count < sizeof loop_counts / sizeof loop_counts[0]; count++) {
        for (unsigned zf = 0; zf < 2; zf++) {
          uint8_t loop[] = {0x67, (uint8_t)op, 0x10};
          cpu.reg[kX86pEcx] = loop_counts[count];
          x86p_flags_set_explicit(&cpu.flags, zf ? 0xAD7 : 0xA97);
          check_case(loop + width, 3 - width, cpu, 0);
        }
      }
    }
  }
  const uint8_t rdtsc[] = {0x0F, 0x31, 0x0F, 0x31};
  cpu.tsc = UINT64_C(0xFFFFFFFF);
  check_case(rdtsc, sizeof rdtsc, cpu, 0);
  for (unsigned control = 0; control <= 0xC00; control += 0x400) {
    cpu.x87.control = (uint16_t)(0x37F | control);
    const uint8_t load[] = {0xD9, 0x2D, 0, 4, 1, 0};
    const uint8_t store[] = {0xD9, 0x3D, 0, 4, 1, 0};
    const uint8_t wait[] = {0x9B};
    check_case(load, sizeof load, cpu, 0);
    check_case(store, sizeof store, cpu, 0);
    check_case(wait, sizeof wait, cpu, 0);
  }

  for (unsigned leaf = 0; leaf < 4; leaf++) {
    cpu.reg[kX86pEax] = leaf;
    const uint8_t cpuid[] = {0x0F, 0xA2}, directions[] = {0xFD, 0xFC};
    check_case(cpuid, sizeof cpuid, cpu, 0);
    check_case(directions, sizeof directions, cpu, 0);
  }
  const size_t xmm_bytes = sizeof cpu.xmm;
  for (size_t i = 0; i < xmm_bytes; i++) {
    ((uint8_t *)cpu.xmm)[i] = (uint8_t)(i * 37);
  }
  for (unsigned opcode = 0x54; opcode <= 0x57; opcode++) {
    const uint8_t reg[] = {0x0F, (uint8_t)opcode, 0xC1};
    const uint8_t mem[] = {0x0F, (uint8_t)opcode, 0x05, 0, 4, 1, 0};
    const uint8_t bad[] = {0x0F, (uint8_t)opcode, 0x05, 0xF1, 0x0F, 1, 0};
    check_case(reg, sizeof reg, cpu, 0);
    check_case(mem, sizeof mem, cpu, 0);
    check_case(bad, sizeof bad, cpu, 1);
  }
  const uint8_t packed_ops[] = {0x58, 0x59, 0x5C, 0x5E};
  for (unsigned op = 0; op < sizeof packed_ops; op++) {
    const uint8_t alias[] = {0x0F, packed_ops[op], 0xC0};
    const uint8_t reg[] = {0x0F, packed_ops[op], 0xC1};
    const uint8_t mem[] = {0x0F, packed_ops[op], 0x05, 0, 4, 1, 0};
    const uint8_t bad[] = {0x0F, packed_ops[op], 0x05, 0xF1, 0x0F, 1, 0};
    check_case(alias, sizeof alias, cpu, 0);
    check_case(reg, sizeof reg, cpu, 0);
    check_case(mem, sizeof mem, cpu, 0);
    check_case(bad, sizeof bad, cpu, 1);
  }
  for (unsigned imm = 0; imm < 256; imm++) {
    const uint8_t alias[] = {0x0F, 0xC6, 0xC0, (uint8_t)imm};
    const uint8_t reg[] = {0x0F, 0xC6, 0xC1, (uint8_t)imm};
    const uint8_t mem[] = {0x0F, 0xC6, 0x05, 0, 4, 1, 0, (uint8_t)imm};
    const uint8_t bad[] = {0x0F, 0xC6, 0x05, 0xF1, 0x0F, 1, 0, (uint8_t)imm};
    check_case(alias, sizeof alias, cpu, 0);
    check_case(reg, sizeof reg, cpu, 0);
    check_case(mem, sizeof mem, cpu, 0);
    check_case(bad, sizeof bad, cpu, 1);
  }
  const uint8_t scalar_moves[][8] = {{0xF3, 0x0F, 0x10, 0xC1},
                                     {0xF3, 0x0F, 0x10, 0x05, 0xFC, 0x0F, 1, 0},
                                     {0xF3, 0x0F, 0x11, 0x05, 0xFC, 0x0F, 1, 0},
                                     {0xF3, 0x0F, 0x10, 0x05, 0xFD, 0x0F, 1, 0},
                                     {0xF3, 0x0F, 0x11, 0x05, 0xFD, 0x0F, 1, 0}};
  for (unsigned i = 0; i < 5; i++) {
    check_case(scalar_moves[i], i == 0 ? 4 : 8, cpu, i >= 3);
  }
  for (unsigned op = 0x12; op <= 0x17; op++) {
    if (op == 0x14 || op == 0x15) {
      continue;
    }
    const uint8_t mem[] = {0x0F, (uint8_t)op, 0x05, 0xF8, 0x0F, 1, 0};
    const uint8_t bad[] = {0x0F, (uint8_t)op, 0x05, 0xF9, 0x0F, 1, 0};
    check_case(mem, sizeof mem, cpu, 0);
    check_case(bad, sizeof bad, cpu, 1);
    if (op == 0x12 || op == 0x16) {
      const uint8_t reg[] = {0x0F, (uint8_t)op, 0xC1};
      const uint8_t alias[] = {0x0F, (uint8_t)op, 0xC0};
      check_case(reg, sizeof reg, cpu, 0);
      check_case(alias, sizeof alias, cpu, 0);
    }
  }
  const uint8_t xmm_load[] = {0x0F, 0x10, 0x05, 0, 4, 1, 0};
  const uint8_t xmm_store[] = {0x0F, 0x11, 0x05, 0, 4, 1, 0};
  check_case(xmm_load, sizeof xmm_load, cpu, 0);
  check_case(xmm_store, sizeof xmm_store, cpu, 0);
  const uint8_t memory_forms[][6] = {{0xDF, 0x05, 0, 4, 1, 0},
                                     {0xDB, 0x05, 0, 4, 1, 0},
                                     {0xDF, 0x2D, 0, 4, 1, 0},
                                     {0xDF, 0x1D, 0, 4, 1, 0},
                                     {0xDB, 0x1D, 0, 4, 1, 0},
                                     {0xDF, 0x3D, 0, 4, 1, 0},
                                     {0xDA, 0x35, 0, 4, 1, 0},
                                     {0xDE, 0x35, 0, 4, 1, 0},
                                     {0xDA, 0x1D, 0, 4, 1, 0},
                                     {0xDE, 0x1D, 0, 4, 1, 0}};
  const uint8_t register_forms[][2] = {{0xD8, 0xD9},
                                       {0xDE, 0xD9},
                                       {0xD9, 0xC9},
                                       {0xD9, 0xE0},
                                       {0xD9, 0xE1},
                                       {0xD9, 0xE4},
                                       {0xD9, 0xFA},
                                       {0xD9, 0xFE},
                                       {0xD9, 0xFF},
                                       {0xD9, 0xFB},
                                       {0xD9, 0xF2},
                                       {0xD9, 0xF3},
                                       {0xD9, 0xF1},
                                       {0xD9, 0xF9},
                                       {0xD9, 0xF0},
                                       {0xD9, 0xFD},
                                       {0xD9, 0xFC},
                                       {0xD9, 0xF8},
                                       {0xD9, 0xF5}};
  for (unsigned depth = 0; depth <= 8; depth++) {
    x86p_x87_reset(&cpu.x87);
    for (unsigned i = 0; i < depth; i++) {
      x86p_x87_push(&cpu.x87, (long double)(i + 1) / 4);
    }
    for (unsigned i = 0; i < sizeof memory_forms / sizeof memory_forms[0]; i++) {
      uint8_t bad[6];
      memcpy(bad, memory_forms[i], sizeof bad);
      bad[2] = 0xFF;
      bad[3] = 0x0F;
      check_case(memory_forms[i], 6, cpu, 0);
      const int writes = i >= 3 && i <= 5;
      check_case(bad, 6, cpu, !writes || depth != 0);
    }
    for (unsigned i = 0; i < sizeof register_forms / sizeof register_forms[0]; i++) {
      check_case(register_forms[i], 2, cpu, 0);
    }
  }
  const uint8_t emms[] = {0x0F, 0x77};
  check_case(emms, sizeof emms, cpu, 0);
  printf("%u translated startup cases, %u failures\n", cases, failures);
  return failures ? 1 : 0;
}
