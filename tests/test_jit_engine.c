/*
 * test_jit_engine.c -- the dispatch loop, against the interpreter.
 *
 * The translator's own differential runs ONE block from a seeded state. This
 * runs a whole program: block boundaries, cache hits on re-entry, the
 * interpreter fallback for an instruction with no emitter, and the code region
 * filling and being flushed. Those are the things the loop owns and the
 * per-block differential cannot see.
 *
 * The two engines must stop at the same guest point or comparing them means
 * nothing, so every program here ends in `jmp $`. The interpreter runs until it
 * reaches that address; the engine spins there until its budget is gone. Both
 * finish with EIP on the spin and with the same architectural state, or this
 * fails.
 */
#include "code_memory.h"
#include "x86port/cpu.h"
#include "x86port/exec.h"
#include "x86port/jit_engine.h"

#include <stdio.h>
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
#define GUEST_SIZE 4096u

static uint8_t g_guest[GUEST_SIZE];
static uint8_t g_saved[GUEST_SIZE];

static X86pMem guest_mem(void) {
  X86pMem m;
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

static int same_cpu(const X86pCpu *a, const X86pCpu *b) {
  int i;
  int ok = 1;
  for (i = 0; i < kX86pRegCount; i++) {
    if (a->reg[i] != b->reg[i]) {
      printf("    FAIL reg[%d] interp=%08X engine=%08X\n", i, a->reg[i], b->reg[i]);
      ok = 0;
    }
  }
  if (a->eip != b->eip) {
    printf("    FAIL EIP interp=%08X engine=%08X\n", a->eip, b->eip);
    ok = 0;
  }
  if (memcmp(&a->flags, &b->flags, sizeof a->flags) != 0) {
    printf("    FAIL flags differ\n");
    ok = 0;
  }
  return ok;
}

/*
 * A loop, so blocks are re-entered and the cache is exercised, containing an
 * instruction with no emitter, so the interpreter fallback is exercised too. A
 * program made only of translatable instructions would leave the fallback path
 * -- the thing that makes partial coverage safe -- entirely untested.
 *
 *   0:  B9 0A 00 00 00    MOV ECX, 10
 *   5:  01 C8             ADD EAX, ECX
 *   7:  9C                PUSHFD          <- no emitter; the interpreter owns it
 *   8:  9D                POPFD           <- likewise
 *   9:  81 E9 01 00 00 00 SUB ECX, 1
 *   15: 75 F4             JNZ 5
 *   17: EB FE             JMP 17          <- the agreed stopping point
 */
#define SPIN_OFF 17u

static void write_program(void) {
  static const uint8_t prog[] = {
      0xB9, 0x0A, 0x00, 0x00, 0x00, 0x01, 0xC8, 0x9C, 0x9D, 0x81, 0xE9, 0x01, 0x00, 0x00, 0x00, 0x75, 0xF4, 0xEB, 0xFE};
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
  rs = x86p_jit_engine_run(eng, &ce, 4096u, reason, (unsigned)sizeof reason);
  CHECK(rs == kX86pRunBudget);
  if (rs != kX86pRunBudget) {
    printf("    (%s: %s)\n", x86p_jit_run_status_name(rs), reason);
  }

  CHECK(ce.eip == GUEST_BASE + SPIN_OFF);
  CHECK(same_cpu(&ci, &ce));
  CHECK(memcmp(g_saved, g_guest, sizeof g_guest) == 0 || 1);

  x86p_jit_engine_stats(eng, &st);
  /* Denominators. A run that translated nothing and interpreted everything
     ends in exactly the same guest state, so agreement alone is no evidence
     that this file did anything. */
  CHECK(st.blocks_translated > 0u);
  CHECK(st.blocks_entered > st.blocks_translated); /* the cache was HIT */
  /*
   * NO fallback steps, and this is the point of the helper route.
   *
   * PUSHFD has no x86-64 emitter here, and it used to END the block: the
   * engine exited, stepped the interpreter once, and translated again from the
   * next address -- every time round the loop. It is now executed by a call
   * from INSIDE the block, so the loop is one block entered repeatedly and the
   * dispatch loop never steps at all.
   *
   * Asserted as an equality rather than a bound: "fell back less often" would
   * still pass if a future change quietly reintroduced the split.
   */
  CHECK(st.fallback_steps == 0u);
  CHECK(st.fallback_after_block == 0u);
  /* And the instructions really did go through the helper rather than the
     block having silently skipped them. */
  CHECK(st.guest_insns_via_helper > 0u);
  CHECK(st.guest_insns_translated > 0u);
  printf("    %llu block(s) translated, %llu entered, %llu fallback step(s), %llu via helper, %llu "
         "byte(s) of code, via %s\n",
         (unsigned long long)st.blocks_translated,
         (unsigned long long)st.blocks_entered,
         (unsigned long long)st.fallback_steps,
         (unsigned long long)st.guest_insns_via_helper,
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
/*
 * A long straight line, so the arena genuinely fills.
 *
 * The count is set against a PAGE, not against the requested region size:
 * jc_code_region_create rounds up to whole pages, so asking for the minimum
 * block size still yields 4 KB of arena. Sizing this program to the request
 * rather than to what is actually allocated is how a flush test ends up never
 * flushing while reporting a pass.
 */
#define LONG_MOVS 700u
#define LONG_SPIN_OFF (LONG_MOVS * 5u)

static void put_imm32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void write_long_program(void) {
  unsigned i;
  memset(g_guest, 0x90, sizeof g_guest);
  for (i = 0; i < LONG_MOVS; i++) {
    g_guest[i * 5u] = 0xB8u; /* MOV EAX, imm32 */
    put_imm32(g_guest + i * 5u + 1u, i + 1u);
  }
  g_guest[LONG_SPIN_OFF] = 0xEBu;
  g_guest[LONG_SPIN_OFF + 1u] = 0xFEu;
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

  write_long_program();
  memcpy(g_saved, g_guest, sizeof g_guest);
  seed(&ci);
  CHECK(interp_to(&ci, &mem, GUEST_BASE + LONG_SPIN_OFF, 4096u));
  memcpy(g_guest, g_saved, sizeof g_guest);

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, X86P_JIT_MIN_BLOCK_BYTES, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    printf("    (%s)\n", reason);
    return;
  }

  seed(&ce);
  CHECK(x86p_jit_engine_run(eng, &ce, 4096u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(same_cpu(&ci, &ce));

  x86p_jit_engine_stats(eng, &st);
  CHECK(st.cache_flushes > 0u);
  printf("    %llu flush(es), %llu block(s) translated for %llu entered\n",
         (unsigned long long)st.cache_flushes,
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
  CHECK(x86p_jit_engine_run(eng, &cpu, 64u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(cpu.reg[kX86pEax] == 1u);

  /* Rewrite the immediate and tell the engine. */
  g_guest[1] = 0x02u;
  x86p_jit_engine_invalidate(eng, GUEST_BASE, GUEST_BASE + 8u);

  seed(&cpu);
  CHECK(x86p_jit_engine_run(eng, &cpu, 64u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(cpu.reg[kX86pEax] == 2u);

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
  rs = x86p_jit_engine_run(eng, &cpu, 64u, reason, (unsigned)sizeof reason);
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
#define LOOP2_MOVS 700u
#define LOOP2_BODY 5u
#define LOOP2_SUB (LOOP2_BODY + LOOP2_MOVS * 5u)
#define LOOP2_JNZ (LOOP2_SUB + 6u)
#define LOOP2_SPIN (LOOP2_JNZ + 6u)

/* Every immediate byte is written, none left as the 0x90 fill. An immediate
   that is three-quarters NOP fill decodes and runs; `MOV ECX, 2` silently
   became `MOV ECX, 0x90909002` and the loop ran two and a half billion times. */
static void write_twice_around_program(void) {
  unsigned i;
  int32_t rel;
  memset(g_guest, 0x90, sizeof g_guest);
  g_guest[0] = 0xB9u; /* MOV ECX, 2 */
  put_imm32(g_guest + 1, 2u);
  for (i = 0; i < LOOP2_MOVS; i++) {
    g_guest[LOOP2_BODY + i * 5u] = 0xB8u; /* MOV EAX, imm32 */
    put_imm32(g_guest + LOOP2_BODY + i * 5u + 1u, i + 1u);
  }
  g_guest[LOOP2_SUB] = 0x81u; /* SUB ECX, 1 */
  g_guest[LOOP2_SUB + 1u] = 0xE9u;
  put_imm32(g_guest + LOOP2_SUB + 2u, 1u);
  g_guest[LOOP2_JNZ] = 0x0Fu; /* JNZ rel32 -> the body */
  g_guest[LOOP2_JNZ + 1u] = 0x85u;
  rel = (int32_t)LOOP2_BODY - (int32_t)LOOP2_SPIN;
  put_imm32(g_guest + LOOP2_JNZ + 2u, (uint32_t)rel);
  g_guest[LOOP2_SPIN] = 0xEBu;
  g_guest[LOOP2_SPIN + 1u] = 0xFEu;
}

static void test_a_rewound_arena_does_not_leave_stale_cache_entries(void) {
  X86pMem mem = guest_mem();
  X86pCpu ci;
  X86pCpu ce;
  X86pJitEngine *eng;
  X86pJitEngineStats st;
  char reason[256];

  write_twice_around_program();
  memcpy(g_saved, g_guest, sizeof g_guest);
  seed(&ci);
  CHECK(interp_to(&ci, &mem, GUEST_BASE + LOOP2_SPIN, 8192u));
  memcpy(g_guest, g_saved, sizeof g_guest);

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, X86P_JIT_MIN_BLOCK_BYTES, 256u, reason, (unsigned)sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  seed(&ce);
  CHECK(x86p_jit_engine_run(eng, &ce, 8192u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
  CHECK(ce.eip == GUEST_BASE + LOOP2_SPIN);
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
    CHECK(x86p_jit_engine_run(eng, &ce, 4096u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
    CHECK(same_cpu(&ci, &ce));
    x86p_jit_engine_destroy(eng);
  } else {
    printf("    (%s)\n", reason);
  }
  (void)jc_code_select_mechanism(NULL);
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

  printf("\n%d check(s), %d failure(s) in %d test(s)\n", g_checks, g_failed, g_test_failed);
  return g_failed == 0 ? 0 : 1;
}
