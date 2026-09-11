#include "cpu.h"
#include "jit_engine.h"
#include "jit_wasm_host.h"
#include "jit_wasm_lower.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kGuestBase = 0x400000, kGuestBytes = 32768, kProgramStride = 16 };
static uint8_t guest[kGuestBytes];
static unsigned checks;
static unsigned failures;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

EM_JS(unsigned, live_modules, (), {
  let count = 0;
  for (const host of Module.x86pWasmHosts.values()) {
    count += host.modules.size;
  }
  return count;
});

static void program(unsigned offset, uint32_t value) {
  guest[offset] = 0xb8; /* MOV EAX, imm32; JMP $ */
  memcpy(guest + offset + 1u, &value, sizeof value);
  guest[offset + 5u] = 0xeb;
  guest[offset + 6u] = 0xfe;
}

static X86pJitEngine *create(const X86pMem *mem, size_t capacity, unsigned blocks) {
  char reason[256] = {0};
  X86pJitEngine *engine = x86p_jit_engine_create(mem, capacity, blocks, reason, sizeof reason);
  check(engine != NULL, reason);
  return engine;
}

static void run(X86pJitEngine *engine, X86pCpu *cpu, unsigned steps) {
  char reason[256] = {0};
  X86pJitRunStatus status = x86p_jit_engine_run(engine, cpu, NULL, steps, reason, sizeof reason);
  check(status == kX86pRunBudget, reason);
}

static void helpers_and_invalidation(const X86pMem *mem) {
  /* Several calls into the real C helpers in the same generated block, with
   * intervening writes. Recorded helper answers cannot pass this contract. */
  static const uint8_t code[] = {0xb8,
                                 0xff,
                                 0xff,
                                 0xff,
                                 0xff, /* MOV EAX,-1 */
                                 0x83,
                                 0xc0,
                                 1, /* ADD EAX,1 */
                                 0x83,
                                 0xd0,
                                 0,    /* ADC EAX,0 */
                                 0x40, /* INC EAX */
                                 0xd1,
                                 0xe0, /* SHL EAX,1 */
                                 0xeb,
                                 0xfe};
  X86pJitEngine *engine = create(mem, 65536u, 128u);
  X86pCpu cpu;
  X86pJitEngineStats before, after;
  if (!engine) {
    return;
  }
  memcpy(guest, code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = kGuestBase;
  run(engine, &cpu, 8u);
  check(cpu.reg[kX86pEax] == 4u, "real imported ALU helper chain returned wrong EAX");
  x86p_jit_engine_stats(engine, &before);
  run(engine, &cpu, 8u);
  x86p_jit_engine_stats(engine, &after);
  check(after.blocks_translated == before.blocks_translated, "warm loop missed the translation cache");
  check(after.blocks_entered == before.blocks_entered + 8u, "warm loop entry denominator is wrong");

  program(0u, 123u);
  x86p_jit_engine_invalidate(engine, kGuestBase + 1u, kGuestBase + 2u);
  cpu.eip = kGuestBase;
  run(engine, &cpu, 1u);
  check(cpu.reg[kX86pEax] == 123u, "interior-byte invalidation retained a stale generated module");

  guest[0] = 0x0f;
  guest[1] = 0x53;
  guest[2] = 0xc0; /* RCPPS: unsupported */
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + 3u);
  cpu.eip = kGuestBase;
  {
    char reason[256] = {0};
    X86pJitRunStatus status = x86p_jit_engine_run(engine, &cpu, NULL, 1u, reason, sizeof reason);
    check(status == kX86pRunUnsupported, "unsupported guest instruction was not refused");
    check(strstr(reason, "RCPPS") != NULL, "unsupported refusal lost its mnemonic");
    check(cpu.reg[kX86pEax] == 123u, "unsupported instruction mutated guest registers");
  }
  x86p_jit_engine_stats(engine, &after);
  check(after.translate_refusals == 1u, "unsupported refusal denominator missing");
  printf("runtime: translated=%llu entered=%llu refusals=%llu\n",
         (unsigned long long)after.blocks_translated,
         (unsigned long long)after.blocks_entered,
         (unsigned long long)after.translate_refusals);
  x86p_jit_engine_destroy(engine);
  check(live_modules() == 0u, "destroy retained generated modules or table entries");
}

