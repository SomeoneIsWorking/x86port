/*
 * test_wasm_leaves.c -- leaves on the engine the browser product runs
 * (jit_wasm_leaf.h), against the interpreter.
 *
 * The x86-64 suite (test_jit_engine.c) holds the same contract for its
 * backend; this one runs the WebAssembly blocks themselves, so the leaf is
 * reached through the block's import and the main module's table, and a site
 * is read and filled as linear memory by emitted code.
 *
 * Every leaf does what its callee does, so each route -- completed, declined,
 * refilled, exhausted -- must end in the interpreter's state. The counts say
 * which route was taken; the state says it was taken correctly.
 *
 * Every case runs over a flat mapping, then over one with a page permission
 * table as the product maps its guest, and then again with the engine's own
 * data -- its chain slots, block front and leaf sites -- above 128 MiB of
 * linear memory, as the product's is. Each of those addresses is a constant
 * in the emitted code, and there it takes the longest encoding a constant has.
 * A leaf CALL carries two chained exits besides; the product's first one was
 * refused as oversized until the size check stopped charging an instruction
 * for the chained exits the chain reserve pays for.
 */
#include "cpu.h"
#include "cpu_compare.h"
#include "exec.h"
#include "jit_engine.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kGuestBase = 0x400000u, kGuestBytes = 0x4000u, kStack = 0x3F00u, kPageShift = 12u };

static uint8_t guest[kGuestBytes];
static uint8_t perms[kGuestBytes >> kPageShift];
static int with_perms; /* map the guest through `perms` */
static unsigned checks;
static unsigned failures;
static const char *current;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s: %s\n", current, message);
  }
}

static X86pMem guest_mem(void) {
  X86pMem mem = {.host = guest, .lo = kGuestBase, .size = sizeof guest};
  if (with_perms) {
    memset(perms, kX86pMemRead | kX86pMemWrite, sizeof perms);
    mem.perms = perms;
    mem.page_shift = kPageShift;
  }
  return mem;
}

static void seed(X86pCpu *cpu) {
  x86p_cpu_reset(cpu);
  cpu->reg[kX86pEax] = 0x1000u;
  cpu->reg[kX86pEsp] = kGuestBase + kStack;
  cpu->eip = kGuestBase;
}

static void put32(unsigned offset, uint32_t value) {
  memcpy(guest + offset, &value, sizeof value);
}

static void rel32(unsigned at, unsigned opcode_bytes, unsigned target) {
  put32(at + opcode_bytes, target - (at + opcode_bytes + 4u));
}

static void report_diff(const char *field, const char *a, const char *b, void *user) {
  (void)user;
  printf("FAIL: %s: %s interpreter=%s engine=%s\n", current, field, a, b);
}

/* The interpreter's state at `spin`, the JMP $ every program ends in. */
static void interpret(X86pCpu *cpu, uint32_t spin) {
  X86pMem mem = guest_mem();
  seed(cpu);
  for (unsigned i = 0; i < 100000u && cpu->eip != kGuestBase + spin; i++) {
    if (x86p_step(cpu, &mem, NULL) != kX86pStepOk) {
      break;
    }
  }
  check(cpu->eip == kGuestBase + spin, "the interpreter did not reach the program's end");
}

/* ---- the leaves --------------------------------------------------------- */

/* One callee, ADD EAX, add; RET, and the address a call must return to. */
typedef struct Callee {
  uint32_t at;
  uint32_t add;
} Callee;

typedef struct LeafRecord {
  int decline;
  uint32_t returns_to; /* 0: any address that is a CALL's next instruction */
  unsigned calls;
  unsigned wrong; /* calls that did not arrive as the callee would */
} LeafRecord;

static LeafRecord record;
static Callee callees[2];

static int complete(X86pCpu *cpu, const Callee *callee) {
  const uint32_t esp = cpu->reg[kX86pEsp];
  uint32_t ret;
  memcpy(&ret, guest + (esp - kGuestBase), sizeof ret);
  record.calls++;
  if (cpu->eip != kGuestBase + callee->at || (record.returns_to && ret != kGuestBase + record.returns_to)) {
    record.wrong++;
  }
  if (record.decline) {
    return 0;
  }
  cpu->reg[kX86pEax] += callee->add;
  cpu->reg[kX86pEsp] = esp + 4u;
  return 1;
}

static int leaf_a(X86pCpu *cpu) {
  return complete(cpu, &callees[0]);
}

static int leaf_b(X86pCpu *cpu) {
  return complete(cpu, &callees[1]);
}

