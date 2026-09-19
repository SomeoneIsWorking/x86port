/* See jit_chain_census.h. */
#include "jit_chain_census.h"

#include <stdlib.h>
#include <string.h>

/* A guest EIP the target never executes at: the empty-slot sentinel, for the
   same reason jit_profile.c uses it. */
#define CHAIN_EMPTY 0xFFFFFFFFu

typedef struct ChainSlot {
  uint32_t entry;
  uint32_t targets[X86P_JIT_CHAIN_TARGETS];
  unsigned count;
} ChainSlot;

struct X86pJitChainCensus {
  ChainSlot *slots; /* entry == CHAIN_EMPTY means free */
  uint32_t mask;
  uint32_t capacity; /* distinct blocks before drops begin */
  uint32_t blocks;
  uint64_t entries;
  uint64_t chainable;
  uint64_t unrecorded;
  uint64_t overflowed;
  uint64_t dropped;
};

static uint32_t mix32(uint32_t x) {
  x *= 0x9E3779B9u;
  return x ^ (x >> 15);
}

static uint32_t round_up_pow2(uint32_t v) {
  uint32_t p = 1u;
  while (p < v && p < 0x40000000u) {
    p <<= 1;
  }
  return p;
}

X86pJitChainCensus *x86p_jit_chain_census_create(uint32_t slot_hint) {
  X86pJitChainCensus *c;
  uint32_t cap;
  if (slot_hint < 64u) {
    slot_hint = 64u;
  }
  cap = round_up_pow2(slot_hint * 2u);
  c = (X86pJitChainCensus *)calloc(1u, sizeof *c);
  if (!c) {
    return NULL;
  }
  c->slots = (ChainSlot *)malloc((size_t)cap * sizeof *c->slots);
  if (!c->slots) {
    free(c);
    return NULL;
  }
  memset(c->slots, 0xFF, (size_t)cap * sizeof *c->slots); /* entry == CHAIN_EMPTY */
  c->mask = cap - 1u;
  c->capacity = cap / 2u;
  return c;
}

void x86p_jit_chain_census_destroy(X86pJitChainCensus *c) {
  if (!c) {
    return;
  }
  free(c->slots);
  free(c);
}

/* The slot holding `entry`, or NULL. */
static ChainSlot *find(const X86pJitChainCensus *c, uint32_t entry) {
  uint32_t i = mix32(entry) & c->mask;
  for (;;) {
    ChainSlot *s = &c->slots[i];
    if (s->entry == entry) {
      return s;
    }
    if (s->entry == CHAIN_EMPTY) {
      return NULL;
    }
    i = (i + 1u) & c->mask;
  }
}

void x86p_jit_chain_census_note_block(
    X86pJitChainCensus *c, uint32_t entry, const uint32_t *targets, unsigned count, unsigned overflowed) {
  uint32_t i;
  ChainSlot *slot = NULL;
  if (!c || entry == CHAIN_EMPTY) {
    return;
  }
  c->overflowed += overflowed;
  i = mix32(entry) & c->mask;
  for (;;) {
    ChainSlot *s = &c->slots[i];
    if (s->entry == entry) {
      slot = s;
      break;
    }
    if (s->entry == CHAIN_EMPTY) {
      if (c->blocks >= c->capacity) {
        c->dropped++;
        return;
      }
      s->entry = entry;
      c->blocks++;
      slot = s;
      break;
    }
    i = (i + 1u) & c->mask;
  }
  /* A retranslation replaces the record rather than merging with it: the old
     block's successors are not this block's. */
  slot->count = 0u;
  for (i = 0u; i < count && i < X86P_JIT_CHAIN_TARGETS; i++) {
    slot->targets[i] = targets[i];
    slot->count++;
  }
}

void x86p_jit_chain_census_note_entry(X86pJitChainCensus *c, uint32_t previous, uint32_t entry, int have_previous) {
  const ChainSlot *slot;
  unsigned i;
  if (!c) {
    return;
  }
  c->entries++;
  if (!have_previous) {
    c->unrecorded++;
    return;
  }
  slot = find(c, previous);
  if (!slot || slot->count == 0u) {
    /* Either the predecessor was dropped, or its backend records no
       successors at all. Neither is evidence that the entry was unchainable,
       and folding them into `chainable`'s complement would say it was. */
    c->unrecorded++;
    return;
  }
  for (i = 0u; i < slot->count; i++) {
    if (slot->targets[i] == entry) {
      c->chainable++;
      return;
    }
  }
}

uint64_t x86p_jit_chain_census_entries(const X86pJitChainCensus *c) {
  return c ? c->entries : 0u;
}
uint64_t x86p_jit_chain_census_chainable(const X86pJitChainCensus *c) {
  return c ? c->chainable : 0u;
}
uint64_t x86p_jit_chain_census_unrecorded(const X86pJitChainCensus *c) {
  return c ? c->unrecorded : 0u;
}
uint64_t x86p_jit_chain_census_overflowed(const X86pJitChainCensus *c) {
  return c ? c->overflowed : 0u;
}
uint32_t x86p_jit_chain_census_blocks(const X86pJitChainCensus *c) {
  return c ? c->blocks : 0u;
}
uint64_t x86p_jit_chain_census_dropped_keys(const X86pJitChainCensus *c) {
  return c ? c->dropped : 0u;
}
