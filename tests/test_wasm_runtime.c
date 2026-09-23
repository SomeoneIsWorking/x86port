#include "cpu.h"
#include "jit_engine.h"
#include "jit_wasm_chain.h"
#include "jit_wasm_host.h"
#include "jit_wasm_lower.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The largest live-block cap any case below asks for. Deliberately past the
   1024 that used to be compiled in, so a build that regressed to a fixed cap
   fails here rather than in a browser. */
enum { kMaxCapacity = 2048u };
enum { kGuestBase = 0x400000, kProgramStride = 16, kGuestBytes = (kMaxCapacity + 16) * kProgramStride };
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

EM_JS(int, imports_bound_once_per_host, (), {
  for (const host of Module.x86pWasmHosts.values()) {
    if (host.modules.size && host.importBindings != 1) {
      return 0;
    }
  }
  return 1;
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

/*
 * `capacity` is now ONE number: the engine sizes its block cache and its live
 * module arena together, because a cache larger than the storage behind it can
 * only hold entries whose code has already been evicted.
 */
static void lifetime(const X86pMem *mem, unsigned capacity) {
  X86pJitEngine *engine = create(mem, 16u * 1024u * 1024u, capacity);
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
  for (i = 0; i < capacity + 16u; ++i) {
    program(i * kProgramStride, i);
    cpu.eip = kGuestBase + i * kProgramStride;
    run(engine, &cpu, 1u);
    check(cpu.reg[kX86pEax] == i, "eviction reused a stale table entry");
    check(live_modules() <= capacity + 1u, "module lifetime exceeded its bound");
  }
  /*
   * BOTH ANSWERS, at whatever cap the caller chose. Round-robin eviction over
   * `capacity` slots after `capacity + 16` translations leaves blocks 16
   * through capacity+15 live, so:
   *   - the most recent block must be entered without retranslating, and
   *   - block 0 must NOT be, because it was evicted.
   * A test that only asserted the first would pass just as happily on an
   * engine that retranslated everything, which is exactly the defect a fixed
   * 1024-module cap produced for every consumer asking for a larger cache.
   */
  {
    uint64_t translated;
    x86p_jit_engine_stats(engine, &stats);
    translated = stats.blocks_translated;
    cpu.eip = kGuestBase + (capacity + 15u) * kProgramStride;
    run(engine, &cpu, 1u);
    x86p_jit_engine_stats(engine, &stats);
    check(cpu.reg[kX86pEax] == capacity + 15u, "a retained block returned another block's result");
    check(stats.blocks_translated == translated, "capacity pressure evicted the newest block instead of preserving it");

    cpu.eip = kGuestBase;
    run(engine, &cpu, 1u);
    x86p_jit_engine_stats(engine, &stats);
    check(cpu.reg[kX86pEax] == 0u, "re-entering an evicted block returned another block's result");
    check(stats.blocks_translated == translated + 1u, "an evicted block was entered without being retranslated");
  }
  /* This host releases modules one at a time, so ordinary capacity pressure
     never needs the whole-space rewind. A flush here would mean unrelated
     translations were thrown away to make room for one block. */
  check(stats.cache_flushes == 0u, "WASM module pressure flushed unrelated translations");
  check(stats.blocks_translated == capacity + 17u, "capacity test failed to translate every block");
  check(imports_bound_once_per_host(), "WASM imports were rebound for each translated block");
  /*
   * The eviction counters, proven where they actually fire. The native storage
   * has no victim and flushes instead, so the engine test can only ever watch
   * these stay at zero -- and a counter that has only been seen at zero is not
   * an instrument. Here the arena really does evict, and the count has to move
   * WITHOUT the embedder's invalidation counters moving: nothing in this test
   * told the engine that guest memory changed, and reporting the engine's own
   * reclaim as an embedder notification is how a browser run came to accuse
   * the wrong owner of 106,000 invalidations.
   */
  check(stats.evictions > 0u, "capacity pressure evicted nothing, so the eviction counter proves nothing");
  check(stats.eviction_blocks_dropped >= stats.evictions, "an eviction freed a slot without dropping its block");
  check(stats.invalidations == 0u, "the engine's own reclaim was counted as an embedder invalidation");
  printf("lifetime: capacity=%u translated=%llu flushes=%llu evictions=%llu dropping=%llu live=%u\n",
         capacity,
         (unsigned long long)stats.blocks_translated,
         (unsigned long long)stats.cache_flushes,
         (unsigned long long)stats.evictions,
         (unsigned long long)stats.eviction_blocks_dropped,
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
  char refusal[256] = {0};
  check(host.instantiate(host.user, invalid, sizeof invalid, refusal, sizeof refusal) < 0,
        "invalid module was accepted");
  check(x86p_wasm_host_error(&host)[0] != '\0', "module refusal discarded the engine's diagnostic");
  check(refusal[0] != '\0', "module refusal did not reach the caller's buffer");
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

/*
 * THE POSITIVE SIDE of the runtime chain census, whose negative lives in
 * tests/test_jit_engine.c: this backend DOES emit constant successor
 * addresses, so a two-block guest loop must report almost every dispatch as
 * one a chaining backend could have removed. Without this case the census
 * could report "unrecorded" for every entry on every host and still pass.
 */
static void chain_census(const X86pMem *mem) {
  char reason[256] = {0};
  X86pCpu cpu;
  X86pJitEngine *engine = create(mem, 1u << 16, 64u);
  const X86pJitChainCensus *census;
  if (!engine) {
    return;
  }
  /* +0: JMP +2 (to +4); +4: JMP -6 (back to +0). Every entry is a dispatch
     and none of them re-enters the block just left. */
  memset(guest, 0x90, sizeof guest);
  guest[0] = 0xeb;
  guest[1] = 0x02;
  guest[4] = 0xeb;
  guest[5] = 0xfa;

  check(x86p_jit_engine_set_chain_census(engine, 1, 64u, reason, sizeof reason), reason);
  memset(&cpu, 0, sizeof cpu);
  cpu.eip = kGuestBase;
  run(engine, &cpu, 200u);

  census = x86p_jit_engine_chain_census(engine);
  check(census != NULL, "chain census detached itself");
  check(x86p_jit_chain_census_entries(census) == 200u, "chain census missed entries");
  /* Every entry but the first, which has no predecessor to compare against. */
  check(x86p_jit_chain_census_chainable(census) == 199u, "chain census saw no chainable dispatch");
  check(x86p_jit_chain_census_unrecorded(census) == 1u, "chain census recorded no successors");
  check(x86p_jit_chain_census_dropped_keys(census) == 0u, "chain census dropped a block");
  printf("chain census: %llu of %llu entries chainable, %llu unrecorded\n",
         (unsigned long long)x86p_jit_chain_census_chainable(census),
         (unsigned long long)x86p_jit_chain_census_entries(census),
         (unsigned long long)x86p_jit_chain_census_unrecorded(census));
  x86p_jit_engine_destroy(engine);
}

/* The run's stop address and the one address the consumer takes over. */
typedef struct ChainPolicy {
  uint32_t take;
} ChainPolicy;

static int chain_boundary(uint32_t eip, void *user) {
  (void)eip;
  (void)user;
  return 0;
}

static int chain_intercept(const X86pCpu *cpu, void *user, void *run_user) {
  (void)run_user;
  return cpu->eip == ((const ChainPolicy *)user)->take;
}

static uint32_t chain_stop(void *run_user) {
  return *(const uint32_t *)run_user;
}

enum { kRingBlocks = 100u, kRingStride = 8u };

/* Block k: INC EAX; JMP block k+1, the last one back to the first. */
static void ring(void) {
  memset(guest, 0x90, sizeof guest);
  for (uint32_t k = 0; k < kRingBlocks; k++) {
    const uint32_t at = k * kRingStride;
    const uint32_t next = (k + 1u) % kRingBlocks * kRingStride;
    const int32_t rel = (int32_t)next - (int32_t)(at + 6u);
    guest[at] = 0x40;
    guest[at + 1u] = 0xe9;
    memcpy(guest + at + 2u, &rel, sizeof rel);
  }
}

static uint64_t
chained_run(X86pJitEngine *engine, X86pCpu *cpu, uint32_t *stop, uint64_t steps, X86pJitRunStatus want) {
  char reason[256] = {0};
  X86pJitEngineStats before, after;
  x86p_jit_engine_stats(engine, &before);
  check(x86p_jit_engine_run(engine, cpu, stop, steps, reason, sizeof reason) == want, reason);
  x86p_jit_engine_stats(engine, &after);
  return after.blocks_chained - before.blocks_chained;
}

/*
 * Chained exits on the engine the product runs: a ring of blocks, more than a
 * compaction batch, so the later laps run through bodies relowered into shared
 * modules on the slots they were published with. Every block counts itself in
 * EAX, so a transfer that skipped a block or entered the wrong one shows.
 */
static void chaining(const X86pMem *mem) {
  char reason[256] = {0};
  ChainPolicy policy = {0u};
  uint32_t no_stop = 0xFFFFFFF0u;
  X86pCpu cpu;
  X86pJitEngine *engine = create(mem, 1u << 20, 256u);
  if (!engine) {
    return;
  }
  x86p_jit_engine_set_intercept(engine, chain_intercept, &policy);
  x86p_jit_engine_set_boundary(engine, chain_boundary, &policy);
  check(x86p_jit_engine_set_run_stop(engine, chain_stop, reason, sizeof reason), reason);
  ring();
  x86p_cpu_reset(&cpu);
  cpu.eip = kGuestBase;

  const uint64_t first = chained_run(engine, &cpu, &no_stop, 10000u, kX86pRunBudget);
  check(cpu.reg[kX86pEax] == 10000u, "a chained ring did not run every block once per entry");
  check(cpu.eip == kGuestBase, "a chained ring stopped at the wrong block");
  /* An exit links to a block the dispatcher found cached, so the first two
     laps dispatch every block; after that only the depth cap returns. */
  check(first >= 10000u - 2u * kRingBlocks - 10000u / X86P_WASM_CHAIN_TRANSFERS - 1u,
        "a warm ring still went through the dispatcher");
  const uint64_t warm = chained_run(engine, &cpu, &no_stop, 10000u, kX86pRunBudget);
  check(cpu.reg[kX86pEax] == 20000u, "a relowered ring did not run every block once per entry");
  /* Only the depth cap sends a transfer back: one dispatch per 256 entries. */
  check(warm >= 10000u - 10000u / X86P_WASM_CHAIN_TRANSFERS - 1u, "relowered blocks lost their links");

  uint32_t stop = kGuestBase + 50u * kRingStride;
  policy.take = stop;
  chained_run(engine, &cpu, &stop, 10000u, kX86pRunIntercept);
  check(cpu.eip == stop, "a transfer entered the run's stop address");
  check(cpu.reg[kX86pEax] == 20050u, "the stop was not reached by the ring");
  policy.take = 0u;

  /* Block 1 now spins. A link still naming its old code keeps the ring going. */
  guest[kRingStride + 1u] = 0xeb;
  guest[kRingStride + 2u] = 0xfe;
  x86p_jit_engine_invalidate(engine, kGuestBase + kRingStride, kGuestBase + 2u * kRingStride);
  cpu.eip = kGuestBase;
  chained_run(engine, &cpu, &no_stop, 100u, kX86pRunBudget);
  check(cpu.eip == kGuestBase + kRingStride + 1u, "a link survived the invalidation of its target");
  printf(
      "chaining: %llu of 10000 entries chained cold, %llu warm\n", (unsigned long long)first, (unsigned long long)warm);
  x86p_jit_engine_destroy(engine);
}

/* Caller k: CALL F; then JMP caller k+1. F, after the callers: INC EAX; RET. */
static void shared_return(void) {
  const uint32_t f = kRingBlocks * 16u;
  memset(guest, 0x90, sizeof guest);
  for (uint32_t k = 0; k < kRingBlocks; k++) {
    const uint32_t at = k * 16u;
    const int32_t call = (int32_t)f - (int32_t)(at + 5u);
    const int32_t jump = (int32_t)((k + 1u) % kRingBlocks * 16u) - (int32_t)(at + 10u);
    guest[at] = 0xe8;
    memcpy(guest + at + 1u, &call, sizeof call);
    guest[at + 5u] = 0xe9;
    memcpy(guest + at + 6u, &jump, sizeof jump);
  }
  guest[f] = 0x40;
  guest[f + 1u] = 0xc3;
}

/*
 * THE PROBE's positive: one RET returning to a hundred call sites misses its
 * slot on nearly every exit, and only the front-array probe keeps it from
 * returning to the dispatcher each time -- without it a third of the entries
 * would be dispatches.
 */
static void probing(const X86pMem *mem) {
  char reason[256] = {0};
  ChainPolicy policy = {0u};
  uint32_t no_stop = 0xFFFFFFF0u;
  X86pCpu cpu;
  X86pJitEngine *engine = create(mem, 1u << 20, 512u);
  if (!engine) {
    return;
  }
  x86p_jit_engine_set_intercept(engine, chain_intercept, &policy);
  x86p_jit_engine_set_boundary(engine, chain_boundary, &policy);
  check(x86p_jit_engine_set_run_stop(engine, chain_stop, reason, sizeof reason), reason);
  shared_return();
  x86p_cpu_reset(&cpu);
  cpu.eip = kGuestBase;
  cpu.reg[kX86pEsp] = kGuestBase + kGuestBytes - 64u;
  (void)chained_run(engine, &cpu, &no_stop, 3u * kRingBlocks * 4u, kX86pRunBudget);
  const uint64_t warm = chained_run(engine, &cpu, &no_stop, 30000u, kX86pRunBudget);
  check(cpu.reg[kX86pEax] == 4u * kRingBlocks + 10000u, "a probed RET skipped or repeated a call");
  check(cpu.eip == kGuestBase, "a probed RET returned to the wrong caller");
  check(cpu.reg[kX86pEsp] == kGuestBase + kGuestBytes - 64u, "a probed RET unbalanced the stack");
  check(warm >= 30000u - 30000u / X86P_WASM_CHAIN_TRANSFERS - 1u, "a RET that missed its slot went to the dispatcher");
  printf("probe: %llu of 30000 entries chained through a shared RET\n", (unsigned long long)warm);
  x86p_jit_engine_destroy(engine);
}

int main(void) {
  X86pMem mem = {.host = guest, .lo = kGuestBase, .size = sizeof guest};
  helpers_and_invalidation(&mem);
  lifetime(&mem, kMaxCapacity);
  lifetime(&mem, 16u);
  smallest_storage(&mem);
  invalid_module();
  chain_census(&mem);
  chaining(&mem);
  probing(&mem);
  printf("WebAssembly shipping runtime: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
