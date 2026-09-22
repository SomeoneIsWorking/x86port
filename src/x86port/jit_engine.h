/*
 * jit_engine.h -- the dispatch loop: what makes the translator an ENGINE.
 *
 * jit_x64.c translates one basic block and hands it back. Nothing in that file
 * runs a program: it does not decide what to translate next, does not remember
 * what it has already translated, and does not know what to do with an
 * instruction it cannot emit. This does all three, and it is the smallest piece
 * that can execute a guest program end to end.
 *
 * THIS DIRECT ENGINE HAS NO FALLBACK EDGE. A block that stops on an
 * instruction the backend has no emitter for leaves guest EIP on that
 * instruction and returns a named unsupported status. A future product
 * dispatcher may compose the separately linked bounded fallback described in
 * AGENTS.md; the separately built interpreter-only diagnostic is not linked
 * here.
 *
 * WHAT IT COUNTS, AND WHY EVERY COUNTER HAS A DENOMINATOR. "The JIT is working"
 * is not observable from a program that finished. A run that translated
 * nothing must not look successful, so refusals and translated/entered blocks
 * are reported explicitly.
 */
#ifndef X86PORT_JIT_ENGINE_H
#define X86PORT_JIT_ENGINE_H

#include "cpu.h"
#include "jit_chain_census.h"
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
/*
 * EVERY FIELD IS A uint64_t RUNNING TOTAL, and x86p_jit_engine_stats_add sums
 * this struct as an array of them so a field added later cannot be silently
 * left out of a pool's total. The cost of that is that a field which is not a
 * summable total -- an address, a flag, a most-recent sample -- would be added
 * to its neighbours and produce a number that looks like a measurement.
 * Anything of that shape belongs on the engine behind its own accessor, as
 * x86p_jit_engine_last_block_entry is.
 */
