/* See jit_wasm_x87_slot.h. */
#include "jit_wasm_x87_slot.h"

#include "jit_wasm_internal.h"

#if X86P_X87_BINARY128

enum {
  kTagOffset = (int)(offsetof(X86pCpu, x87) + offsetof(X86pX87, tag)),
  kTopOffset = (int)(offsetof(X86pCpu, x87) + offsetof(X86pX87, top)),
  kRegSize = (int)sizeof(X86pX87Reg)
};

/* Alignment hints are zero throughout this backend; jit_wasm_state.c explains
   why a promise the guest does not make must not be emitted. */
#define ALIGN_NONE 0u

static void cpu(X86pWasmLower *l) {
  x86p_wasm_state_cpu(&l->state);
}

static void constant(X86pWasmLower *l, int32_t value) {
  x86p_wasm_i32_const(l->e, value);
}

/* (index + delta) & 7, from the index on the stack. */
static void wrap(X86pWasmLower *l, int delta) {
  constant(l, delta);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, X86P_X87_REGS - 1);
  x86p_wasm_i32_op(l->e, kWasmI32And);
}

void x86p_wasm_x87_slot_index(X86pWasmLower *l, int delta, int local) {
  cpu(l);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
  wrap(l, delta);
  x86p_wasm_local_tee(l->e, (uint32_t)local);
  cpu(l);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTagOffset);
  constant(l, (int32_t)kX86pX87TagEmpty);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
}

void x86p_wasm_x87_slot_register(X86pWasmLower *l, int local) {
  cpu(l);
  x86p_wasm_local_get(l->e, (uint32_t)local);
  constant(l, kRegSize);
  x86p_wasm_i32_op(l->e, kWasmI32Mul);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
}

void x86p_wasm_x87_slot_set_tag(X86pWasmLower *l, int local, X86pX87Tag tag) {
  cpu(l);
  x86p_wasm_local_get(l->e, (uint32_t)local);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, (int32_t)tag);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTagOffset);
}

void x86p_wasm_x87_slot_set_top(X86pWasmLower *l, int local) {
  cpu(l);
  x86p_wasm_local_get(l->e, (uint32_t)local);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
}

void x86p_wasm_x87_slot_retire(X86pWasmLower *l, int local) {
  x86p_wasm_x87_slot_set_tag(l, local, kX86pX87TagEmpty);
  cpu(l);
  x86p_wasm_local_get(l->e, (uint32_t)local);
  wrap(l, 1);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
}

void x86p_wasm_x87_slot_pop(X86pWasmLower *l, int local) {
  cpu(l);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
  x86p_wasm_local_set(l->e, (uint32_t)local);
  x86p_wasm_x87_slot_retire(l, local);
}

#endif /* X86P_X87_BINARY128 */
