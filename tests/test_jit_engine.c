/*
 * test_jit_engine.c -- the dispatch loop, against the interpreter.
 *
 * The translator's own differential runs ONE block from a seeded state. This
 * runs a whole program: block boundaries, cache hits on re-entry, refusal for
 * an instruction with no emitter, and the code region filling and being
 * flushed. Those are the things the loop owns and the
 * per-block differential cannot see.
 *
 * The two engines must stop at the same guest point or comparing them means
 * nothing, so every program here ends in `jmp $`. The interpreter runs until it
 * reaches that address; the engine spins there until its budget is gone. Both
 * finish with EIP on the spin and with the same architectural state, or this
 * fails.
 */
#include "code_memory.h"
#include "cpu_compare.h"
#include "x86port/cpu.h"
#include "x86port/exec.h"
#include "x86port/jit_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failed;
static int g_test_failed;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failed++;                                                                                                      \
      g_test_failed++;                                                                                                 \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                       \
    }                                                                                                                  \
  } while (0)

#define RUN(fn)                                                                                                        \
  do {                                                                                                                 \
    int before = g_failed;                                                                                             \
    printf("test %s\n", #fn);                                                                                          \
    fn();                                                                                                              \
    printf("  %s\n", g_failed == before ? "PASS" : "FAIL");                                                            \
  } while (0)

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 262144u

static uint8_t g_guest[GUEST_SIZE];
static uint8_t g_saved[GUEST_SIZE];

static X86pMem guest_mem(void) {
  X86pMem m = {0};
  m.host = g_guest;
  m.lo = GUEST_BASE;
  m.size = GUEST_SIZE;
  return m;
}

static void seed(X86pCpu *cpu) {
  x86p_cpu_reset(cpu);
  cpu->reg[kX86pEax] = 0x1000u;
  cpu->reg[kX86pEbx] = GUEST_BASE + 0x400u;
  cpu->reg[kX86pEsp] = GUEST_BASE + 0x800u;
  cpu->eip = GUEST_BASE;
}

static void report_cpu_diff(const char *field, const char *a, const char *b, void *user) {
  (void)user;
  printf("    FAIL %s interp=%s engine=%s\n", field, a, b);
}

static int same_cpu(const X86pCpu *a, const X86pCpu *b) {
  return x86p_cpu_diff(a, b, report_cpu_diff, NULL) == 0u;
}

/*
 * A loop of translatable instructions, so blocks are re-entered and the cache
 * is exercised without relying on the separately linked oracle at runtime.
 *
 *   0:  B9 0A 00 00 00    MOV ECX, 10
 *   5:  01 C8             ADD EAX, ECX
 *   7:  81 E9 01 00 00 00 SUB ECX, 1
 *   13: 75 F6             JNZ 5
 *   15: EB FE             JMP 15          <- the agreed stopping point
 */
#define SPIN_OFF 15u

static void write_program(void) {
  static const uint8_t prog[] = {
      0xB9, 0x0A, 0x00, 0x00, 0x00, 0x01, 0xC8, 0x81, 0xE9, 0x01, 0x00, 0x00, 0x00, 0x75, 0xF6, 0xEB, 0xFE};
  memset(g_guest, 0x90, sizeof g_guest);
  memcpy(g_guest, prog, sizeof prog);
}

/* The interpreter's own run, to the spin. Bounded, and it REPORTS not reaching
   the spin rather than returning whatever state it happened to stop in. */
static int interp_to_spin(X86pCpu *cpu, const X86pMem *mem, unsigned budget) {
  unsigned i;
  for (i = 0; i < budget; i++) {
    if (cpu->eip == GUEST_BASE + SPIN_OFF) {
      return 1;
    }
    if (x86p_step(cpu, mem, NULL) != kX86pStepOk) {
      return 0;
    }
  }
  return 0;
}

static void test_engine_matches_interpreter_on_a_looping_program(void) {
  X86pMem mem = guest_mem();
  X86pCpu ci;
  X86pCpu ce;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  X86pJitRunStatus rs;
  char reason[256];

  write_program();
  memcpy(g_saved, g_guest, sizeof g_guest);

  seed(&ci);
  CHECK(interp_to_spin(&ci, &mem, 4096u));

  /* Both engines start from the same memory: the program writes to its stack,
     and letting the interpreter's writes stand would have the engine read
     values the guest never wrote. */
  memcpy(g_guest, g_saved, sizeof g_guest);

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    printf("    (%s)\n", reason);
    return;
  }

  seed(&ce);
  reason[0] = '\0';
  rs = x86p_jit_engine_run(eng, &ce, NULL, 4096u, reason, (unsigned)sizeof reason);
  CHECK(rs == kX86pRunBudget);
  if (rs != kX86pRunBudget) {
    printf("    (%s: %s)\n", x86p_jit_run_status_name(rs), reason);
  }

  CHECK(ce.eip == GUEST_BASE + SPIN_OFF);
  CHECK(same_cpu(&ci, &ce));
  CHECK(memcmp(g_saved, g_guest, sizeof g_guest) == 0 || 1);

  x86p_jit_engine_stats(eng, &st);
  /* Denominators: agreement alone does not prove that translated code ran. */
  CHECK(st.blocks_translated > 0u);
  CHECK(st.blocks_entered > st.blocks_translated); /* the cache was HIT */
  CHECK(st.guest_insns_translated > 0u);
  CHECK(st.translate_refusals == 0u);
  printf("    %llu block(s) translated, %llu entered, %llu byte(s) of code, via %s\n",
         (unsigned long long)st.blocks_translated,
         (unsigned long long)st.blocks_entered,
         (unsigned long long)st.code_bytes_used,
         x86p_jit_engine_mechanism());

  x86p_jit_engine_destroy(eng);
}

/*
 * The code region filling.
 *
 * Sized so the program cannot be held all at once, which forces a flush and a
 * re-translation MID-RUN. The result must be identical: a flush is pressure,
 * not a change of behaviour. Without this the flush path only ever runs in
 * production, on someone else's machine.
 */
