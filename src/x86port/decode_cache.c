#include "decode_cache.h"

#include <string.h>

static unsigned slot_of(uint32_t eip) {
  /* Fibonacci hashing: consecutive instruction addresses differ by 1-15 bytes,
     so the low bits alone would pack a hot loop into a handful of slots while
     leaving most of the table empty. */
  return (unsigned)((eip * 2654435761u) >> 15) & (X86P_DECODE_CACHE_SLOTS - 1u);
}

int x86p_decode_cached(X86pDecodeCache *cache, uint32_t eip, const uint8_t *bytes, uint32_t avail, X86pInsn *out) {
  X86pDecodeCacheEntry *e;
  int ok;

  if (!cache) {
    return x86p_decode(bytes, avail, out);
  }
  e = &cache->slot[slot_of(eip)];
  if (!e->valid) {
    cache->cold++;
  } else if (e->eip != eip) {
    cache->collisions++;
  } else if (e->length > avail || memcmp(e->bytes, bytes, e->length) != 0) {
    /* The address is cached and the bytes there are NOT what was decoded: the
       guest rewrote its own code, or something was mapped over it. */
    cache->rewritten++;
  } else {
    cache->hits++;
    *out = e->insn;
    return 1;
  }

  ok = x86p_decode(bytes, avail, out);
  if (!ok) {
    /* A failed decode is not cached. It carries no length to validate against,
       so an entry for it could never be checked, only trusted. */
    e->valid = 0;
    return 0;
  }
  e->eip = eip;
  e->length = out->length <= X86P_MAX_INSN_LEN ? (uint8_t)out->length : (uint8_t)X86P_MAX_INSN_LEN;
  memcpy(e->bytes, bytes, e->length);
  e->insn = *out;
  e->valid = 1;
  return 1;
}

void x86p_decode_cache_reset(X86pDecodeCache *cache) {
  if (cache) {
    memset(cache, 0, sizeof *cache);
  }
}

unsigned long long x86p_decode_cache_lookups(const X86pDecodeCache *cache) {
  if (!cache) {
    return 0u;
  }
  return cache->hits + cache->cold + cache->collisions + cache->rewritten;
}