static X86pJitLeafFn resolve(uint32_t target, void *user) {
  (void)user;
  if (target == kGuestBase + callees[0].at) {
    return leaf_a;
  }
  return target == kGuestBase + callees[1].at ? leaf_b : NULL;
}

static void callee(unsigned index, uint32_t at, uint32_t add) {
  callees[index] = (Callee){at, add};
  guest[at] = 0x83; /* ADD EAX, imm8; RET */
  guest[at + 1u] = 0xC0;
  guest[at + 2u] = (uint8_t)add;
  guest[at + 3u] = 0xC3;
}

/* ---- the engine --------------------------------------------------------- */

static int intercept_nothing(const X86pCpu *cpu, void *user, void *run_user) {
  (void)cpu;
  (void)user;
  (void)run_user;
  return 0;
}

static int no_boundary(uint32_t eip, void *user) {
  (void)eip;
  (void)user;
  return 0;
}

static uint32_t stop_at(void *run_user) {
  return *(const uint32_t *)run_user;
}

/* A chaining engine, which is the only kind that calls leaves. */
static X86pJitEngine *leaf_engine(X86pMem *mem) {
  char reason[256] = {0};
  X86pJitEngine *engine = x86p_jit_engine_create(mem, 1u << 20, 512u, reason, sizeof reason);
  check(engine != NULL, reason);
  if (!engine) {
    return NULL;
  }
  x86p_jit_engine_set_intercept(engine, intercept_nothing, NULL);
  x86p_jit_engine_set_boundary(engine, no_boundary, NULL);
  check(x86p_jit_engine_set_run_stop(engine, stop_at, reason, sizeof reason), reason);
  check(x86p_jit_engine_set_leaves(engine, resolve, NULL, reason, sizeof reason), reason);
  return engine;
}

/* Run to `spin` and compare with the interpreter's `expect`. */
static void run_to_spin(X86pJitEngine *engine, const X86pCpu *expect, uint32_t spin, uint64_t budget) {
  uint32_t no_stop = 0xFFFFFFF0u;
  char reason[256] = {0};
  X86pCpu cpu;
  seed(&cpu);
  check(x86p_jit_engine_run(engine, &cpu, &no_stop, budget, reason, sizeof reason) == kX86pRunBudget, reason);
  check(cpu.eip == kGuestBase + spin, "the engine did not end at the program's spin");
  check(x86p_cpu_diff(expect, &cpu, report_diff, NULL) == 0u, "the engine's state differs from the interpreter's");
}

/*
 * A direct CALL:
 *
 *   0: B9 0A 00 00 00    MOV ECX, 10
 *   5: E8 <0x100>        CALL callee
 *  10: 49                DEC ECX
 *  11: 75 F8             JNZ 5
 *  13: EB FE             JMP 13
 */
static void direct_program(void) {
  static const uint8_t prog[] = {0xB9, 0x0A, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x49, 0x75, 0xF8, 0xEB, 0xFE};
  memset(guest, 0x90, sizeof guest);
  memcpy(guest, prog, sizeof prog);
  rel32(5u, 1u, 0x100u);
  callee(0u, 0x100u, 3u);
  callee(1u, 0x110u, 5u);
}

static void direct_call(int decline) {
  X86pMem mem = guest_mem();
  X86pJitEngineStats st;
  X86pCpu expect;
  current = decline ? "direct CALL, declined" : "direct CALL";
  direct_program();
  interpret(&expect, 13u);
  check(expect.reg[kX86pEax] == 0x1000u + 30u, "the program's callee ran the wrong number of times");
  X86pJitEngine *engine = leaf_engine(&mem);
  if (!engine) {
    return;
  }
  memset(&record, 0, sizeof record);
  record.decline = decline;
  record.returns_to = 10u;
  run_to_spin(engine, &expect, 13u, 400u);
  x86p_jit_engine_stats(engine, &st);
  check(record.wrong == 0u, "a leaf was not entered as its callee would be");
  /* The CALL is in two translations, the block at 0 and the loop's at 5,
     and the leaf is asked on every pass, declined or not. */
  check(st.leaf_calls == 2u, "the direct CALLs were not translated to call their leaf");
  check(record.calls == 10u, "the leaf was not called once per CALL");
  printf("%s: %u leaf call(s), %llu block(s) entered\n", current, record.calls, (unsigned long long)st.blocks_entered);
  x86p_jit_engine_destroy(engine);
}

/*
 * A CALL through a register:
 *
 *   0: B9 0A 00 00 00    MOV ECX, 10
 *   5: BA <A>            MOV EDX, callee A
 *  10: FF D2             CALL EDX
 *  12: 81 F2 <k>         XOR EDX, k        ; k = A ^ B alternates, 0 does not
 *  18: 49                DEC ECX
 *  19: 75 F5             JNZ 10
 *  21: EB FE             JMP 21
 */