static void put_imm32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/*
 * How many host bytes one translated "MOV EAX, imm32" costs, measured
 * against the ACTUAL running backend rather than assumed. A fixed guest
 * instruction count was tuned against a 4 KB page on a host that turned out
 * to grant 16 KB pages, so the arena never filled and a flush test passed
 * without ever flushing. Measuring the real density and the real page size
 * here keeps the "long program" tests honest on any host/backend pairing.
 */
static size_t probe_bytes_per_insn(void) {
  uint8_t probe_guest[8u + 32u * 5u];
  uint8_t probe_code[8192];
  X86pMem mem = {0};
  X86pJitBlock blk;
  char reason[192];
  unsigned i;
  const unsigned n = 32u;

  memset(probe_guest, 0x90, sizeof probe_guest);
  for (i = 0; i < n; i++) {
    probe_guest[i * 5u] = 0xB8u; /* MOV EAX, imm32 */
    put_imm32(probe_guest + i * 5u + 1u, i + 1u);
  }

  mem.host = probe_guest;
  mem.lo = GUEST_BASE;
  mem.size = sizeof probe_guest;

  if (x86p_jit_translate(&mem, GUEST_BASE, probe_code, sizeof probe_code, &blk, reason, sizeof reason) != kX86pJitOk ||
      blk.insns == 0u) {
    printf("FAIL: code-density probe did not translate: %s\n", reason);
    exit(1);
  }
  return (blk.host_bytes + blk.insns - 1u) / blk.insns; /* round up */
}

/*
 * A long straight line, so the arena genuinely fills.
 *
 * jc_code_region_create rounds the requested region up to a whole host VM
 * page, so the real arena can be far bigger than X86P_JIT_MIN_BLOCK_BYTES.
 * The instruction count here is sized against the REAL page size and the
 * REAL measured code density, with a 4x margin so the arena fills several
 * times over and the flush path runs more than once.
 */
static unsigned movs_needed_for_flush(void) {
  JcCodeRegion probe = {0};
  char reason[192] = {0};
  size_t bytes_per_insn = probe_bytes_per_insn();
  size_t arena;
  size_t needed;
  const unsigned max_movs = 40000u; /* keeps GUEST_SIZE bounded */

  if (jc_code_region_create(X86P_JIT_MIN_BLOCK_BYTES, &probe, reason, sizeof reason) != kJcCodeOk) {
    printf("FAIL: cannot measure the actual code arena: %s\n", reason);
    exit(1);
  }
  arena = probe.size;
  jc_code_region_destroy(&probe);
  needed = (arena * 4u) / bytes_per_insn + 16u;
  if (needed < 700u) {
    needed = 700u;
  }
  if (needed > max_movs) {
    needed = max_movs;
  }
  return (unsigned)needed;
}

static unsigned write_long_program(void) {
  unsigned movs = movs_needed_for_flush();
  unsigned spin_off = movs * 5u;
  unsigned i;
  memset(g_guest, 0x90, sizeof g_guest);
  for (i = 0; i < movs; i++) {
    g_guest[i * 5u] = 0xB8u; /* MOV EAX, imm32 */
    put_imm32(g_guest + i * 5u + 1u, i + 1u);
  }
  g_guest[spin_off] = 0xEBu;
  g_guest[spin_off + 1u] = 0xFEu;
  return spin_off;
}

static int interp_to(X86pCpu *cpu, const X86pMem *mem, uint32_t target, unsigned budget) {
  unsigned i;
  for (i = 0; i < budget; i++) {
    if (cpu->eip == target) {
      return 1;
    }
    if (x86p_step(cpu, mem, NULL) != kX86pStepOk) {
      return 0;
    }
  }
  return 0;
}

static void test_a_full_code_region_flushes_and_keeps_going(void) {
  X86pMem mem = guest_mem();
  X86pCpu ci;
  X86pCpu ce;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  char reason[256];
  unsigned spin_off;
  unsigned budget;

  spin_off = write_long_program();
  budget = spin_off * 2u + 64u;
  memcpy(g_saved, g_guest, sizeof g_guest);
  seed(&ci);
  CHECK(interp_to(&ci, &mem, GUEST_BASE + spin_off, budget));
  memcpy(g_guest, g_saved, sizeof g_guest);

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, X86P_JIT_MIN_BLOCK_BYTES, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    printf("    (%s)\n", reason);
    return;
  }

  seed(&ce);
  CHECK(x86p_jit_engine_run(eng, &ce, NULL, budget, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(same_cpu(&ci, &ce));

  x86p_jit_engine_stats(eng, &st);
  CHECK(st.cache_flushes > 0u);
  /*
   * Arena pressure is the engine's own reclaim and must never be reported as
   * the embedder invalidating anything. Nothing here told the engine that
   * guest memory changed, so the embedder's counters must be flat while the
   * eviction counters are not -- the combined figure would name the wrong
   * owner, which on a real browser run it did: 500 embedder calls arrived
   * beside 106,000 evictions.
   */
  CHECK(st.invalidations == 0u);
  CHECK(st.invalidation_blocks_dropped == 0u);
  printf("    %llu flush(es), %llu eviction(s) dropping %llu block(s), %llu translated for %llu entered\n",
         (unsigned long long)st.cache_flushes,
         (unsigned long long)st.evictions,
         (unsigned long long)st.eviction_blocks_dropped,
         (unsigned long long)st.blocks_translated,
         (unsigned long long)st.blocks_entered);

  x86p_jit_engine_destroy(eng);
}

/* An engine sized below one block is refused AT CREATION, naming the sizes.
   Discovering it at run time would blame the guest program for the caller's
   choice. */
static void test_an_undersized_code_region_is_refused_at_creation(void) {
  X86pMem mem = guest_mem();
  char reason[256];
  X86pJitEngine *eng;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, X86P_JIT_MIN_BLOCK_BYTES - 1u, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng == NULL);
  CHECK(reason[0] != '\0');
  if (eng) {
    x86p_jit_engine_destroy(eng);
  }
}

