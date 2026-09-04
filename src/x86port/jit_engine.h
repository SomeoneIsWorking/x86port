/*
 * jit_engine.h -- the dispatch loop: what makes the translator an ENGINE.
 *
 * jit_x64.c translates one basic block and hands it back. Nothing in that file
 * runs a program: it does not decide what to translate next, does not remember
 * what it has already translated, and does not know what to do with an
 * instruction it cannot emit. This does all three, and it is the smallest piece
 * that can execute a guest program end to end.
 *
 * THERE IS NO FALLBACK. A block that stops on an instruction the backend has
 * no emitter for leaves guest EIP on that instruction and returns a named
 * unsupported status. The separately built test oracle is not linked here.
 *
 * WHAT IT COUNTS, AND WHY EVERY COUNTER HAS A DENOMINATOR. "The JIT is working"
 * is not observable from a program that finished. A run that translated
 * nothing must not look successful, so refusals and translated/entered blocks
 * are reported explicitly.
 */
#ifndef X86PORT_JIT_ENGINE_H
#define X86PORT_JIT_ENGINE_H

#include "cpu.h"
#include "jit_profile.h"
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
  kX86pRunUnsupported,  /* the product JIT has no route for the instruction */
  kX86pRunFetchFault,   /* EIP itself is not in mapped memory */
  kX86pRunMemoryFault,  /* a guest access was refused; cpu->eip is on it */
  kX86pRunDivideError,  /* #DE: the guest must receive this */
  /* Traps and faults the guest actually took. Distinct from Unsupported. */
  kX86pRunInterrupt,
  kX86pRunProtectionFault,
  kX86pRunBoundRange,
  kX86pRunTranslateFailed, /* the block at this EIP could not be translated */
  kX86pRunOutOfCode,       /* the code region filled and a flush did not help */
  kX86pRunIntercept,       /* intercepted by consumer: thunk, override, setjmp, return */
  kX86pRunStatusCount      /* MUST stay last: the denominator */
} X86pJitRunStatus;

const char *x86p_jit_run_status_name(X86pJitRunStatus s);

/*
 * What the run actually did.
 *
 * A RETIRED-INSTRUCTION TOTAL IS DELIBERATELY ABSENT. A translated block does
 * not report how many instructions it ran, and the block cache records the
 * guest BYTES it spans rather than the count. Summing translation-time counts
 * would double-count every re-entered block, and charging one per block entry
 * would be a number that looks like an instruction count and is not. So the
 * translated/entered/refusal counts are what is published, and the budget is
 * counted in the same units it is charged in.
 */
typedef struct X86pJitEngineStats {
  uint64_t blocks_entered;
  uint64_t blocks_translated;
  uint64_t guest_insns_translated; /* summed at TRANSLATION, where it is known */
  uint64_t translate_refusals;     /* translations that hit an unmodelled entry */
  uint64_t cache_flushes;
  uint64_t code_bytes_used;
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
 * STEP is one block entry or one native interception dispatch, so the budget
 * is charged in the units the loop actually measures.
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

/*
 * An interception predicate called before a block is looked up or executed.
 * Returns non-zero if the current EIP must not be executed by the JIT (e.g. it
 * is a host thunk, native override, setjmp frame, or return sentinel).
 *
 * When this returns non-zero, x86p_jit_engine_run immediately returns
 * kX86pRunIntercept with cpu->eip untouched.
 */
typedef int (*X86pJitInterceptFn)(const X86pCpu *cpu, void *user);

void x86p_jit_engine_set_intercept(X86pJitEngine *e, X86pJitInterceptFn fn, void *user);

/*
 * What the inline dispatch handler did with an interception point.
 */
typedef enum X86pJitDispatchResult {
  /* The handler executed the intercepted operation by mutating cpu (eip, regs,
     esp) and the run must continue from the new cpu->eip -- no unwind. */
  kX86pDispatchContinue = 0,
  /* The handler wants x86p_jit_engine_run to return kX86pRunIntercept, exactly
     as if no dispatch handler were installed. For the cases that genuinely
     need the caller's host frame back: a guest setjmp/longjmp, control
     reaching the caller's own return address. */
  kX86pDispatchUnwind,
} X86pJitDispatchResult;

/*
 * Called from INSIDE the run loop when the intercept predicate fires, instead
 * of unwinding the run with kX86pRunIntercept. The handler runs the host thunk
 * or native override the address stands for -- reading and writing cpu
 * directly -- then returns kX86pDispatchContinue so the same run resumes at
 * the updated cpu->eip. Each dispatched intercept counts as one step against
 * the run's budget, so an override that fails to advance eip still terminates
 * the slice rather than spinning.
 *
 * Without a handler installed (the default), every interception point unwinds
 * the run: correct, but at ~20k host-API calls per game frame the teardown and
 * re-entry of x86p_jit_engine_run dominates. Null clears it.
 */
typedef X86pJitDispatchResult (*X86pJitDispatchFn)(X86pCpu *cpu, void *user);

void x86p_jit_engine_set_dispatch(X86pJitEngine *e, X86pJitDispatchFn fn, void *user);

/*
 * A predicate consulted DURING translation: it must return non-zero for any
 * guest address the consumer's intercept handler would take over, so a block is
 * never translated past one. The intercept predicate above runs only between
 * blocks; without this, an interception point reached by fall-through inside a
 * straight-line run is translated over and the original guest bytes execute
 * there. Branch targets are already block leaders, so this matters only for the
 * fall-through case. Null clears it.
 */
void x86p_jit_engine_set_boundary(X86pJitEngine *e, X86pJitBoundaryFn fn, void *user);

/*
 * Turn the block cache off (enabled != 0 turns it back on; it is on by default).
 *
 * With the cache off every block is retranslated on entry and the code region
 * is rewound after it runs, so nothing is ever executed from a translation made
 * before the current guest bytes. This is a DIAGNOSTIC, not a mode to ship:
 * it is how a consumer distinguishes "the JIT translates this block wrongly"
 * from "the JIT is running a stale translation the guest has since overwritten"
 * without yet having wired write-notified invalidation.
 */
void x86p_jit_engine_set_cache(X86pJitEngine *e, int enabled);

/*
 * Attach or remove an execution-weighted block-entry histogram (off by
 * default). With it on, every block entry bumps a saturating counter keyed by
 * the block's guest address -- the number x86p_jit_engine_stats cannot give,
 * because there a block entered once and a block entered ten million times
 * weigh the same. `slot_hint` sizes the table for that many distinct block
 * addresses before it starts dropping new keys (and counting the drops).
 *
 * This is the profiler for "where does a running guest spend its time":
 * x86p_jit_profile_top over x86p_jit_engine_profile() names the blocks whose
 * emitters are worth improving first.
 * Cheap enough to
 * leave on for a session (one masked load + compare on the hot path), still a
 * diagnostic and not a shipping default.
 */
int x86p_jit_engine_set_profile(X86pJitEngine *e, int enabled, uint32_t slot_hint, char *reason, unsigned reason_len);

/* The attached profile, or NULL. Borrowed -- the engine owns it; valid until
   the next set_profile call or engine destruction. */
const X86pJitProfile *x86p_jit_engine_profile(const X86pJitEngine *e);

/* Which code-memory mechanism this build resolved. "It worked on my machine"
   and "it worked through the same mechanism as the user's machine" are
   different claims. */
const char *x86p_jit_engine_mechanism(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_ENGINE_H */
