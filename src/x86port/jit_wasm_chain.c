/* jit_wasm_chain.c -- see jit_wasm_chain.h. */
#include "jit_wasm_chain.h"

#include "block_cache.h"
#include "jit_wasm_module.h"

#include <string.h>

#define ALIGN_NONE 0u

static const uint32_t kRunBudget = (uint32_t)offsetof(X86pJitChainRun, budget);
static const uint32_t kRunStop = (uint32_t)offsetof(X86pJitChainRun, stop);
static const uint32_t kRunLast = (uint32_t)offsetof(X86pJitChainRun, last);
static const uint32_t kRunPending = (uint32_t)offsetof(X86pJitChainRun, pending);

_Static_assert(sizeof(JcBlockFront) == 16u, "the probe scales the front index by a shift of 4");

void x86p_wasm_chain_exits_init(X86pWasmChainExits *c, const X86pWasmChainUse *use, uint32_t entry) {
  memset(c, 0, sizeof *c);
  c->use.reuse_first = -1;
  c->first = -1;
  c->entry = entry;
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

/* A 32-bit linear address, or 0 with the emitter refused: emitted code names
   the run header and the front array by one, which memory allocated on a
   64-bit host does not have. */
static uint32_t linear(X86pWasmEmit *e, uintptr_t address) {
  if (address > UINT32_MAX) {
    e->overflow = 1;
    return 0u;
  }
  return (uint32_t)address;
}

static void push_target(X86pWasmEmit *e, uint32_t imm, int local) {
  if (local >= 0) {
    x86p_wasm_local_get(e, (uint32_t)local);
  } else {
    x86p_wasm_i32_const(e, (int32_t)imm);
  }
}

/* The target, zero-extended, against the i64 guest address on the stack:
   leaves nonzero when they differ. */
static void guest_differs(X86pWasmEmit *e, uint32_t imm, int local) {
  if (local >= 0) {
    x86p_wasm_local_get(e, (uint32_t)local);
    x86p_wasm_i64_extend_i32_u(e);
  } else {
    x86p_wasm_i64_const(e, (int64_t)imm);
  }
  x86p_wasm_i64_eq(e);
  x86p_wasm_i32_op(e, kWasmI32Eqz);
}

/* Where a transfer finds the table index it calls. */
typedef enum ChainHost {
  kChainHostSlot,  /* the slot at `disp` past the run header */
  kChainHostProbed /* the front entry's host, left in kX86pWasmLocalAddr */
} ChainHost;

/*
 * Inside a block whose branch 0 leaves: the run's stop address and budget,
 * which every transfer honours, then the transfer, whose answer this block
 * returns. A probe's exit had already named its slot, so a probe clears it.
 */
static void emit_transfer(X86pWasmEmit *e, uint32_t base, uint32_t imm, int local, ChainHost host, uint32_t disp) {
  /* Not the run's stop address, which only the dispatcher enters. */
  push_target(e, imm, local);
  x86p_wasm_i32_const(e, (int32_t)base);
  x86p_wasm_i32_load(e, ALIGN_NONE, kRunStop);
  x86p_wasm_i32_op(e, kWasmI32Eq);
  x86p_wasm_br_if(e, 0u);
  /* --budget, and leave when it reaches zero. */
  x86p_wasm_i32_const(e, (int32_t)base);
  x86p_wasm_i32_const(e, (int32_t)base);
  x86p_wasm_i64_load(e, ALIGN_NONE, kRunBudget);
  x86p_wasm_i64_const(e, -1);
  x86p_wasm_i64_add(e);
  x86p_wasm_local_tee(e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_store(e, ALIGN_NONE, kRunBudget);
  x86p_wasm_local_get(e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(e, 0);
  x86p_wasm_i64_eq(e);
  x86p_wasm_br_if(e, 0u);
  x86p_wasm_i32_const(e, (int32_t)base);
  push_target(e, imm, local);
  x86p_wasm_i32_store(e, ALIGN_NONE, kRunLast);
  if (host == kChainHostProbed) {
    x86p_wasm_i32_const(e, (int32_t)base);
    x86p_wasm_i32_const(e, 0);
    x86p_wasm_i32_store(e, ALIGN_NONE, kRunPending);
  }
  x86p_wasm_local_get(e, (uint32_t)kX86pWasmLocalCpu);
  if (host == kChainHostProbed) {
    x86p_wasm_local_get(e, (uint32_t)kX86pWasmLocalAddr);
  } else {
    x86p_wasm_i32_const(e, (int32_t)base);
    x86p_wasm_i32_load(e, ALIGN_NONE, disp + (uint32_t)offsetof(X86pJitChainSlot, host));
  }
  x86p_wasm_return_call(e, (uint32_t)kX86pWasmImportChainCall);
}

/*
 * THE PROBE (jit_chain.h), for the computed EIP in `local` that missed its
 * slot: the front entry for that address, when it holds it with a host --
 * never the block that exited, whose re-entry the dispatcher counts.
 */
static void emit_probe(X86pWasmChainExits *c, X86pWasmEmit *e, uint32_t base, int local) {
  const uint32_t front = linear(e, (uintptr_t)x86p_jit_chain_front(c->use.chain));
  if (front == 0u) {
    return;
  }
  x86p_wasm_block(e, kWasmVoid);
  x86p_wasm_local_get(e, (uint32_t)local);
  x86p_wasm_i32_const(e, (int32_t)c->entry);
  x86p_wasm_i32_op(e, kWasmI32Eq);
  x86p_wasm_br_if(e, 0u);
  /* front + ((target >> SHIFT) & (SLOTS - 1)) * 16 */
  x86p_wasm_i32_const(e, (int32_t)front);
  x86p_wasm_local_get(e, (uint32_t)local);
  x86p_wasm_i32_const(e, (int32_t)JC_BLOCK_FRONT_SHIFT);
  x86p_wasm_i32_op(e, kWasmI32ShrU);
  x86p_wasm_i32_const(e, (int32_t)(JC_BLOCK_FRONT_SLOTS - 1u));
  x86p_wasm_i32_op(e, kWasmI32And);
  x86p_wasm_i32_const(e, 4);
  x86p_wasm_i32_op(e, kWasmI32Shl);
  x86p_wasm_i32_op(e, kWasmI32Add);
  x86p_wasm_local_tee(e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_i64_load(e, ALIGN_NONE, (uint32_t)offsetof(JcBlockFront, guest));
  guest_differs(e, 0u, local);
  x86p_wasm_br_if(e, 0u);
  /* A NULL host marks an address the cache refuses. */
  x86p_wasm_local_get(e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_i32_load(e, ALIGN_NONE, (uint32_t)offsetof(JcBlockFront, host));
  x86p_wasm_local_tee(e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_i32_op(e, kWasmI32Eqz);
  x86p_wasm_br_if(e, 0u);
  emit_transfer(e, base, 0u, local, kChainHostProbed, 0u);
  x86p_wasm_end(e);
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
  const uint32_t base = linear(e, x86p_jit_chain_base(c->use.chain));
  if (base == 0u) {
    return;
  }
  const uint32_t disp = (uint32_t)x86p_jit_chain_slot_disp(c->use.chain, slot);
  const size_t start = x86p_wasm_here(e);
  if (c->first < 0) {
    c->first = slot;
  }
  c->slotted++;

  x86p_wasm_block(e, kWasmVoid);
  /* slot.guest == target: an unlinked slot never matches. */
  x86p_wasm_i32_const(e, (int32_t)base);
  x86p_wasm_i64_load(e, ALIGN_NONE, disp + (uint32_t)offsetof(X86pJitChainSlot, guest));
  guest_differs(e, imm, local);
  x86p_wasm_br_if(e, 0u);
  emit_transfer(e, base, imm, local, kChainHostSlot, disp);
  x86p_wasm_end(e);
  /* Missed: name the slot for the dispatcher to link. */
  x86p_wasm_i32_const(e, (int32_t)base);
  x86p_wasm_i32_const(e, (int32_t)(slot + 1));
  x86p_wasm_i32_store(e, ALIGN_NONE, kRunPending);
  if (local >= 0) {
    emit_probe(c, e, base, local);
  }
  const size_t emitted = x86p_wasm_here(e) - start;
  c->bytes += emitted;
  if (emitted > X86P_WASM_CHAIN_EXIT_BYTES && c->oversized == 0u) {
    c->oversized = emitted;
  }
}

size_t x86p_wasm_chain_trampoline(void *buf, size_t cap) {
  static const X86pWasmType kParams[2] = {kWasmI32, kWasmI32};
  static const X86pWasmType kResult[1] = {kWasmI32};
  X86pWasmEmit e;
  X86pWasmSize section;
  X86pWasmSize body;
  x86p_wasm_init(&e, buf, cap);
  x86p_wasm_module_begin(&e);
  /* The one type is a block's signature (jit_wasm_module.h). */
  section = x86p_wasm_section_begin(&e, kWasmSectionType);
  x86p_wasm_u32(&e, 1u);
  x86p_wasm_functype(&e, kParams, 2u, kResult, 1u);
  x86p_wasm_size_end(&e, section);
  section = x86p_wasm_section_begin(&e, kWasmSectionImport);
  x86p_wasm_u32(&e, 1u);
  x86p_wasm_import_table(&e, X86P_WASM_MEMORY_MODULE, X86P_WASM_CHAIN_TABLE_FIELD, 0u, 0, 0u);
  x86p_wasm_size_end(&e, section);
  section = x86p_wasm_section_begin(&e, kWasmSectionFunction);
  x86p_wasm_u32(&e, 1u);
  x86p_wasm_u32(&e, 0u);
  x86p_wasm_size_end(&e, section);
  section = x86p_wasm_section_begin(&e, kWasmSectionExport);
  x86p_wasm_u32(&e, 1u);
  x86p_wasm_export_func(&e, X86P_WASM_CHAIN_TRAMPOLINE_EXPORT, 0u);
  x86p_wasm_size_end(&e, section);
  section = x86p_wasm_section_begin(&e, kWasmSectionCode);
  x86p_wasm_u32(&e, 1u);
  x86p_wasm_body_begin(&e, &body);
  x86p_wasm_locals(&e, 0u);
  /* The block's two words: the cpu, and the index again, which it ignores. */
  x86p_wasm_local_get(&e, 0u);
  x86p_wasm_local_get(&e, 1u);
  x86p_wasm_local_get(&e, 1u);
  x86p_wasm_return_call_indirect(&e, 0u);
  x86p_wasm_body_end(&e, body);
  x86p_wasm_size_end(&e, section);
  return x86p_wasm_ok(&e) ? e.len : 0u;
}