static void lifetime(const X86pMem *mem, unsigned cache_blocks) {
  X86pJitEngine *engine = create(mem, 16u * 1024u * 1024u, cache_blocks);
  X86pJitEngine *other = create(mem, 65536u, 128u);
  X86pCpu cpu;
  X86pJitEngineStats stats;
  unsigned i;
  if (!engine || !other) {
    x86p_jit_engine_destroy(engine);
    x86p_jit_engine_destroy(other);
    return;
  }
  x86p_cpu_reset(&cpu);
  program(0u, 37u);
  cpu.eip = kGuestBase;
  run(other, &cpu, 1u);
  for (i = 0; i < X86P_WASM_MAX_LIVE_MODULES + 16u; ++i) {
    program(i * kProgramStride, i);
    cpu.eip = kGuestBase + i * kProgramStride;
    run(engine, &cpu, 1u);
    check(cpu.reg[kX86pEax] == i, "eviction reused a stale table entry");
    check(live_modules() <= X86P_WASM_MAX_LIVE_MODULES + 1u, "module lifetime exceeded its bound");
  }
  x86p_jit_engine_stats(engine, &stats);
  check(stats.cache_flushes > 0u, "capacity pressure did not exercise a flush");
  check(stats.blocks_translated == X86P_WASM_MAX_LIVE_MODULES + 16u, "capacity test failed to translate every block");
  printf("lifetime: cache_capacity=%u translated=%llu flushes=%llu live=%u\n",
         cache_blocks,
         (unsigned long long)stats.blocks_translated,
         (unsigned long long)stats.cache_flushes,
         live_modules());
  x86p_jit_engine_destroy(engine);
  check(live_modules() == 1u, "destroy affected another engine's module");
  cpu.eip = kGuestBase;
  run(other, &cpu, 1u);
  check(cpu.reg[kX86pEax] == 37u, "another engine's table entry changed during eviction");
  char reason[256] = {0};
  check(x86p_jit_engine_invalidate_all(other, reason, sizeof reason), reason);
  check(live_modules() == 0u, "whole-space invalidation retained modules");
  cpu.eip = kGuestBase;
  run(other, &cpu, 1u);
  check(cpu.reg[kX86pEax] == 0u, "whole-space invalidation reused stale code");
  x86p_jit_engine_destroy(other);
  check(live_modules() == 0u, "lifetime test leaked modules");
}

static void invalid_module(void) {
  X86pWasmHost host;
  char reason[256] = {0};
  static const uint8_t invalid[] = {0, 97, 115, 109, 255};
  check(x86p_wasm_host_create(&host, reason, sizeof reason), reason);
  if (!host.user) {
    return;
  }
  check(host.instantiate(host.user, invalid, sizeof invalid) < 0, "invalid module was accepted");
  check(x86p_wasm_host_error(&host)[0] != '\0', "module refusal discarded the engine's diagnostic");
  check(live_modules() == 0u, "invalid module mutated publication state");
  x86p_wasm_host_destroy(&host);
}

static void smallest_storage(const X86pMem *mem) {
  X86pJitEngine *engine = create(mem, X86P_WASM_MIN_MODULE_BYTES, 128u);
  X86pCpu cpu;
  unsigned i;
  if (!engine) {
    return;
  }
  x86p_cpu_reset(&cpu);
  for (i = 0u; i < 32u; ++i) {
    program(i * kProgramStride, i);
    cpu.eip = kGuestBase + i * kProgramStride;
    run(engine, &cpu, 1u);
    check(cpu.reg[kX86pEax] == i, "minimum module budget cannot hold its promised single block");
  }
  x86p_jit_engine_destroy(engine);
  check(live_modules() == 0u, "minimum storage leaked generated modules");
}

int main(void) {
  X86pMem mem = {.host = guest, .lo = kGuestBase, .size = sizeof guest};
  helpers_and_invalidation(&mem);
  lifetime(&mem, 8192u);
  lifetime(&mem, 16u);
  smallest_storage(&mem);
  invalid_module();
  printf("WebAssembly shipping runtime: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