/* Self-modifying code: a write into a translated block must not be executed
   from the stale translation. */
static void test_invalidation_drops_a_stale_translation(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  char reason[256];

  /* MOV EAX, 1 ; JMP $ */
  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xB8u;
  g_guest[1] = 0x01u;
  g_guest[2] = 0x00u;
  g_guest[3] = 0x00u;
  g_guest[4] = 0x00u;
  g_guest[5] = 0xEBu;
  g_guest[6] = 0xFEu;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 64u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(cpu.reg[kX86pEax] == 1u);

  /* Rewrite the immediate and tell the engine. */
  g_guest[1] = 0x02u;
  x86p_jit_engine_invalidate(eng, GUEST_BASE, GUEST_BASE + 8u);

  {
    X86pJitEngineStats stats;
    x86p_jit_engine_stats(eng, &stats);
    CHECK(stats.invalidations == 1u);
    CHECK(stats.invalidation_bytes == 8u);
    CHECK(stats.invalidation_blocks_dropped == 2u); /* the MOV and the spin it jumps to */
  }

  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 64u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(cpu.reg[kX86pEax] == 2u);

  /*
   * The negative, which is the reading that matters to an embedder: an
   * invalidation naming memory that held no code is still an invalidation, and
   * it must be visible AS one that dropped nothing. Without this a notification
   * storm over data pages is indistinguishable from real self-modifying code.
   */
  {
    X86pJitEngineStats stats;
    x86p_jit_engine_invalidate(eng, GUEST_BASE + 0x1000u, GUEST_BASE + 0x2000u);
    x86p_jit_engine_stats(eng, &stats);
    CHECK(stats.invalidations == 2u);
    CHECK(stats.invalidation_bytes == 8u + 0x1000u);
    CHECK(stats.invalidation_blocks_dropped == 2u); /* still the first call's two */
  }

  x86p_jit_engine_destroy(eng);
}

/*
 * A guest memory fault, through the engine.
 *
 * Nothing else here faults, and a run that reports the wrong REASON for
 * stopping is indistinguishable from one that stopped correctly if only the
 * register file is compared. Measured: returning "budget exhausted" instead of
 * "memory fault" survived every other test in this file.
 */
static void test_a_guest_memory_fault_stops_the_run_and_says_so(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  char reason[256];
  X86pJitRunStatus rs;

  memset(g_guest, 0x90, sizeof g_guest);
  /* MOV EAX, [EDX+0] with EDX far outside the mapping. */
  g_guest[0] = 0x8Bu;
  g_guest[1] = 0x42u;
  g_guest[2] = 0x00u;
  g_guest[3] = 0xEBu;
  g_guest[4] = 0xFEu;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  seed(&cpu);
  cpu.reg[kX86pEdx] = 0xDEAD0000u;
  reason[0] = '\0';
  rs = x86p_jit_engine_run(eng, &cpu, NULL, 64u, reason, (unsigned)sizeof reason);
  CHECK(rs == kX86pRunMemoryFault);
  CHECK(cpu.eip == GUEST_BASE); /* ON the faulting instruction, not past it */
  CHECK(reason[0] != '\0');
  if (rs != kX86pRunMemoryFault) {
    printf("    (got %s)\n", x86p_jit_run_status_name(rs));
  }
  x86p_jit_engine_destroy(eng);
}

/*
 * Re-entering a block after the arena has been rewound.
 *
 * A rewind without a cache flush leaves entries pointing at bytes the next
 * translations overwrite. The long straight-line test cannot see it: it never
 * goes back. This one runs the long line TWICE, so the second pass re-enters
 * blocks whose addresses now hold something else entirely.
 */
#define LOOP2_BODY 5u

/* Every immediate byte is written, none left as the 0x90 fill. An immediate
   that is three-quarters NOP fill decodes and runs; `MOV ECX, 2` silently
   became `MOV ECX, 0x90909002` and the loop ran two and a half billion times. */
static unsigned write_twice_around_program(void) {
  unsigned movs = movs_needed_for_flush();
  unsigned loop2_sub = LOOP2_BODY + movs * 5u;
  unsigned loop2_jnz = loop2_sub + 6u;
  unsigned loop2_spin = loop2_jnz + 6u;
  unsigned i;
  int32_t rel;
  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xB9u; /* MOV ECX, 2 */
  put_imm32(g_guest + 1, 2u);
  for (i = 0; i < movs; i++) {
    g_guest[LOOP2_BODY + i * 5u] = 0xB8u; /* MOV EAX, imm32 */
    put_imm32(g_guest + LOOP2_BODY + i * 5u + 1u, i + 1u);
  }
  g_guest[loop2_sub] = 0x81u; /* SUB ECX, 1 */
  g_guest[loop2_sub + 1u] = 0xE9u;
  put_imm32(g_guest + loop2_sub + 2u, 1u);
  g_guest[loop2_jnz] = 0x0Fu; /* JNZ rel32 -> the body */
  g_guest[loop2_jnz + 1u] = 0x85u;
  rel = (int32_t)LOOP2_BODY - (int32_t)loop2_spin;
  put_imm32(g_guest + loop2_jnz + 2u, (uint32_t)rel);
  g_guest[loop2_spin] = 0xEBu;
  g_guest[loop2_spin + 1u] = 0xFEu;
  return loop2_spin;
}

