/*
 * jit_engine.h -- the dispatch loop: what makes the translator an ENGINE.
 *
 * jit_x64.c translates one basic block and hands it back. Nothing in that file
 * runs a program: it does not decide what to translate next, does not remember
 * what it has already translated, and does not know what to do with an
 * instruction it cannot emit. This does all three, and it is the smallest piece
 * that can execute a guest program end to end.
 *
 * THE INTERPRETER IS THE FALLBACK, NOT A SECOND ENGINE. A block that stops on
 * an instruction the backend has no emitter for exits with the guest EIP
 * pointing AT that instruction; the loop then steps the interpreter exactly
 * once and goes back to translating. So coverage is a performance property
 * rather than a correctness one: an unmodelled instruction costs a fallback
 * step, never a wrong answer and never a refusal to run. That is the whole
 * reason this migration can proceed one emitter at a time.
 *
 * WHAT IT COUNTS, AND WHY EVERY COUNTER HAS A DENOMINATOR. "The JIT is working"
 * is not observable from a program that finished. A run that translated nothing
 * and interpreted every instruction finishes with exactly the same guest state
 * as one that translated everything, so the only way to tell them apart is to
 * count both and report the ratio. x86p_jit_engine_stats is therefore not
 * optional instrumentation; it is the only evidence that this file did
 * anything.
 */
#ifndef X86PORT_JIT_ENGINE_H
#define X86PORT_JIT_ENGINE_H

#include "cpu.h"
#include "jit_x64.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct X86pJitEngine X86pJitEngine;

/* Why a run stopped. Every one of these is a REPORTED outcome; none of them is
   a state the caller has to infer from the CPU. */
typedef enum X86pJitRunStatus {
  kX86pRunBudget = 0,   /* ran the requested number of instructions, no more */
  kX86pRunDecodeFailed, /* the bytes at EIP are not an instruction */
  kX86pRunUnsupported,  /* named, but neither backend nor interpreter has it */
  kX86pRunFetchFault,   /* EIP itself is not in mapped memory */
  kX86pRunMemoryFault,  /* a guest access was refused; cpu->eip is on it */
  kX86pRunDivideError,  /* #DE: the guest must receive this */
  /* Traps and faults the guest actually took. Distinct from Unsupported: the
     instruction is modelled and this is its outcome. See exec.h. */
  kX86pRunInterrupt,
  kX86pRunProtectionFault,
  kX86pRunBoundRange,
  kX86pRunTranslateFailed, /* the block at this EIP could not be translated */
  kX86pRunOutOfCode,       /* the code region filled and a flush did not help */
  kX86pRunStatusCount      /* MUST stay last: the denominator */
} X86pJitRunStatus;

const char *x86p_jit_run_status_name(X86pJitRunStatus s);

/*
 * What the run actually did.
 *
 * `blocks_entered` against `fallback_steps` is the number that says whether
 * this engine is doing its job: the first is guest instructions running as
 * translated code, the second is guest instructions the interpreter had to run
 * one at a time.
 *
 * A RETIRED-INSTRUCTION TOTAL IS DELIBERATELY ABSENT. A translated block does
 * not report how many instructions it ran, and the block cache records the
 * guest BYTES it spans rather than the count. Summing translation-time counts
 * would double-count every re-entered block, and charging one per block entry
 * would be a number that looks like an instruction count and is not. So the
 * honest pair above is what is published, and the budget is counted in the same
 * units it is charged in.
 */
typedef struct X86pJitEngineStats {
  uint64_t blocks_entered;
  uint64_t blocks_translated;
  uint64_t guest_insns_translated; /* summed at TRANSLATION, where it is known */
  uint64_t fallback_steps;         /* guest instructions the interpreter ran */
  /*
   * Of those, the ones taken straight from a block's unsupported EXIT rather
   * than after a translation refused the instruction at a block entry. Both
   * routes reach the interpreter and both are correct, so the split is not
   * cosmetic: it is the only way to see whether the fast route is being taken
   * at all. Removing it leaves the loop CORRECT and quietly paying a wasted
   * translation attempt for every unmodelled instruction in every hot loop --
   * a defect no state comparison can see.
   */
  uint64_t fallback_after_block;
  uint64_t translate_refusals; /* translations that hit an unmodelled entry */
  uint64_t cache_flushes;
  uint64_t code_bytes_used;
  /*
   * Guest instructions the blocks execute by CALLING the interpreter rather
   * than as emitted host code.
   *
   * The honest counterweight to the coverage number. Routing every instruction
   * through the helper would translate 100% of a program and produce a
   * threaded interpreter; guest state cannot tell the difference, and only
   * this ratio can. Counted at translation, so it measures the code produced
   * rather than how often it ran.
   */
  uint64_t guest_insns_via_helper;
} X86pJitEngineStats;

/*
 * Create an engine over `mem`. `code_bytes` sizes the code region and
 * `cache_blocks` the block cache; both are refused rather than clamped when the
 * host will not provide them, with `reason` naming what failed.
 *
 * The engine holds `mem` by pointer and does not copy it: the mapping must
 * outlive the engine, and a change to it invalidates translated code, which is
 * what x86p_jit_engine_invalidate is for.
 */
X86pJitEngine *
x86p_jit_engine_create(const X86pMem *mem, size_t code_bytes, size_t cache_blocks, char *reason, unsigned reason_len);
void x86p_jit_engine_destroy(X86pJitEngine *e);

/*
 * Run until `max_steps` steps have been taken, or something stops the run. A
 * STEP is one block entry or one interpreter instruction -- the two things this
 * loop can do -- so the budget is charged in the units the loop actually
 * measures rather than in an instruction count it would have to invent.
 *
 * The consequence, stated because it will otherwise surprise someone: as
 * coverage improves a given budget covers MORE guest instructions, since more
 * of them fit inside a block. A caller that needs a bound on guest work should
 * read the stats, not assume a step is an instruction.
 */
X86pJitRunStatus
x86p_jit_engine_run(X86pJitEngine *e, X86pCpu *cpu, uint64_t max_steps, char *reason, unsigned reason_len);

/* Forget translations overlapping [lo, hi) -- self-modifying code, an overlay
   load, DMA into code memory. */
void x86p_jit_engine_invalidate(X86pJitEngine *e, uint32_t lo, uint32_t hi);

void x86p_jit_engine_stats(const X86pJitEngine *e, X86pJitEngineStats *out);

/* Which code-memory mechanism this build resolved. "It worked on my machine"
   and "it worked through the same mechanism as the user's machine" are
   different claims. */
const char *x86p_jit_engine_mechanism(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_ENGINE_H */