static void site_program(int alternate) {
  static const uint8_t prog[] = {0xB9, 0x0A, 0, 0, 0, 0xBA, 0,    0,    0,    0,    0xFF, 0xD2,
                                 0x81, 0xF2, 0, 0, 0, 0,    0x49, 0x75, 0xF5, 0xEB, 0xFE};
  memset(guest, 0x90, sizeof guest);
  memcpy(guest, prog, sizeof prog);
  callee(0u, 0x100u, 3u);
  callee(1u, 0x110u, 5u);
  put32(6u, kGuestBase + 0x100u);
  put32(14u, alternate ? (0x100u ^ 0x110u) : 0u);
}

typedef struct SiteExpect {
  unsigned calls;
  uint64_t fills;
  uint64_t exhausted;
} SiteExpect;

static void site_call(int alternate, int decline, SiteExpect want) {
  X86pMem mem = guest_mem();
  X86pJitEngineStats st;
  X86pCpu expect;
  current = alternate ? "CALL EDX, alternating" : decline ? "CALL EDX, declined" : "CALL EDX";
  site_program(alternate);
  interpret(&expect, 21u);
  X86pJitEngine *engine = leaf_engine(&mem);
  if (!engine) {
    return;
  }
  memset(&record, 0, sizeof record);
  record.decline = decline;
  record.returns_to = 12u;
  run_to_spin(engine, &expect, 21u, 600u);
  x86p_jit_engine_stats(engine, &st);
  check(record.wrong == 0u, "a leaf was not entered as its callee would be");
  check(st.leaf_sites_refused == 0u, "a site was refused");
  /* CALL EDX is in two translations, the block at 0 and the loop's at 10,
     each with its own site. */
  check(st.leaf_sites == 2u, "the indirect CALLs were not given sites");
  check(record.calls == want.calls, "the leaves were called the wrong number of times");
  check(st.leaf_site_fills == want.fills, "the sites asked the resolver the wrong number of times");
  check(st.leaf_site_leaf_fills == want.fills, "a fill for a target with a leaf did not name it");
  check(st.leaf_sites_exhausted == want.exhausted, "the wrong number of sites stopped asking");
  printf("%s: %u leaf call(s), %llu fill(s), %llu site(s) exhausted\n",
         current,
         record.calls,
         (unsigned long long)st.leaf_site_fills,
         (unsigned long long)st.leaf_sites_exhausted);
  x86p_jit_engine_destroy(engine);
}

/*
 * The widest CALL a site is emitted for, base + index * 4 + disp32 through
 * memory:
 *
 *   0: BB <base>                MOV EBX, base
 *   5: 31 C9                    XOR ECX, ECX
 *   7: FF 94 8B 00 02 00 00     CALL [EBX + ECX*4 + 0x200]  ; = callee A
 *  14: 31 C9                    XOR ECX, ECX
 *  16: EB FE                    JMP 16
 */
static void widest_call(void) {
  static const uint8_t prog[] = {
      0xBB, 0, 0, 0, 0, 0x31, 0xC9, 0xFF, 0x94, 0x8B, 0x00, 0x02, 0x00, 0x00, 0x31, 0xC9, 0xEB, 0xFE};
  X86pMem mem = guest_mem();
  X86pCpu expect;
  current = "CALL [EBX+ECX*4+disp32]";
  memset(guest, 0x90, sizeof guest);
  memcpy(guest, prog, sizeof prog);
  put32(1u, kGuestBase);
  put32(0x200u, kGuestBase + 0x300u);
  callee(0u, 0x300u, 3u);
  callee(1u, 0x310u, 5u);
  interpret(&expect, 16u);
  X86pJitEngine *engine = leaf_engine(&mem);
  if (!engine) {
    return;
  }
  memset(&record, 0, sizeof record);
  record.returns_to = 14u;
  run_to_spin(engine, &expect, 16u, 50u);
  check(record.wrong == 0u, "a leaf was not entered as its callee would be");
  check(record.calls == 1u, "the widest CALL did not call its leaf");
  x86p_jit_engine_destroy(engine);
}

/*
 * More callers than a compaction batch, so later laps run bodies relowered
 * into shared modules: each must call its leaf through the site it was
 * published with, and stay chained while it does.
 *
 * Caller k at k*16: MOV EDX, callee; CALL EDX; JMP caller k+1, the last back
 * to the first. Callee A: ADD EAX, 3; RET.
 */
enum { kCallers = 100u, kCallerStride = 16u, kCallee = kCallers * kCallerStride };

