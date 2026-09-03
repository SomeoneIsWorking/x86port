/*
 * decode_cache.h -- decoded instructions, kept between executions of the same
 * address.
 *
 * WHY THIS EXISTS. Decoding is the interpreter's single largest cost: measured
 * on pc/xmen2's attract loop, Zydis accounted for 35% of all cycles, against
 * 12% for the code that actually executes the instruction. A guest loop decodes
 * the same bytes on every iteration and gets the same answer every time, so the
 * work is repeated rather than necessary.
 *
 * WHY IT IS EXACT, NOT AN ASSUMPTION. An entry is only used when the guest
 * bytes at that address still MATCH the ones that were decoded, compared on
 * every hit. Self-modifying code, an overlay loaded over a cached page, a JIT
 * inside the guest -- each changes the bytes, the compare fails, and the entry
 * is decoded again. There is no invalidation protocol to get wrong, and no way
 * for a stale entry to execute: the validation is the same information the
 * decoder would have read.
 *
 * The cache is CALLER-OWNED state, not a global. One per executing thread
 * keeps it lock-free; sharing one between threads would need a lock, and the
 * hit is a memcmp of a few bytes, so the lock would cost more than the decode
 * it saves.
 */
#ifndef X86PORT_DECODE_CACHE_H
#define X86PORT_DECODE_CACHE_H

#include "decode.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direct-mapped, a power of two. 16384 entries is ~2 MB and covers the working
   set of a game frame with room to spare; direct-mapped rather than associative
   because a miss costs one decode, which is exactly what the uncached path
   costs, so a conflict is a lost saving and never a fault. */
#define X86P_DECODE_CACHE_SLOTS 16384u

typedef struct X86pDecodeCacheEntry {
  uint32_t eip;
  uint8_t valid;
  uint8_t length; /* bytes compared on a hit */
  uint8_t bytes[X86P_MAX_INSN_LEN];
  X86pInsn insn;
} X86pDecodeCacheEntry;

typedef struct X86pDecodeCache {
  X86pDecodeCacheEntry slot[X86P_DECODE_CACHE_SLOTS];
  /* Every lookup lands in exactly one of these four, so they sum to the
     denominator and a report can never imply work it did not do. */
  unsigned long long hits;
  unsigned long long cold;       /* slot empty */
  unsigned long long collisions; /* slot held a different address */
  unsigned long long rewritten;  /* same address, the bytes had changed */
} X86pDecodeCache;

/* Decode `bytes` (`avail` of them, the fetch window at `eip`), answering from
   the cache when it holds this address with these exact bytes. Returns what
   x86p_decode returns. A NULL cache decodes every time -- the uncached path,
   kept so a caller can turn the cache off and compare. */
int x86p_decode_cached(X86pDecodeCache *cache, uint32_t eip, const uint8_t *bytes, uint32_t avail, X86pInsn *out);

/* Drop every entry. Not needed for correctness -- an entry validates itself --
   but a caller that has remapped the whole address space can reclaim the slots
   rather than paying a compare to discover each one is stale. */
void x86p_decode_cache_reset(X86pDecodeCache *cache);

/* Lookups and the hit share, as a percentage of them. Prints as a denominator
   with the count, so "the cache did nothing" reads differently from "the cache
   was never asked". */
unsigned long long x86p_decode_cache_lookups(const X86pDecodeCache *cache);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_DECODE_CACHE_H */