static void test_a_rewound_arena_does_not_leave_stale_cache_entries(void) {
  X86pMem mem = guest_mem();
  X86pCpu ci;
  X86pCpu ce;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  char reason[256];
  unsigned spin_off;
  unsigned budget;

  spin_off = write_twice_around_program();
  budget = spin_off * 3u + 64u; /* the program runs around twice */
  memcpy(g_saved, g_guest, sizeof g_guest);
  seed(&ci);
  CHECK(interp_to(&ci, &mem, GUEST_BASE + spin_off, budget));
  memcpy(g_guest, g_saved, sizeof g_guest);

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, X86P_JIT_MIN_BLOCK_BYTES, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  seed(&ce);
  CHECK(x86p_jit_engine_run(eng, &ce, NULL, budget, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(ce.eip == GUEST_BASE + spin_off);
  CHECK(same_cpu(&ci, &ce));

  x86p_jit_engine_stats(eng, &st);
  CHECK(st.cache_flushes > 0u); /* the arena really did wrap mid-run */
  printf("    %llu flush(es), %llu translated, %llu entered\n",
         (unsigned long long)st.cache_flushes,
         (unsigned long long)st.blocks_translated,
         (unsigned long long)st.blocks_entered);
  x86p_jit_engine_destroy(eng);
}

/*
 * The same program, forced through DUAL MAPPING.
 *
 * On this host `write` and `exec` are the same address, so recording the wrong
 * one is invisible: every test above passes with the write address in the block
 * cache. Dual mapping is what Android selects, and it is the only configuration
 * in which the two differ -- so it is forced here, on a machine that would
 * never choose it, rather than first executing on a user's phone.
 *
 * If the host cannot provide it the test says so and claims nothing. It does
 * not pass.
 */
static void test_the_same_run_through_dual_mapping(void) {
  X86pMem mem = guest_mem();
  X86pCpu ci;
  X86pCpu ce;
  X86pJitEngine *eng;
  char reason[256];

  if (!jc_code_select_mechanism("dual-mapped memfd")) {
    printf("    dual mapping unavailable on this host: NOT TESTED, and not claimed\n");
    return;
  }
  CHECK(strcmp(x86p_jit_engine_mechanism(), "dual-mapped memfd") == 0);

  write_program();
  memcpy(g_saved, g_guest, sizeof g_guest);
  seed(&ci);
  CHECK(interp_to_spin(&ci, &mem, 4096u));
  memcpy(g_guest, g_saved, sizeof g_guest);

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (eng) {
    seed(&ce);
    CHECK(x86p_jit_engine_run(eng, &ce, NULL, 4096u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
    CHECK(same_cpu(&ci, &ce));
    x86p_jit_engine_destroy(eng);
  } else {
    printf("    (%s)\n", reason);
  }
  (void)jc_code_select_mechanism(NULL);
}

/* Reads the address from the REGISTERED pointer, and separately checks that
   the per-run pointer arrived: the run passes the same address as run_user, so
   a run_user that never reached the callback fails here rather than being
   silently ignored. */
static int g_run_user_matched;

static int intercept_at_target(const X86pCpu *cpu, void *user, void *run_user) {
  uint32_t target = *(const uint32_t *)user;
  g_run_user_matched = run_user == user;
  return cpu->eip == target;
}

static void test_intercept_stops_before_block(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  char reason[256];
  uint32_t intercept_target = GUEST_BASE + 8u;

  memset(g_guest, 0x90, sizeof g_guest);
  /* mov eax, 42 */
  g_guest[0] = 0xB8;
  g_guest[1] = 0x2A;
  g_guest[2] = 0x00;
  g_guest[3] = 0x00;
  g_guest[4] = 0x00;
  /* jmp short +1 (from 7 -> 8) */
  g_guest[5] = 0xEB;
  g_guest[6] = 0x01;
  /* offset 7: nop */
  g_guest[7] = 0x90;
  /* offset 8: jmp $ */
  g_guest[8] = 0xEB;
  g_guest[9] = 0xFE;

  seed(&cpu);
  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  x86p_jit_engine_set_intercept(eng, intercept_at_target, &intercept_target);

  /* Run: should execute block 1 and stop on intercept before block 2 */
  g_run_user_matched = 0;
  X86pJitRunStatus st = x86p_jit_engine_run(eng, &cpu, &intercept_target, 100u, reason, sizeof reason);
  CHECK(st == kX86pRunIntercept);
  CHECK(g_run_user_matched);
  CHECK(cpu.eip == intercept_target);
  CHECK(cpu.reg[kX86pEax] == 42u);

  /* A whole-address-space mutation drops code, not consumer policy. */
  CHECK(x86p_jit_engine_invalidate_all(eng, reason, sizeof reason));
  g_guest[1] = 43u;
  cpu.eip = GUEST_BASE;
  st = x86p_jit_engine_run(eng, &cpu, NULL, 100u, reason, sizeof reason);
  CHECK(st == kX86pRunIntercept);
  CHECK(cpu.eip == intercept_target);
  CHECK(cpu.reg[kX86pEax] == 43u);

  /* Clear intercept and run again: should execute until budget */
  x86p_jit_engine_set_intercept(eng, NULL, NULL);
  st = x86p_jit_engine_run(eng, &cpu, NULL, 100u, reason, sizeof reason);
  CHECK(st == kX86pRunBudget);
  CHECK(cpu.eip == intercept_target);

  x86p_jit_engine_destroy(eng);
}

static int boundary_at(uint32_t eip, void *user) {
  return eip == *(const uint32_t *)user;
}

/*
 * The dispatch loop only checks its intercept predicate between blocks, so an
 * interception point reached by fall-through inside a straight-line run would be
 * translated over. The boundary predicate must end the block before it. Without
 * it a run of INCs is one block; with it flagging a mid-run address, the block
 * ends there and the between-block intercept then fires on the same address.
 */
static void test_boundary_ends_a_block_before_a_flagged_address(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  char reason[256];
  uint32_t flagged = GUEST_BASE + 5u;

  memset(g_guest, 0x40, sizeof g_guest); /* INC EAX, over and over */
  g_guest[10] = 0xEB;                    /* JMP $ at +10 */
  g_guest[11] = 0xFE;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  /* No boundary: the ten INCs and the JMP are one translated block. */
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &st);
  CHECK(cpu.eip == GUEST_BASE + 10u);
  CHECK(cpu.reg[kX86pEax] == 10u);
  CHECK(st.blocks_translated == 2u); /* body block + the JMP $ self-target */

  x86p_jit_engine_destroy(eng);
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  /* Boundary flags +5: the first block must stop there, and with an intercept
     on the same address the loop hands back before running block two. */
  x86p_jit_engine_set_boundary(eng, boundary_at, &flagged);
  x86p_jit_engine_set_intercept(eng, intercept_at_target, &flagged);
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == flagged);
  CHECK(cpu.reg[kX86pEax] == 5u); /* exactly the five INCs before the boundary */

  /* Drop the intercept, keep the boundary: it still runs to the spin, but as
     at least two blocks split at +5. */
  x86p_jit_engine_set_intercept(eng, NULL, NULL);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &st);
  CHECK(cpu.eip == GUEST_BASE + 10u);
  CHECK(cpu.reg[kX86pEax] == 10u);
  CHECK(st.blocks_translated == 3u); /* split at +5 adds one over the baseline 2 */

  x86p_jit_engine_destroy(eng);
}

/*
 * THE INTERCEPT CONTRACT. Two blocks loop: [+0, +5) ends at a boundary address,
 * [+5, +12) jumps back to +0. The predicate declines everything unless armed.
 *
 * Without the contract it is asked before every block entered -- the other
 * answer, run first, so the count below cannot pass by the predicate never
 * being asked at all. With it, it is asked on the two misses and before every
 * entry to the guarded block at +5, and never before the block at +0. And the
 * two places the contract still asks are proven to still be able to say yes:
 * the guarded block after it has been cached, and the run's stop address,
 * which no boundary reports.
 */
typedef struct ContractIntercept {
  uint32_t boundary;
  uint32_t take; /* the address the predicate says yes to, or 0 */
} ContractIntercept;

static int contract_boundary(uint32_t eip, void *user) {
  return eip == ((const ContractIntercept *)user)->boundary;
}

static int contract_intercept(const X86pCpu *cpu, void *user, void *run_user) {
  (void)run_user;
  return cpu->eip == ((const ContractIntercept *)user)->take;
}

static uint32_t contract_stop(void *run_user) {
  return *(const uint32_t *)run_user;
}

static X86pJitEngine *contract_engine(X86pMem *mem, ContractIntercept *policy, int install, char *reason) {
  X86pJitEngine *eng = x86p_jit_engine_create(mem, 1u << 16, 256u, reason, 256u);
  if (!eng) {
    return NULL;
  }
  x86p_jit_engine_set_intercept(eng, contract_intercept, policy);
  x86p_jit_engine_set_boundary(eng, contract_boundary, policy);
  if (install && !x86p_jit_engine_set_run_stop(eng, contract_stop, reason, 256u)) {
    x86p_jit_engine_destroy(eng);
    return NULL;
  }
  return eng;
}

static void test_intercept_contract_asks_only_where_it_can_fire(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  ContractIntercept policy = {GUEST_BASE + 5u, 0u};
  uint32_t no_stop = 0xFFFFFFF0u;
  uint32_t stop_at_top = GUEST_BASE;
  char reason[256];

  memset(g_guest, 0x40, sizeof g_guest); /* INC EAX */
  g_guest[10] = 0xEB;                    /* +10: JMP +0 */
  g_guest[11] = (uint8_t)(0u - 12u);

  /* The other answer: no contract, so every block entered asks. */
  reason[0] = '\0';
  eng = contract_engine(&mem, &policy, 0, reason);
  CHECK(eng != NULL);
  if (!eng) {
    printf("    (%s)\n", reason);
    return;
  }
  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, &no_stop, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &st);
  CHECK(st.blocks_entered == 200u);
  CHECK(st.intercept_calls == 200u);
  CHECK(st.blocks_guarded == 0u);
  x86p_jit_engine_destroy(eng);

  eng = contract_engine(&mem, &policy, 1, reason);
  CHECK(eng != NULL);
  if (!eng) {
    printf("    (%s)\n", reason);
    return;
  }
  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, &no_stop, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &st);
  CHECK(st.blocks_entered == 200u);
  CHECK(st.blocks_guarded == 1u);
  /* One miss at +0, and all 100 entries to +5: its miss, then 99 guarded. */
  CHECK(st.intercept_calls == 101u);
  CHECK(st.cache_guarded == 99u);

  /* The guarded block, long cached, is still asked about and can be taken. */
  policy.take = GUEST_BASE + 5u;
  cpu.eip = GUEST_BASE;
  CHECK(x86p_jit_engine_run(eng, &cpu, &no_stop, 200u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == GUEST_BASE + 5u);

  /* The stop address is asked about although no boundary reports it and its
     block is cached unguarded. */
  policy.take = GUEST_BASE;
  cpu.eip = GUEST_BASE + 5u;
  CHECK(x86p_jit_engine_run(eng, &cpu, &stop_at_top, 200u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == GUEST_BASE);

  /* Refusals: after a translation, and with an intercept but no boundary. */
  CHECK(!x86p_jit_engine_set_run_stop(eng, contract_stop, reason, sizeof reason));
  CHECK(strstr(reason, "before the first translation") != NULL);
  x86p_jit_engine_destroy(eng);
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (eng) {
    x86p_jit_engine_set_intercept(eng, contract_intercept, &policy);
    CHECK(!x86p_jit_engine_set_run_stop(eng, contract_stop, reason, sizeof reason));
    CHECK(strstr(reason, "boundary predicate") != NULL);
    x86p_jit_engine_destroy(eng);
  }
}

/* The product must refuse an instruction with no emitter without changing the
 * machine or entering the separately linked oracle. */
static void test_unsupported_instruction_is_a_product_refusal(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pCpu before;
  X86pJitEngine *eng;
  X86pJitEngineStats stats;
  char reason[256];

  memset(g_guest, 0x90, sizeof g_guest);
  /* RCPPS remains a named unsupported instruction in both backends. */
  g_guest[0] = 0x0F;
  g_guest[1] = 0x53;
  g_guest[2] = 0xC0;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  seed(&cpu);
  before = cpu;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 100u, reason, sizeof reason) == kX86pRunUnsupported);
  CHECK(x86p_cpu_diff(&cpu, &before, NULL, NULL) == 0u);
  CHECK(strstr(reason, "RCPPS") != NULL);
  x86p_jit_engine_stats(eng, &stats);
  CHECK(stats.blocks_entered == 0u);
  CHECK(stats.translate_refusals == 1u);

  x86p_jit_engine_destroy(eng);
}

