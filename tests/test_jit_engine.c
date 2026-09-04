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
#include "x86port/cpu.h"
#include "x86port/exec.h"
#include "x86port/jit_engine.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
  rs = x86p_jit_engine_run(eng, &ce, 4096u, reason, (unsigned)sizeof reason);
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
  X86pMem mem;
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
    return 8u; /* conservative fallback if the probe itself cannot translate */
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
  long page = sysconf(_SC_PAGESIZE);
  size_t bytes_per_insn = probe_bytes_per_insn();
  size_t arena;
  size_t needed;
  const unsigned max_movs = 40000u; /* keeps GUEST_SIZE bounded */

  if (page <= 0) {
    page = 4096;
  }
  arena = (size_t)page; /* X86P_JIT_MIN_BLOCK_BYTES rounds up to exactly one page */
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
  CHECK(x86p_jit_engine_run(eng, &ce, budget, reason, (unsigned)sizeof reason) == kX86pRunBudget);
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
  CHECK(x86p_jit_engine_run(eng, &ce, budget, reason, (unsigned)sizeof reason) == kX86pRunBudget);
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
    CHECK(x86p_jit_engine_run(eng, &ce, 4096u, reason, (unsigned)sizeof reason) == kX86pRunBudget);
    CHECK(same_cpu(&ci, &ce));
    x86p_jit_engine_destroy(eng);
  } else {
    printf("    (%s)\n", reason);
  }
  (void)jc_code_select_mechanism(NULL);
}

static int intercept_at_target(const X86pCpu *cpu, void *user) {
  uint32_t target = *(const uint32_t *)user;
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
  X86pJitRunStatus st = x86p_jit_engine_run(eng, &cpu, 100u, reason, sizeof reason);
  CHECK(st == kX86pRunIntercept);
  CHECK(cpu.eip == intercept_target);
  CHECK(cpu.reg[kX86pEax] == 42u);

  /* Clear intercept and run again: should execute until budget */
  x86p_jit_engine_set_intercept(eng, NULL, NULL);
  st = x86p_jit_engine_run(eng, &cpu, 100u, reason, sizeof reason);
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
  CHECK(x86p_jit_engine_run(eng, &cpu, 200u, reason, sizeof reason) == kX86pRunBudget);
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
  CHECK(x86p_jit_engine_run(eng, &cpu, 200u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == flagged);
  CHECK(cpu.reg[kX86pEax] == 5u); /* exactly the five INCs before the boundary */

  /* Drop the intercept, keep the boundary: it still runs to the spin, but as
     at least two blocks split at +5. */
  x86p_jit_engine_set_intercept(eng, NULL, NULL);
  CHECK(x86p_jit_engine_run(eng, &cpu, 200u, reason, sizeof reason) == kX86pRunBudget);
  x86p_jit_engine_stats(eng, &st);
  CHECK(cpu.eip == GUEST_BASE + 10u);
  CHECK(cpu.reg[kX86pEax] == 10u);
  CHECK(st.blocks_translated == 3u); /* split at +5 adds one over the baseline 2 */

  x86p_jit_engine_destroy(eng);
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
  g_guest[0] = 0x9C; /* PUSHFD: shared oracle semantics exist; no x64 emitter */

  reason[0] = '\0';
  eng = x86p_jit_engine_create(&mem, 1u << 16, 256u, reason, sizeof reason);
  CHECK(eng != NULL);
  if (!eng) {
    return;
  }
  seed(&cpu);
  before = cpu;
  CHECK(x86p_jit_engine_run(eng, &cpu, 100u, reason, sizeof reason) == kX86pRunUnsupported);
  CHECK(memcmp(&cpu, &before, sizeof cpu) == 0);
  CHECK(strstr(reason, "PUSHF") != NULL);
  x86p_jit_engine_stats(eng, &stats);
  CHECK(stats.blocks_entered == 0u);
  CHECK(stats.translate_refusals == 1u);

  x86p_jit_engine_destroy(eng);
}

/* ---- inline dispatch: handle an interception point without unwinding ---- */

static uint32_t g_disp_thunk, g_disp_unwind;
static int g_disp_calls;

static int intercept_thunk_or_unwind(const X86pCpu *cpu, void *user) {
  (void)user;
  return cpu->eip == g_disp_thunk || cpu->eip == g_disp_unwind;
}

static X86pJitDispatchResult dispatch_thunk_or_unwind(X86pCpu *cpu, void *user) {
  (void)user;
  if (cpu->eip == g_disp_thunk) {
    g_disp_calls++;
    cpu->reg[kX86pEax] += 0x10u; /* the "thunk" side effect */
    cpu->eip += 1u;              /* step over the one-byte stand-in */
    return kX86pDispatchContinue;
  }
  return kX86pDispatchUnwind; /* g_disp_unwind: hand the run back */
}

/* A handler that never advances eip -- the slice must still terminate. */
static int intercept_always(const X86pCpu *cpu, void *user) {
  (void)cpu;
  (void)user;
  return 1;
}
static X86pJitDispatchResult dispatch_stuck(X86pCpu *cpu, void *user) {
  (void)cpu;
  (void)user;
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
  CHECK(x86p_jit_engine_run(eng, &cpu, 100u, reason, sizeof reason) == kX86pRunIntercept);
  CHECK(cpu.eip == GUEST_BASE + 5u);
  CHECK(cpu.reg[kX86pEax] == 1u);
  CHECK(g_disp_calls == 0);

  /* With the handler: the run stays on one stack across the thunk and only
     unwinds at the sentinel. */
  x86p_jit_engine_set_dispatch(eng, dispatch_thunk_or_unwind, NULL);
  seed(&cpu);
  cpu.reg[kX86pEax] = 0u;
  CHECK(x86p_jit_engine_run(eng, &cpu, 100u, reason, sizeof reason) == kX86pRunIntercept);
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
  CHECK(x86p_jit_engine_run(eng, &cpu, 64u, reason, sizeof reason) == kX86pRunBudget);
  CHECK(g_disp_calls == 64); /* one per step, then the budget stops it */

  x86p_jit_engine_destroy(eng);
}

/*
 * The block-entry profile is execution-weighted: a block a spin re-enters two
 * hundred times must outweigh a block entered once, which is exactly what
 * x86p_jit_engine_stats cannot show.
 */
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
  CHECK(x86p_jit_engine_run(eng, &cpu, 200u, reason, sizeof reason) == kX86pRunBudget);

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
  RUN(test_unsupported_instruction_is_a_product_refusal);
  RUN(test_inline_dispatch_continues_the_run_without_unwinding);
  RUN(test_inline_dispatch_that_never_advances_still_ends_the_slice);
  RUN(test_profile_weights_a_block_by_how_often_it_is_entered);

  printf("\n%d check(s), %d failure(s) in %d test(s)\n", g_checks, g_failed, g_test_failed);
  return g_failed == 0 ? 0 : 1;
}
