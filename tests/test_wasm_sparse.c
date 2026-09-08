#include "cpu.h"
#include "jit_engine.h"
#include "memory_sparse.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kCode = 0x400000, kPage = 4096 };
#define DATA 0xa0000000u
#define CROSS (DATA + kPage - 2u)

static unsigned checks, failures;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

static X86pJitRunStatus run(X86pJitEngine *engine, X86pCpu *cpu) {
  char reason[256] = {0};
  X86pJitRunStatus result = x86p_jit_engine_run(engine, cpu, 1, reason, sizeof reason);
  if (result != kX86pRunBudget && result != kX86pRunMemoryFault) {
    check(0, reason);
  }
  return result;
}

static void sparse_generated_access(void) {
  /* MOV [EBX],EAX; MOV EDX,[EBX]; JMP $. Data straddles two separately
   * allocated pages at a high guest address impossible to reserve in wasm32. */
  uint8_t code[] = {0x89, 0x03, 0x8b, 0x13, 0xeb, 0xfe};
  uint8_t first[kPage] = {0}, second[kPage] = {0};
  uint8_t replacement[] = {0xb8, 0x78, 0x56, 0x34, 0x12, 0xeb, 0xfe};
  X86pSparseMem *sparse = x86p_sparse_create();
  X86pMem memory = {.sparse = sparse};
  X86pJitEngine *engine;
  X86pJitEngineStats before, after;
  X86pCpu cpu, snapshot;
  char reason[256] = {0};
  check(sparse != NULL, "sparse allocation");
  if (!sparse) {
    return;
  }
  check(x86p_sparse_map(sparse, kCode, code, sizeof code), "sparse code mapping");
  check(x86p_sparse_map(sparse, DATA, first, sizeof first), "sparse first page");
  check(x86p_sparse_map(sparse, DATA + kPage, second, sizeof second), "sparse second page");
  engine = x86p_jit_engine_create(&memory, 65536, 128, reason, sizeof reason);
  check(engine != NULL, reason);
  if (!engine) {
    x86p_sparse_destroy(sparse);
    return;
  }
  x86p_cpu_reset(&cpu);
  cpu.eip = kCode;
  cpu.reg[kX86pEax] = 0x76543210;
  cpu.reg[kX86pEbx] = CROSS;
  check(run(engine, &cpu) == kX86pRunBudget, "generated split-page store and load execute");
  check(cpu.reg[kX86pEdx] == 0x76543210, "generated split-page load value");
  check(first[kPage - 2] == 0x10 && first[kPage - 1] == 0x32 && second[0] == 0x54 && second[1] == 0x76,
        "generated split-page store reaches both real allocations");
  x86p_jit_engine_stats(engine, &before);
  check(before.blocks_translated == 1 && before.blocks_entered == 1 && before.translate_refusals == 0,
        "sparse execution has nonzero exact JIT denominators");
  cpu.eip = kCode;
  check(run(engine, &cpu) == kX86pRunBudget, "generated sparse cache hit");
  x86p_jit_engine_stats(engine, &after);
  check(after.blocks_translated == 1 && after.blocks_entered == 2, "sparse warm entry reuses translated block");

  check(x86p_sparse_protect(sparse, DATA, 2u * kPage, kX86pMemRead), "protect data read-only");
  cpu.eip = kCode;
  cpu.reg[kX86pEax] = 0xffffffff;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "generated read-only MOV store faults before any CPU commit");
  check(first[kPage - 2] == 0x10 && second[1] == 0x76, "read-only store preserves both backing spans");
  x86p_jit_engine_invalidate(engine, kCode, kCode + sizeof code);
  code[0] = 0x01; /* ADD [EBX],EAX: failed store must not commit lazy flags. */
  cpu.eip = kCode;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "read-only generated ALU faults before flags or memory commit");
  x86p_jit_engine_invalidate(engine, kCode, kCode + sizeof code);
  code[0] = 0x89;
  check(x86p_sparse_protect(sparse, DATA, 2u * kPage, kX86pMemWrite), "protect data write-only");
  cpu.eip = kCode + 2;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "write-only generated load faults before destination commit");
  check(x86p_sparse_protect(sparse, DATA, 2u * kPage, kX86pMemRead | kX86pMemWrite), "restore data read-write");

  /* The same compiled store now crosses a hole. The guard must return to the
   * CPU fault boundary without a JS trap, memory prefix write, or flag change. */
  x86p_jit_engine_invalidate(engine, DATA + kPage, DATA + 2u * kPage);
  check(x86p_sparse_unmap(sparse, DATA + kPage, sizeof second), "remove data page");
  cpu.eip = kCode;
  cpu.reg[kX86pEax] = 0xffffffff;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault, "generated sparse guard returns typed guest fault");
  check(memcmp(&cpu, &snapshot, sizeof cpu) == 0, "fault preserves CPU and faulting EIP");
  check(first[kPage - 2] == 0x10 && first[kPage - 1] == 0x32, "fault preserves mapped store prefix");
  check(x86p_sparse_map(sparse, DATA + kPage, second, sizeof second), "restore page");
  check(run(engine, &cpu) == kX86pRunBudget && cpu.reg[kX86pEdx] == 0xffffffff,
        "compiled guest access sees restored mapping");

  /* Replacing executable backing requires invalidation before mutation. */
  x86p_jit_engine_stats(engine, &before);
  x86p_jit_engine_invalidate(engine, kCode, kCode + sizeof code);
  check(x86p_sparse_unmap(sparse, kCode, sizeof code), "unmap executable allocation");
  check(x86p_sparse_map(sparse, kCode, replacement, sizeof replacement), "map replacement executable allocation");
  cpu.eip = kCode;
  check(run(engine, &cpu) == kX86pRunBudget && cpu.reg[kX86pEax] == 0x12345678,
        "invalidation retranslates replacement executable backing");
  x86p_jit_engine_stats(engine, &after);
  check(after.blocks_translated == before.blocks_translated + 1, "replacement adds exactly one translation");
  printf("sparse WASM: translated=%llu entered=%llu refusals=%llu\n",
         (unsigned long long)after.blocks_translated,
         (unsigned long long)after.blocks_entered,
         (unsigned long long)after.translate_refusals);
  x86p_jit_engine_destroy(engine);
  x86p_sparse_destroy(sparse);
}

int main(void) {
  sparse_generated_access();
  printf("WebAssembly sparse memory: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