typedef struct X86pJitEngineStats {
  uint64_t blocks_entered;
  /*
   * Of those, the ones that re-entered the block just left: a guest loop going
   * round again, having paid a full dispatch to do it.
   *
   * COUNTED PER ENTRY, WHICH IS THE POINT. Every other loop figure here is
   * summed at translation, so a loop that runs a million times weighs the same
   * as one that never runs -- useless for sizing a fix whose whole value is in
   * iterations. This is the share of real dispatches a backend lowering a
   * self-exit as a WebAssembly `loop` would remove, and it can report a low
   * number as readily as a high one.
   */
  uint64_t blocks_reentered;
  uint64_t blocks_translated;
  uint64_t guest_insns_translated; /* summed at TRANSLATION, where it is known */
  /* Jcc and SETcc emitted, and how many of those the backend lowered to the
     host's own condition codes instead of a call to x86p_cond. Summed at
     translation for the same reason as the instruction count, so these are a
     property of the CODE the run translated, not of how often it ran. A
     backend that lowers none is correct and merely slower, which is why the
     total is published beside the inline count rather than alone. */
  uint64_t conds_translated;
  uint64_t conds_inline;
  /* Of the rest, those whose flag-writing predecessor was never recorded. See
     X86pJitBlock::cond_unknown_kind: the remainder, conds_translated minus
     conds_inline minus this, is the count that a new derivation would win. */
  uint64_t conds_unknown_kind;
  /* Memory-operand x87 loads translated, and those whose widening the emitted
     code performs itself. See X86pJitBlock::x87_loads: a backend with no
     inline form reports the denominator and a zero, rather than nothing. */
  uint64_t x87_loads_translated;
  uint64_t x87_loads_inline;
  /* Memory-operand x87 stores translated, and those the emitted code narrows
     itself. Same contract again; the store's inline arm rounds, so its share
     is a property of the VALUES the route stores as well as of the code. */
  uint64_t x87_stores_translated;
  uint64_t x87_stores_inline;
  /* SIMD instructions translated, and those the emitted code performs with the
     host's own 128-bit SIMD. Same contract: the denominator is published so a
     zero numerator can be told apart from a corpus with no SIMD in it. */
  uint64_t simd_translated;
  uint64_t simd_inline;

  /*
   * The exit census, over the blocks this engine translated. X86pWasmExitCensus
   * defines each; these are the same four counts summed as blocks are cached.
   *
   * THIS POPULATION IS THE ONE THAT DECIDES BLOCK CHAINING, and it is not the
   * one an offline corpus walk produces. Where a block starts is decided by
   * whoever cut it: a tool that walks each function linearly from its first
   * byte puts a loop head in the middle of a block, while this engine
   * translates from the address it was asked to dispatch to -- so a loop head
   * that is branched to becomes a block ENTRY here and its backedge is an
   * exits_self. The offline number is a floor for this one, not an estimate of
   * it.
   *
   * Filled by the WebAssembly backend only; a consumer reporting them must say
   * so rather than print a zero that reads like a census.
   */
  uint64_t exits;
  uint64_t exits_static;
  uint64_t exits_backward;
  uint64_t exits_self;

  uint64_t translate_refusals; /* translations that hit an unmodelled entry */
  /* Invalidation, asked and achieved. `invalidations` counts calls to
     x86p_jit_engine_invalidate and `invalidation_bytes` the guest range they
     named; `invalidation_blocks_dropped` counts the cached blocks those calls
     actually forgot. The pair is the discriminator: calls without drops are an
     embedder notifying about memory that held no code, and drops without a
     matching climb in blocks_translated mean the dropped code was cold. */
  uint64_t invalidations;
  uint64_t invalidation_bytes;
  uint64_t invalidation_blocks_dropped;
  /* The engine reclaiming its own code region, which is a different fact and
     is never counted above. An eviction says the arena is too small for the
     working set: every block it drops is one the run is about to translate
     again. `cache_flushes` below counts the whole-arena reset that happens
     when even evicting a victim will not free room. */
  uint64_t evictions;
  uint64_t eviction_blocks_dropped;
  uint64_t cache_flushes;
  uint64_t code_bytes_used;
  /*
   * Batches of singly-published blocks this storage has rebuilt as ONE module,
   * and batches it left alone.
   *
   * On a WebAssembly host every published module is a permanent engine object
   * and the engine has a limit on those -- measured in Firefox 156, refusal at
   * about 16,350 live modules, which a real title reaches in seconds. Blocks
   * are published one to a module so they can run the moment they translate,
   * and then rebuilt in batches so the count stays far below that. A run whose
   * compactions stay at zero is holding one module per translated block, and
   * from the outside it looks exactly like a run that is compacting perfectly;
   * these two numbers are the only difference. Both are zero on hosts that
   * write machine code, where there is no per-block engine object to reduce.
   */
  uint64_t compactions;
  uint64_t compaction_refusals;
  /* Blocks waiting for a batch to fill, and 1 per engine that has stopped
     compacting for good, so a compaction count that stops rising can be told
     apart from batches that stopped filling. */
  uint64_t compaction_pending;
  uint64_t compaction_stopped;
  /* The limits the code arena was created with, so `code_bytes_used` has a
     denominator and an eviction says WHICH limit it hit. Summed across a pool,
     these are the pool's total. */
  uint64_t code_bytes_limit;
  uint64_t block_records;
  /* Evictions by the limit that asked for them. A run that keeps evicting
     while its budget and its slots are both far from full was refused by the
     third one, and only the split says so. */
  uint64_t evictions_out_of_bytes;
  uint64_t evictions_out_of_slots;
  uint64_t evictions_at_engine_limit;
  /* Live-module ceilings the WebAssembly engine's refusals put in force, and
     how many were retired again after backing off below them. */
  uint64_t ceilings_learned;
  uint64_t ceilings_retired;
  /* The block cache's own counters (jit-common JcBlockStats): lookups, the
     hits its direct-mapped front cache answered, and the table slots probed
     for the rest. A front cache too small for the run's hot set shows here as
     a front-hit share far below the hit rate, and nowhere else. */
  /* Calls to the intercept predicate, beside blocks_entered: under the
     intercept contract (x86p_jit_engine_set_run_stop) the predicate is asked
     only on misses, at guarded blocks and at the run's stop address, and this
     is how a reader tells a contract that works from one never installed.
     blocks_guarded is how many translations were marked to be asked about. */
  uint64_t intercept_calls;
  uint64_t blocks_guarded;
  uint64_t cache_lookups;
  uint64_t cache_hits;
  uint64_t cache_front_hits;
  uint64_t cache_table_probes;
  /* Lookups that found a GUARDED block and refused it to the dispatcher's fast
     path; each is followed by the intercept call and a second lookup. */
  uint64_t cache_guarded;
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
 *
 * `run_user` is the caller's state for THIS run, handed unchanged to the
 * intercept and dispatch callbacks beside their registered user pointer. It
 * exists because those callbacks almost always need the consumer's current
 * call frame, which is per-thread: without it a consumer must reach that frame
 * through a thread-local, and the run loop consults the intercept once per
 * block boundary. On a host whose thread-locals are emulated -- an Android
 * shared object below API 29 -- that lookup is a function call through a
 * pthread key, and it measured 13.4% of the port library's samples in X-Men 2.
 * Living on this run's own stack, it is per-thread by construction and cannot
 * be raced by a second guest thread the way a registered pointer can. Null
 * when the consumer has no such state.
 */
X86pJitRunStatus x86p_jit_engine_run(
    X86pJitEngine *e, X86pCpu *cpu, void *run_user, uint64_t max_steps, char *reason, unsigned reason_len);

/* Forget translations overlapping [lo, hi) -- self-modifying code, an overlay
   load, DMA into code memory. */
void x86p_jit_engine_invalidate(X86pJitEngine *e, uint32_t lo, uint32_t hi);

/* Drop every translation while preserving configuration and callbacks. Use
   between block entries, including mutations ending at the top of guest space. */
int x86p_jit_engine_invalidate_all(X86pJitEngine *e, char *reason, unsigned reason_len);

void x86p_jit_engine_stats(const X86pJitEngine *e, X86pJitEngineStats *out);

/*
 * Add `item` into `sum`, field by field, for an embedder that runs an engine
 * per guest thread and wants one total. It lives here because the struct does:
 * a consumer that writes its own summation loop silently reports zero for
 * every field added afterwards, which is how an invalidation counter would
 * arrive already broken.
 */
void x86p_jit_engine_stats_add(X86pJitEngineStats *sum, const X86pJitEngineStats *item);

/*
 * An interception predicate called before a block is looked up or executed.
 * Returns non-zero if the current EIP must not be executed by the JIT (e.g. it
 * is a host thunk, native override, setjmp frame, or return sentinel).
 *
 * When this returns non-zero, x86p_jit_engine_run immediately returns
 * kX86pRunIntercept with cpu->eip untouched.
 */
typedef int (*X86pJitInterceptFn)(const X86pCpu *cpu, void *user, void *run_user);

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
typedef X86pJitDispatchResult (*X86pJitDispatchFn)(X86pCpu *cpu, void *user, void *run_user);

void x86p_jit_engine_set_dispatch(X86pJitEngine *e, X86pJitDispatchFn fn, void *user);

/*
 * THE INTERCEPT CONTRACT. Without it the intercept predicate is asked before
 * every block, which on a title's gameplay is an indirect call out of the
 * framework per block entered -- a few percent of all cycles spent hearing
 * "no". Installing a stop function declares that the predicate can return
 * non-zero ONLY:
 *
 *   - at an address the boundary predicate reports, or
 *   - at the address `fn(run_user)` returns for the current run -- the one
 *     address whose interception depends on run-time state, such as the
 *     return address that ends a call the consumer made into guest code.
 *
 * The engine then asks the predicate only on a block-cache miss, before a
 * block translated at a boundary address (such blocks are cached GUARDED), and
 * at the stop address. `fn` is called once per x86p_jit_engine_run.
 *
 * Refused (returns 0, with `reason`) when an intercept predicate is installed
 * without a boundary predicate, and when blocks are already cached: those
 * were never checked against the boundary predicate, so nothing marks the
 * ones the contract must still ask about. Null clears it.
 */
typedef uint32_t (*X86pJitRunStopFn)(void *run_user);

int x86p_jit_engine_set_run_stop(X86pJitEngine *e, X86pJitRunStopFn fn, char *reason, unsigned reason_len);

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

/*
 * The runtime chain census: of the dispatches actually paid, how many went to
 * an address the block just left had already emitted as a constant. That is
 * the population general block chaining removes, and neither the static exit
 * counts nor blocks_reentered can say how big it is. Off by default; the
 * census belongs to the engine and lives until the next call or destruction.
 */
int x86p_jit_engine_set_chain_census(
    X86pJitEngine *e, int enabled, uint32_t slot_hint, char *reason, unsigned reason_len);
const X86pJitChainCensus *x86p_jit_engine_chain_census(const X86pJitEngine *e);

/*
 * Report the first few entries to ONE guest address, with the block entered
 * before it and the live register file.
 *
 * "Which block is hot" is answered by the profile; "how did the run first get
 * HERE, and in what state" is not, and that is the question a wedge leaves
 * behind. Measured: a title's boot spun on a two-byte `JMP $` its own code
 * contains, reached by any of three paths -- two conditional branches and the
 * fall-through from a call that was not supposed to return -- and nothing
 * could say which.
 *
 * `previous` is the block entered immediately before, across dispatch calls;
 * `have_previous` is 0 for the first block of the run, where there is no
 * previous block and 0 would be a lie. `cpu` is live: the callback may read
 * it and must not resume execution through it.
 *
 * `reports` bounds how many entries are reported, because a watched address
 * may be entered twenty million times a second. Passing 0, or a NULL
 * callback, turns the watch off. Costs one compare against a counter on the
 * hot path when off.
 */
typedef void (*X86pJitEntryWatchFn)(void *user, uint32_t addr, uint32_t previous, int have_previous, X86pCpu *cpu);
void x86p_jit_engine_set_entry_watch(
    X86pJitEngine *e, uint32_t guest_addr, uint64_t reports, X86pJitEntryWatchFn fn, void *user);

/* The attached profile, or NULL. Borrowed -- the engine owns it; valid until
   the next set_profile call or engine destruction. */
/*
 * The guest address of the MOST RECENT block entry this engine made.
 *
 * blocks_reentered says a run is going round a block and cannot say which one,
 * and the block-entry histogram cannot answer it either once its table is
 * full: a spin that begins after the table fills has its key refused, so the
 * histogram ranks whatever was early. Measured on the API 35 x86_64 emulator,
 * a wedged run reported 1,761,605,419 entries of which 1,761,478,604 were
 * dropped and a top entry with 11,630 hits -- 0.0%.
 *
 * This is already tracked on the hot path for blocks_reentered, so reading it
 * costs nothing. IT IS A SAMPLE OF ONE: it says nothing about where a healthy
 * run spends its time, and it is not summable across a pool, which is why it
 * is not in X86pJitEngineStats. It is the address to aim
 * x86p_jit_engine_set_entry_watch at when a run has stopped making progress.
 * Zero when the engine has entered no block.
 */
uint32_t x86p_jit_engine_last_block_entry(const X86pJitEngine *e);

const X86pJitProfile *x86p_jit_engine_profile(const X86pJitEngine *e);

/* Which code-memory mechanism this build resolved. "It worked on my machine"
   and "it worked through the same mechanism as the user's machine" are
   different claims. */
const char *x86p_jit_engine_mechanism(void);
/* What the last refused module gathering in THIS engine said, or "" when none
   was refused. It is a string rather than a counter, so it is asked for by
   engine instead of summed with the others. */
const char *x86p_jit_engine_compaction_refusal_reason(const X86pJitEngine *e);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_ENGINE_H */
