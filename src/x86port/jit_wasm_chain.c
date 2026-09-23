/* jit_wasm_chain.c -- see jit_wasm_chain.h. */
#include "jit_wasm_chain.h"

#include "jit_wasm_module.h"

#include <string.h>

#define ALIGN_NONE 0u

static const uint32_t kRunBudget = (uint32_t)offsetof(X86pJitChainRun, budget);
static const uint32_t kRunStop = (uint32_t)offsetof(X86pJitChainRun, stop);
static const uint32_t kRunLast = (uint32_t)offsetof(X86pJitChainRun, last);
static const uint32_t kRunPending = (uint32_t)offsetof(X86pJitChainRun, pending);

void x86p_wasm_chain_exits_init(X86pWasmChainExits *c, const X86pWasmChainUse *use) {
  memset(c, 0, sizeof *c);
  c->use.reuse_first = -1;
  c->first = -1;
  if (use) {
    c->use = *use;
  }
}

size_t x86p_wasm_chain_reserve(const X86pWasmChainExits *c) {
  if (!c->use.chain || c->slotted >= X86P_WASM_CHAIN_SLOTS) {
    return 0u;
  }
  return (size_t)(X86P_WASM_CHAIN_SLOTS - c->slotted) * X86P_WASM_CHAIN_EXIT_BYTES;
}

static int64_t take_slot(X86pWasmChainExits *c) {
  if (c->slotted >= X86P_WASM_CHAIN_SLOTS) {
    return -1;
  }
  if (c->use.reuse_first >= 0) {
    return c->slotted < c->use.reuse_count ? c->use.reuse_first + (int64_t)c->slotted : -1;
  }
  return x86p_jit_chain_claim(c->use.chain);
}

static void push_target(X86pWasmEmit *e, uint32_t imm, int local) {
  if (local >= 0) {
    x86p_wasm_local_get(e, (uint32_t)local);
  } else {
    x86p_wasm_i32_const(e, (int32_t)imm);
  }
}

static void push_base(X86pWasmEmit *e, uint32_t base) {
  x86p_wasm_i32_const(e, (int32_t)base);
}

void x86p_wasm_chain_emit(X86pWasmChainExits *c, X86pWasmEmit *e, uint32_t imm, int local) {
  if (!c->use.chain) {
    return;
  }
  const int64_t slot = take_slot(c);
  if (slot < 0) {
    c->unslotted++;
    return;
  }
  const uintptr_t chain_base = x86p_jit_chain_base(c->use.chain);
  if (chain_base > UINT32_MAX) {
    /* Emitted code names the run header by a 32-bit linear address, which a
       chain allocated on a 64-bit host does not have. Refuse the block. */
    e->overflow = 1;
    return;
  }
  const uint32_t base = (uint32_t)chain_base;
  const uint32_t disp = (uint32_t)x86p_jit_chain_slot_disp(c->use.chain, slot);
  if (c->first < 0) {
    c->first = slot;
  }
  c->slotted++;

  x86p_wasm_block(e, kWasmVoid);
  /* slot.guest == target, zero-extended: an unlinked slot never matches. */
  push_base(e, base);
  x86p_wasm_i64_load(e, ALIGN_NONE, disp + (uint32_t)offsetof(X86pJitChainSlot, guest));
  if (local >= 0) {
    x86p_wasm_local_get(e, (uint32_t)local);
    x86p_wasm_i64_extend_i32_u(e);
  } else {
    x86p_wasm_i64_const(e, (int64_t)imm);
  }
  x86p_wasm_i64_eq(e);
  x86p_wasm_i32_op(e, kWasmI32Eqz);
  x86p_wasm_br_if(e, 0u);
  /* Not the run's stop address, which only the dispatcher enters. */
  push_target(e, imm, local);
  push_base(e, base);
  x86p_wasm_i32_load(e, ALIGN_NONE, kRunStop);
  x86p_wasm_i32_op(e, kWasmI32Eq);
  x86p_wasm_br_if(e, 0u);
  /* --budget, and leave when it reaches zero. */
  push_base(e, base);
  push_base(e, base);
  x86p_wasm_i64_load(e, ALIGN_NONE, kRunBudget);
  x86p_wasm_i64_const(e, -1);
  x86p_wasm_i64_add(e);
  x86p_wasm_local_tee(e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_store(e, ALIGN_NONE, kRunBudget);
  x86p_wasm_local_get(e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(e, 0);
  x86p_wasm_i64_eq(e);
  x86p_wasm_br_if(e, 0u);
  /* The run's last entry, then the transfer: its answer is this block's. */
  push_base(e, base);
  push_target(e, imm, local);
  x86p_wasm_i32_store(e, ALIGN_NONE, kRunLast);
  x86p_wasm_local_get(e, (uint32_t)kX86pWasmLocalCpu);
  push_base(e, base);
  x86p_wasm_i32_load(e, ALIGN_NONE, disp + (uint32_t)offsetof(X86pJitChainSlot, host));
  x86p_wasm_call(e, (uint32_t)kX86pWasmImportChainCall);
  x86p_wasm_return(e);
  x86p_wasm_end(e);
  /* Missed: name the slot for the dispatcher to link. */
  push_base(e, base);
  x86p_wasm_i32_const(e, (int32_t)(slot + 1));
  x86p_wasm_i32_store(e, ALIGN_NONE, kRunPending);
}

uint32_t x86p_wasm_chain_call(X86pCpu *cpu, uint32_t host) {
  /* On the wasm host a function pointer IS a table index (jit_wasm.c says why
     the conversion goes through a pointer-sized integer). */
  uint32_t (*fn)(X86pCpu *);
  *(void **)&fn = (void *)(uintptr_t)host;
  return fn(cpu);
}
