/* See jit_profile.h. */
#include "jit_profile.h"

#include <stdlib.h>
#include <string.h>

/* A guest EIP the target never executes at: the table's empty-slot sentinel.
   The guest address space this port runs is a 32-bit ring-3 image; the last
   dword of it is not a valid instruction start. */
#define PROF_EMPTY 0xFFFFFFFFu

struct X86pJitProfile {
  uint32_t *keys;    /* PROF_EMPTY == free */
  uint64_t *counts;  /* parallel to keys */
  uint32_t mask;     /* capacity - 1; capacity is a power of two */
  uint32_t capacity; /* max distinct keys before drops begin (== cap/2) */
  uint32_t distinct; /* keys currently held */
  uint64_t total;    /* every hit, dropped or not */
  uint64_t dropped;  /* hits whose key could not be inserted */
};

static uint32_t mix32(uint32_t x) {
  /* Fibonacci hashing then a shift: block addresses are 16-byte-ish aligned
     runs, so the low bits carry little and the raw value clusters. */
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

X86pJitProfile *x86p_jit_profile_create(uint32_t slot_hint) {
  X86pJitProfile *p;
  uint32_t cap;
  if (slot_hint < 64u) {
    slot_hint = 64u;
  }
  cap = round_up_pow2(slot_hint * 2u);

  p = (X86pJitProfile *)calloc(1u, sizeof *p);
  if (!p) {
    return NULL;
  }
  p->keys = (uint32_t *)malloc((size_t)cap * sizeof *p->keys);
  p->counts = (uint64_t *)calloc((size_t)cap, sizeof *p->counts);
  if (!p->keys || !p->counts) {
    free(p->keys);
    free(p->counts);
    free(p);
    return NULL;
  }
  memset(p->keys, 0xFF, (size_t)cap * sizeof *p->keys); /* all PROF_EMPTY */
  p->mask = cap - 1u;
  p->capacity = cap / 2u;
  return p;
}

void x86p_jit_profile_destroy(X86pJitProfile *p) {
  if (!p) {
    return;
  }
  free(p->keys);
  free(p->counts);
  free(p);
}

void x86p_jit_profile_hit(X86pJitProfile *p, uint32_t guest_eip) {
  uint32_t i;
  if (!p) {
    return;
  }
  p->total++;
  if (guest_eip == PROF_EMPTY) {
    p->dropped++; /* cannot represent the sentinel value itself */
    return;
  }
  i = mix32(guest_eip) & p->mask;
  for (;;) {
    uint32_t k = p->keys[i];
    if (k == guest_eip) {
      if (p->counts[i] != UINT64_MAX) {
        p->counts[i]++;
      }
      return;
    }
    if (k == PROF_EMPTY) {
      if (p->distinct >= p->capacity) {
        p->dropped++;
        return;
      }
      p->keys[i] = guest_eip;
      p->counts[i] = 1u;
      p->distinct++;
      return;
    }
    i = (i + 1u) & p->mask;
  }
}

static int entry_desc(const void *a, const void *b) {
  const X86pJitProfileEntry *x = (const X86pJitProfileEntry *)a;
  const X86pJitProfileEntry *y = (const X86pJitProfileEntry *)b;
  if (x->entries < y->entries) {
    return 1;
  }
  if (x->entries > y->entries) {
    return -1;
  }
  if (x->guest_eip < y->guest_eip) {
    return -1;
  }
  return x->guest_eip > y->guest_eip;
}

uint32_t x86p_jit_profile_top(const X86pJitProfile *p, X86pJitProfileEntry *out, uint32_t n) {
  X86pJitProfileEntry *all;
  uint32_t i, m = 0u, want;
  if (!p || !out || n == 0u) {
    return 0u;
  }
  if (p->distinct == 0u) {
    return 0u;
  }

  all = (X86pJitProfileEntry *)malloc((size_t)p->distinct * sizeof *all);
  if (!all) {
    return 0u;
  }
  for (i = 0u; i <= p->mask; i++) {
    if (p->keys[i] == PROF_EMPTY) {
      continue;
    }
    all[m].guest_eip = p->keys[i];
    all[m].entries = p->counts[i];
    m++;
  }
  qsort(all, m, sizeof *all, entry_desc);
  want = n < m ? n : m;
  memcpy(out, all, (size_t)want * sizeof *out);
  free(all);
  return want;
}

uint64_t x86p_jit_profile_total_hits(const X86pJitProfile *p) {
  return p ? p->total : 0u;
}
uint32_t x86p_jit_profile_distinct(const X86pJitProfile *p) {
  return p ? p->distinct : 0u;
}
uint64_t x86p_jit_profile_dropped_keys(const X86pJitProfile *p) {
  return p ? p->dropped : 0u;
}