static void ring_program(void) {
  memset(guest, 0x90, sizeof guest);
  for (unsigned k = 0; k < kCallers; k++) {
    const unsigned at = k * kCallerStride;
    guest[at] = 0xBA;
    put32(at + 1u, kGuestBase + kCallee);
    guest[at + 5u] = 0xFF;
    guest[at + 6u] = 0xD2;
    guest[at + 7u] = 0xE9;
    rel32(at + 7u, 1u, (k + 1u) % kCallers * kCallerStride);
  }
  callee(0u, kCallee, 3u);
  callee(1u, kCallee + 0x10u, 5u);
}

static uint64_t chained(X86pJitEngine *engine, X86pCpu *cpu, uint64_t steps) {
  uint32_t no_stop = 0xFFFFFFF0u;
  char reason[256] = {0};
  X86pJitEngineStats before;
  X86pJitEngineStats after;
  x86p_jit_engine_stats(engine, &before);
  check(x86p_jit_engine_run(engine, cpu, &no_stop, steps, reason, sizeof reason) == kX86pRunBudget, reason);
  x86p_jit_engine_stats(engine, &after);
  return after.blocks_chained - before.blocks_chained;
}

static void relowered_sites(void) {
  X86pMem mem = guest_mem();
  X86pJitEngineStats st;
  X86pCpu cpu;
  current = "relowered sites";
  ring_program();
  X86pJitEngine *engine = leaf_engine(&mem);
  if (!engine) {
    return;
  }
  memset(&record, 0, sizeof record);
  seed(&cpu);
  (void)chained(engine, &cpu, 4u * 2u * kCallers);
  const unsigned cold = record.calls;
  const uint64_t warm = chained(engine, &cpu, 20000u);
  x86p_jit_engine_stats(engine, &st);
  check(record.wrong == 0u, "a leaf was not entered as its callee would be");
  /* Two blocks per caller -- its CALL and the JMP it returns to -- and a leaf
     call in every other one. */
  check(record.calls - cold == 10000u, "a warm lap did not call the leaf once per caller");
  check(cpu.reg[kX86pEax] == 0x1000u + 3u * record.calls, "a leaf's work was lost or repeated");
  check(cpu.reg[kX86pEsp] == kGuestBase + kStack, "the leaves unbalanced the stack");
  /* 200 blocks, so at least six batches were rebuilt, every one accepted. */
  check(st.compactions >= 6u, "no batch of callers was relowered");
  check(st.compaction_refusals == 0u, "a relowered caller did not lower as it was published");
  check(st.leaf_sites == kCallers, "each caller's CALL was not given one site");
  /* One fill per site, ever: a relowered body that claimed a fresh site would
     fill it again. */
  check(st.leaf_site_fills == kCallers, "a relowered CALL asked the resolver again");
  check(st.leaf_sites_refused == 0u, "a site was refused");
  /* One dispatch, and every other entry a transfer: the leaf never sent the
     run back to the dispatcher. */
  check(warm >= 20000u - 1u, "a call through a relowered site returned to the dispatcher");
  printf("%s: %llu of 20000 entries chained, %llu compaction(s), %llu fill(s) for %llu site(s)\n",
         current,
         (unsigned long long)warm,
         (unsigned long long)st.compactions,
         (unsigned long long)st.leaf_site_fills,
         (unsigned long long)st.leaf_sites);
  x86p_jit_engine_destroy(engine);
}

static void every_case(void) {
  direct_call(0);
  direct_call(1);
  /* One fill per site; every call takes the leaf. */
  site_call(0, 0, (SiteExpect){10u, 2u, 0u});
  /* A declined call changes nothing and reaches the guest callee. */
  site_call(0, 1, (SiteExpect){10u, 2u, 0u});
  /* The loop's site sees B, A, B, A and stops asking: after that only A, its
     last target, takes the leaf, and B runs as an ordinary CALL. The first
     call (A) is the block at 0's; 1 + 4 + 2 leaf calls in all. */
  site_call(1, 0, (SiteExpect){7u, 5u, 1u});
  widest_call();
  relowered_sites();
}

int main(void) {
  every_case();
  with_perms = 1;
  printf("-- with a page permission table --\n");
  every_case();
  /* Held, not used: everything the engines allocate from here on lies above
     it, where a signed LEB128 constant needs five bytes. */
  void *const ballast = malloc(256u << 20);
  check(ballast != NULL && (uintptr_t)ballast + (256u << 20) > (1u << 27), "no room to move the engine's data up");
  printf("-- with the engine's data above 128 MiB --\n");
  every_case();
  free(ballast);
  printf("%u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
