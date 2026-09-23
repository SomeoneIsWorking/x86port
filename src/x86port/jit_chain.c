/* jit_chain.c -- see jit_chain.h. */
#include "jit_chain.h"

#include <stdlib.h>
#include <string.h>

/*
 * The run header and the slots are one allocation, header first, so emitted
 * code holds ONE address and reaches both by displacement.
 */
struct X86pJitChain {
  X86pJitChainRun run;
  const struct JcBlockFront *front;
  size_t capacity;
  size_t claimed;
  X86pJitChainSlot slots[];
};

/* Emitted displacements are 32-bit. */
#define MAX_SLOTS ((size_t)(INT32_MAX / (int32_t)sizeof(X86pJitChainSlot)) - 64u)

static void unlink_slots(X86pJitChain *chain, size_t count) {
  size_t i;
  for (i = 0; i < count; i++) {
    chain->slots[i].guest = X86P_JIT_CHAIN_UNLINKED;
    chain->slots[i].host = NULL;
  }
  chain->run.pending = 0u;
}

X86pJitChain *x86p_jit_chain_create(size_t slots) {
  X86pJitChain *chain;
  if (slots == 0u || slots > MAX_SLOTS) {
    return NULL;
  }
  chain = (X86pJitChain *)calloc(1u, sizeof *chain + slots * sizeof(X86pJitChainSlot));
  if (!chain) {
    return NULL;
  }
  chain->capacity = slots;
  unlink_slots(chain, slots);
  return chain;
}

void x86p_jit_chain_destroy(X86pJitChain *chain) {
  free(chain);
}

X86pJitChainRun *x86p_jit_chain_run(X86pJitChain *chain) {
  return &chain->run;
}

int64_t x86p_jit_chain_claim(X86pJitChain *chain) {
  if (chain->claimed >= chain->capacity) {
    return -1;
  }
  chain->slots[chain->claimed].guest = X86P_JIT_CHAIN_UNLINKED;
  chain->slots[chain->claimed].host = NULL;
  return (int64_t)chain->claimed++;
}

void x86p_jit_chain_set_front(X86pJitChain *chain, const struct JcBlockFront *front) {
  chain->front = front;
}

const struct JcBlockFront *x86p_jit_chain_front(const X86pJitChain *chain) {
  return chain->front;
}

size_t x86p_jit_chain_claimed(const X86pJitChain *chain) {
  return chain->claimed;
}

void x86p_jit_chain_rewind(X86pJitChain *chain, size_t claimed) {
  if (claimed < chain->claimed) {
    chain->claimed = claimed;
  }
}

size_t x86p_jit_chain_capacity(const X86pJitChain *chain) {
  return chain->capacity;
}

uintptr_t x86p_jit_chain_base(const X86pJitChain *chain) {
  return (uintptr_t)chain;
}

int32_t x86p_jit_chain_slot_disp(const X86pJitChain *chain, int64_t slot) {
  (void)chain;
  return (int32_t)(offsetof(X86pJitChain, slots) + (size_t)slot * sizeof(X86pJitChainSlot));
}

void x86p_jit_chain_link(X86pJitChain *chain, int64_t slot, uint32_t guest, void *host) {
  chain->slots[slot].host = host;
  chain->slots[slot].guest = guest;
}

const X86pJitChainSlot *x86p_jit_chain_slot(const X86pJitChain *chain, int64_t slot) {
  return &chain->slots[slot];
}

void x86p_jit_chain_unlink_all(X86pJitChain *chain) {
  unlink_slots(chain, chain->claimed);
}

void x86p_jit_chain_reset(X86pJitChain *chain) {
  unlink_slots(chain, chain->claimed);
  chain->claimed = 0u;
}