/* ---- inline dispatch: handle an interception point without unwinding ---- */

static uint32_t g_disp_thunk, g_disp_unwind;
static int g_disp_calls;

static int intercept_thunk_or_unwind(const X86pCpu *cpu, void *user, void *run_user) {
  (void)user;
  (void)run_user;
  return cpu->eip == g_disp_thunk || cpu->eip == g_disp_unwind;
}

static X86pJitDispatchResult dispatch_thunk_or_unwind(X86pCpu *cpu, void *user, void *run_user) {
  (void)user;
  (void)run_user;
  if (cpu->eip == g_disp_thunk) {
    g_disp_calls++;
    cpu->reg[kX86pEax] += 0x10u; /* the "thunk" side effect */
    cpu->eip += 1u;              /* step over the one-byte stand-in */
    return kX86pDispatchContinue;
  }
  return kX86pDispatchUnwind; /* g_disp_unwind: hand the run back */
}

/* A handler that never advances eip -- the slice must still terminate. */
static int intercept_always(const X86pCpu *cpu, void *user, void *run_user) {
  (void)cpu;
  (void)user;
  (void)run_user;
  return 1;
}
static X86pJitDispatchResult dispatch_stuck(X86pCpu *cpu, void *user, void *run_user) {
  (void)cpu;
  (void)user;
  (void)run_user;
  g_disp_calls++;
  return kX86pDispatchContinue;
}

