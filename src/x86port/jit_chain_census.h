/*
 * jit_chain_census.h -- did the run go where the translator already knew it
 * could go?
 *
 * The static census (X86pWasmExitCensus) says what fraction of a block's exits
 * name a constant address, and blocks_reentered says how often a block was
 * re-entered from itself. Neither answers the question that decides whether
 * general block chaining is worth building: of the dispatches actually paid,
 * how many went to an address the block just left had already emitted as a
 * constant? A chaining backend removes exactly those and no others.
 *
 * So this keeps, per translated block, the immediate successor addresses that
 * block's emitted code contains, and compares each entry against the previous
 * block's set. It is a fixed-capacity open-addressed table for the same reason
 * the profile is: one masked index and a compare on the hot path, drops
 * counted rather than hidden, and off unless a consumer opts in.
 *
 * WHAT A ZERO MEANS. `chainable` counts entries whose address was in the
 * previous block's recorded set. An entry is `unrecorded` when the previous
 * block is not in the table -- it was dropped, or the backend recorded no
 * successors for it, which is what every machine-code backend does today. The
 * two are reported separately because a run against a backend that records
 * nothing would otherwise read as a run where nothing is chainable.
 */
#ifndef X86PORT_JIT_CHAIN_CENSUS_H
#define X86PORT_JIT_CHAIN_CENSUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The most successor addresses kept per block. A block's exits are its
   conditional branches' taken paths plus its terminator; past this the extra
   ones are counted as overflow rather than silently forgotten. */
#define X86P_JIT_CHAIN_TARGETS 6u

typedef struct X86pJitChainCensus X86pJitChainCensus;

/* `slot_hint` distinct blocks without a drop; the table is a power of two at
   twice that, so the load factor stays at 0.5. NULL on allocation failure. */
X86pJitChainCensus *x86p_jit_chain_census_create(uint32_t slot_hint);
void x86p_jit_chain_census_destroy(X86pJitChainCensus *c);

/* Record the immediate successors the block at `entry` emitted. Replaces any
   previous record for that address, because a retranslation supersedes it. */
void x86p_jit_chain_census_note_block(
    X86pJitChainCensus *c, uint32_t entry, const uint32_t *targets, unsigned count, unsigned overflowed);

/* One dispatch from `previous` to `entry`. `previous` is ignored on the very
   first entry of a run, which has no predecessor; pass have_previous = 0. */
void x86p_jit_chain_census_note_entry(X86pJitChainCensus *c, uint32_t previous, uint32_t entry, int have_previous);

uint64_t x86p_jit_chain_census_entries(const X86pJitChainCensus *c);
uint64_t x86p_jit_chain_census_chainable(const X86pJitChainCensus *c);
uint64_t x86p_jit_chain_census_unrecorded(const X86pJitChainCensus *c);
uint64_t x86p_jit_chain_census_overflowed(const X86pJitChainCensus *c);
uint32_t x86p_jit_chain_census_blocks(const X86pJitChainCensus *c);
uint64_t x86p_jit_chain_census_dropped_keys(const X86pJitChainCensus *c);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_JIT_CHAIN_CENSUS_H */