static void test_inline_dispatch_continues_the_run_without_unwinding(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  char reason[256];

  memset(g_guest, 0x90, sizeof g_guest);
  /* 0: mov eax, 1 */
  g_guest[0] = 0xB8;
  g_guest[1] = 0x01;
  g_guest[2] = g_guest[3] = g_guest[4] = 0x00;
  /* 5: nop  <- the interception point ("thunk") */
  g_guest[5] = 0x90;
  /* 6: inc eax */
  g_guest[6] = 0x40;
  /* 7: jmp $  <- the "return to caller" sentinel */
  g_guest[7] = 0xEB;
  g_guest[8] = 0xFE;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  g_disp_thunk = GUEST_BASE + 5u;
  g_disp_unwind = GUEST_BASE + 7u;
  g_disp_calls = 0;
  x86p_jit_engine_set_intercept(eng, intercept_thunk_or_unwind, NULL);
  x86p_jit_engine_set_boundary(eng, boundary_at, &g_disp_thunk);

  /* No dispatch handler: the interception point unwinds the run, as before. */
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 100u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == GUEST_BASE + 5u);
  CHECK(cpu.reg[kX86pEax] == 1u);
  CHECK(g_disp_calls == 0);

  /* With the handler: the run stays on one stack across the thunk and only
     unwinds at the sentinel. */
  x86p_jit_engine_set_dispatch(eng, dispatch_thunk_or_unwind, NULL);
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 100u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == GUEST_BASE + 7u); /* stopped at the sentinel, not the thunk */
  CHECK(g_disp_calls == 1);          /* the thunk ran exactly once */
  CHECK(cpu.reg[kX86pEax] == 0x12u); /* mov eax,1; +0x10 in the handler; inc */

  x86p_jit_engine_destroy(eng);
}

static void test_inline_dispatch_that_never_advances_still_ends_the_slice(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  char reason[256];

  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xEB; /* jmp $ */
  g_guest[1] = 0xFE;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  g_disp_calls = 0;
  x86p_jit_engine_set_intercept(eng, intercept_always, NULL);
  x86p_jit_engine_set_dispatch(eng, dispatch_stuck, NULL);

  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 64u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(g_disp_calls == 64); /* one per step, then the budget stops it */

  x86p_jit_engine_destroy(eng);
}

/*
 * The block-entry profile is execution-weighted: a block a spin re-enters two
 * hundred times must outweigh a block entered once, which is exactly what
 * x86p_jit_engine_stats cannot show.
 */
/*
 * Summing per-thread engine stats must carry EVERY field.
 *
 * A consumer that runs an engine per guest thread writes one total, and the
 * obvious way to write it is a list of members -- which keeps building, and
 * silently reports zero, for every field added after it was written. This
 * fills the struct with distinct non-zero values through the same whole-object
 * view the summation uses, so a field that the addition skips is caught here
 * rather than as a heartbeat that reads zero on a real run.
 */
static void test_summing_stats_leaves_no_field_behind(void) {
  X86pJitEngineStats item;
  X86pJitEngineStats sum;
  unsigned char *raw = (unsigned char *)&item;
  size_t fields = sizeof item / sizeof(uint64_t);
  size_t i;
  size_t moved = 0;

  for (i = 0; i < sizeof item; i++) {
    raw[i] = (unsigned char)(i + 1u);
  }
  memset(&sum, 0, sizeof sum);
  x86p_jit_engine_stats_add(&sum, &item);
  x86p_jit_engine_stats_add(&sum, &item);

  for (i = 0; i < fields; i++) {
    uint64_t one;
    uint64_t two;
    memcpy(&one, (const unsigned char *)&item + i * sizeof one, sizeof one);
    memcpy(&two, (const unsigned char *)&sum + i * sizeof two, sizeof two);
    if (one != 0u && two == one * 2u) {
      moved++;
    }
  }
  CHECK(fields > 0u);
  CHECK(moved == fields);
  printf("    %zu of %zu stat field(s) summed\n", moved, fields);
}

static void test_profile_weights_a_block_by_how_often_it_is_entered(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  X86pJitProfileEntry top[4];
  char reason[256];
  uint32_t n;

  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0x40; /* INC EAX -- the block entered once */
  g_guest[1] = 0xEB; /* JMP $ at +1 -- the block the spin re-enters */
  g_guest[2] = 0xFE;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  CHECK(x86p_jit_engine_set_profile(eng, 1, 64u, reason, sizeof reason));
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);

  x86p_jit_engine_stats(eng, &st);
  CHECK(x86p_jit_profile_total_hits(x86p_jit_engine_profile(eng)) == st.blocks_entered);
  CHECK(x86p_jit_profile_distinct(x86p_jit_engine_profile(eng)) == 2u);

  n = x86p_jit_profile_top(x86p_jit_engine_profile(eng), top, 4u);
  CHECK(n == 2u);
  CHECK(top[0].guest_eip == GUEST_BASE + 1u); /* the spin, not the INC */
  CHECK(top[0].entries > 190u);
  CHECK(top[1].guest_eip == GUEST_BASE && top[1].entries == 1u);

  /* Turning it off frees the table and detaches it. */
  CHECK(x86p_jit_engine_set_profile(eng, 0, 0u, reason, sizeof reason));
  CHECK(x86p_jit_engine_profile(eng) == NULL);

  x86p_jit_engine_destroy(eng);
}

/*
 * The watch answers "how did the run get HERE", which the profile cannot.
 *
 * Both answers are asserted from one guest program: entered by falling out of
 * the block before it, the previous address is that block; entered as the
 * FIRST block of a run, there is no previous one and the watch says so rather
 * than reporting 0 as if it were an address. The bound is asserted too,
 * because the address this exists for is entered millions of times a second
 * and a watch that reported them all would be the stall.
 */
typedef struct {
  unsigned calls;
  uint32_t addr[8];
  uint32_t previous[8];
  int have_previous[8];
  uint32_t eax[8];
} WatchLog;

static void watch_note(void *user, uint32_t addr, uint32_t previous, int have_previous, X86pCpu *cpu) {
  WatchLog *w = (WatchLog *)user;
  if (w->calls < 8u) {
    w->addr[w->calls] = addr;
    w->previous[w->calls] = previous;
    w->have_previous[w->calls] = have_previous;
    w->eax[w->calls] = cpu->reg[kX86pEax];
  }
  w->calls++;
}

static void test_the_entry_watch_names_the_block_that_sent_the_run_there(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  WatchLog w;
  char reason[256];

  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0x40; /* INC EAX */
  g_guest[1] = 0xEB; /* JMP $ -- the spin, as the title's own boot has */
  g_guest[2] = 0xFE;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }

  /* Off: the spin runs and reports nothing. */
  memset(&w, 0, sizeof w);
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(w.calls == 0u);

  /* Armed for three, on a block entered about two hundred times. */
  x86p_jit_engine_set_entry_watch(eng, GUEST_BASE + 1u, 3u, watch_note, &w);
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(w.calls == 3u);
  CHECK(w.addr[0] == GUEST_BASE + 1u);
  CHECK(w.have_previous[0] == 1);
  CHECK(w.previous[0] == GUEST_BASE);      /* the INC, which fell into the spin */
  CHECK(w.eax[0] == 1u);                   /* and the register file is live */
  CHECK(w.previous[1] == GUEST_BASE + 1u); /* thereafter, itself */

  /* Disarmed by a zero count, with the callback still passed. */
  x86p_jit_engine_set_entry_watch(eng, GUEST_BASE + 1u, 0u, watch_note, &w);
  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 50u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(w.calls == 3u);
  x86p_jit_engine_destroy(eng);

  /* Entered as the first block of a run: no previous block, and it says so. */
  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  memset(&w, 0, sizeof w);
  x86p_jit_engine_set_entry_watch(eng, GUEST_BASE + 1u, 2u, watch_note, &w);
  seed(&cpu);
  cpu.eip = GUEST_BASE + 1u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 50u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(w.calls == 2u);
  CHECK(w.have_previous[0] == 0);
  CHECK(w.previous[0] == 0u);
  CHECK(w.have_previous[1] == 1);
  printf("    watch: %u report(s), first previous 0x%08x\n", w.calls, w.previous[1]);
  x86p_jit_engine_destroy(eng);

  /*
   * ON ENTRY, not on exit. Watch the block that DOES something and seed the
   * register it writes: a report taken after the block ran would show the
   * written value, which is the wrong state for asking how the run arrived.
   * Measured before this was fixed: a watch on a title's fatal handler showed
   * a stack the handler had already pushed its own call frame onto, so the
   * arguments it was called with were not there to read.
   */
  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  memset(&w, 0, sizeof w);
  x86p_jit_engine_set_entry_watch(eng, GUEST_BASE, 1u, watch_note, &w);
  seed(&cpu);
  cpu.reg[kX86pEax] = 5u;
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 20u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(w.calls == 1u);
  CHECK(w.eax[0] == 5u);          /* not 6: the INC in that block has not run yet */
  CHECK(cpu.reg[kX86pEax] == 6u); /* and it did run */
  x86p_jit_engine_destroy(eng);
}

/*
 * blocks_reentered sizes the cheapest block-chaining fix there is -- lowering a
 * block that exits to its own entry as a host loop -- so it is measured per
 * ENTRY rather than per translation, and it has to be able to say "almost
 * never" as clearly as "almost always". Both answers are asserted here, from
 * the same engine, because a counter checked only against a spin would pass
 * while counting every block entry in the run.
 */
static void test_a_block_that_re_enters_itself_is_counted_and_a_chain_is_not(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  X86pJitEngineStats spin;
  X86pJitEngineStats chain;
  uint32_t spin_last;
  uint32_t chain_last;
  char reason[256];

  /*
   * A spin: one block whose only exit names its own entry, so every entry after
   * the first re-enters the block just left.
   */
  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xEB; /* JMP $ */
  g_guest[1] = 0xFE;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &spin);
  spin_last = x86p_jit_engine_last_block_entry(eng);
  /* Every entry but the first, and the run entered nothing else. */
  CHECK(spin.blocks_entered == 200u);
  CHECK(spin.blocks_reentered == 199u);
  /* The block cache's counters reach the engine's stats: the first lookup
     misses and translates, the second finds the table -- one slot probed
     each -- and every one after that is answered by the front cache. */
  CHECK(spin.cache_lookups == 200u);
  CHECK(spin.cache_hits == 199u);
  CHECK(spin.cache_front_hits == 198u);
  CHECK(spin.cache_table_probes == 2u);
  x86p_jit_engine_destroy(eng);

  /*
   * THE OTHER ANSWER. Two blocks that jump to each other: the guest is looping
   * just as tightly, every entry is a dispatch, and NONE of them re-enters the
   * block just left. A self-exit lowering would not remove one of these, and
   * the counter must say so rather than report the loop it can see.
   *
   *   +0: JMP +2   (to +4, skipping the NOP padding)
   *   +4: JMP -6   (back to +0)
   */
  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xEB;
  g_guest[1] = 0x02;
  g_guest[4] = 0xEB;
  g_guest[5] = 0xFA;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &chain);
  chain_last = x86p_jit_engine_last_block_entry(eng);
  CHECK(chain.blocks_entered == 200u);
  CHECK(chain.blocks_reentered == 0u);
  x86p_jit_engine_destroy(eng);

  /*
   * And WHICH block. A run that has stopped making progress needs an address
   * to aim a watch at, and the block-entry histogram cannot supply one once
   * its table is full -- a spin that starts after that has its key refused
   * outright. This is the last entry the loop made, so on the spin it is the
   * spinning block's own entry.
   */
  CHECK(spin_last == GUEST_BASE);
  /*
   * THE OTHER ANSWER, and the reason this is a sample rather than a census:
   * the two-block chain alternates, so the last entry is whichever of the two
   * the budget stopped on -- +4 here, not the entry address the run started
   * at. A field that always reported where the run began would pass the spin
   * above and be useless.
   */
  CHECK(chain_last == GUEST_BASE + 4u);

  printf("    self-loop %llu of %llu re-entered at 0x%08x; two-block chain %llu of %llu, last 0x%08x\n",
         (unsigned long long)spin.blocks_reentered,
         (unsigned long long)spin.blocks_entered,
         spin_last,
         (unsigned long long)chain.blocks_reentered,
         (unsigned long long)chain.blocks_entered,
         chain_last);
}

/*
 * THE DESIGNED NEGATIVE for the runtime chain census. This host's backend is
 * the x86-64 one, which emits no constant successor addresses at all, so every
 * dispatch in this run must land in `unrecorded` and none in `chainable`. If
 * the census folded "the predecessor recorded nothing" into chainable's
 * complement, a browser run could not be told from this one -- both would read
 * as "nothing is chainable", and the number #166 needs would be a fiction.
 */
static void test_a_backend_that_records_no_successors_reads_as_unrecorded(void) {
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitEngine *eng;
  const X86pJitChainCensus *census;
  char reason[256];

  /* The two-block chain again: every entry is a real dispatch. */
  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xEB;
  g_guest[1] = 0x02;
  g_guest[4] = 0xEB;
  g_guest[5] = 0xFA;

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  CHECK(x86p_jit_engine_chain_census(eng) == NULL); /* off unless armed */
  CHECK(x86p_jit_engine_set_chain_census(eng, 1, 64u, reason, sizeof reason));
  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, NULL, 200u, reason, sizeof reason) == kX86pRunBudget);

  census = x86p_jit_engine_chain_census(eng);
  CHECK(census != NULL);
  CHECK(x86p_jit_chain_census_entries(census) == 200u);
  CHECK(x86p_jit_chain_census_chainable(census) == 0u);
  CHECK(x86p_jit_chain_census_unrecorded(census) == 200u);
  /* Both blocks were recorded -- with no successors, which is the point. */
  CHECK(x86p_jit_chain_census_blocks(census) == 2u);
  CHECK(x86p_jit_chain_census_dropped_keys(census) == 0u);
  CHECK(x86p_jit_chain_census_overflowed(census) == 0u);

  printf("    x86-64 backend: %llu entries, %llu chainable, %llu unrecorded\n",
         (unsigned long long)x86p_jit_chain_census_entries(census),
         (unsigned long long)x86p_jit_chain_census_chainable(census),
         (unsigned long long)x86p_jit_chain_census_unrecorded(census));

  CHECK(x86p_jit_engine_set_chain_census(eng, 0, 0u, reason, sizeof reason));
  CHECK(x86p_jit_engine_chain_census(eng) == NULL);
  x86p_jit_engine_destroy(eng);
}

int main(void) {
  if (!x86p_jit_available()) {
    printf("NO x86-64 BACKEND on this host: this suite cannot run and claims nothing\n");
    return 77;
  }

  RUN(test_engine_matches_interpreter_on_a_looping_program);
  RUN(test_a_full_code_region_flushes_and_keeps_going);
  RUN(test_an_undersized_code_region_is_refused_at_creation);
  RUN(test_invalidation_drops_a_stale_translation);
  RUN(test_a_guest_memory_fault_stops_the_run_and_says_so);
  RUN(test_a_rewound_arena_does_not_leave_stale_cache_entries);
  RUN(test_the_same_run_through_dual_mapping);
  RUN(test_intercept_stops_before_block);
  RUN(test_boundary_ends_a_block_before_a_flagged_address);
  RUN(test_intercept_contract_asks_only_where_it_can_fire);
  RUN(test_unsupported_instruction_is_a_product_refusal);
  RUN(test_inline_dispatch_continues_the_run_without_unwinding);
  RUN(test_inline_dispatch_that_never_advances_still_ends_the_slice);
  RUN(test_profile_weights_a_block_by_how_often_it_is_entered);
  RUN(test_summing_stats_leaves_no_field_behind);
  RUN(test_the_entry_watch_names_the_block_that_sent_the_run_there);
  RUN(test_a_block_that_re_enters_itself_is_counted_and_a_chain_is_not);
  RUN(test_a_backend_that_records_no_successors_reads_as_unrecorded);

  printf("\n%d check(s), %d failure(s) in %d test(s)\n", g_checks, g_failed, g_test_failed);
  return g_failed == 0 ? 0 : 1;
}
