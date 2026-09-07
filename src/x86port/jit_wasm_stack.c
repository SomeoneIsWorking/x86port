/*
 * jit_wasm_stack.c -- PUSH and POP.
 *
 * The stack is ordinary guest memory, so both go through the same bounds check
 * and the same fault exit as any other access. What is specific to them is the
 * ORDER, and in both cases it is the order the architecture specifies:
 *
 *  - PUSH reads its operand BEFORE moving ESP, so `PUSH ESP` stores the old
 *    value, and it moves ESP only AFTER the store has been accepted, so a
 *    faulting push leaves the stack pointer where it was rather than past a
 *    value it never wrote. The second rule is invisible until something
 *    faults, and then it corrupts every frame above it.
 *  - POP advances ESP BEFORE writing the destination, so `POP ESP` ends
 *    holding the popped value rather than the adjusted pointer, and a memory
 *    destination is addressed from the ALREADY advanced ESP.
 *
 * Only the 32-bit forms are lowered. A 16-bit push moves ESP by two and
 * writes a word, which is a different instruction wearing the same mnemonic;
 * accepting it here at width four would silently corrupt the frame.
 */
#include "jit_wasm_internal.h"

#include <stddef.h>

void x86p_wasm_push_local(X86pWasmLower *l, X86pWasmLocal value, uint32_t pc) {
  x86p_wasm_state_load_reg(&l->state, kX86pEsp, 4);
  x86p_wasm_i32_const(l->e, 4);
  x86p_wasm_i32_op(l->e, kWasmI32Sub);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_state_guard_addr(&l->state, pc, 4);
  x86p_wasm_state_store_mem(&l->state, 4, value);

  /* Committed last: everything above can still leave with a memory fault. */
  x86p_wasm_state_store_reg(&l->state, kX86pEsp, 4, kX86pWasmLocalA);
}

int x86p_wasm_push_accepts(const X86pInsn *insn) {
  const X86pOperand *o;
  if (insn->operands != 1) {
    return 0;
  }
  o = &insn->operand[0];
  if (o->kind == kX86pOperandImm) {
    /*
     * `68 imm32` and `6A imm8` both push a DWORD -- the byte form is
     * sign-extended, which the decoder has already done. Size 2 is the
     * operand-size-prefixed form that pushes a word, and it is refused with
     * every other 16-bit push.
     */
    return o->size == 4 || o->size == 1;
  }
  return x86p_wasm_operand_ok(o, 4, 0);
}

void x86p_wasm_push_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *o = &insn->operand[0];

  /* Read first: `PUSH ESP` stores the value ESP had before the push, and a
     memory source must be read from where it is now. */
  if (o->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, o, pc, 4);
    x86p_wasm_state_load_mem(&l->state, 4);
  } else if (o->kind == kX86pOperandImm) {
    x86p_wasm_i32_const(l->e, (int32_t)o->imm);
  } else {
    x86p_wasm_state_load_reg(&l->state, o->reg, 4);
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);

  x86p_wasm_push_local(l, kX86pWasmLocalR, pc);
}

int x86p_wasm_pop_accepts(const X86pInsn *insn) {
  if (insn->operands != 1) {
    return 0;
  }
  return x86p_wasm_operand_ok(&insn->operand[0], 4, 1);
}

void x86p_wasm_pop_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *o = &insn->operand[0];

  x86p_wasm_state_load_reg(&l->state, kX86pEsp, 4);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_state_guard_addr(&l->state, pc, 4);
  x86p_wasm_state_load_mem(&l->state, 4);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);

  /* ESP advances before the destination is written, so `POP ESP` ends holding
     the popped value and a memory destination is addressed from the advanced
     pointer. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_i32_const(l->e, 4);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_state_store_reg(&l->state, kX86pEsp, 4, kX86pWasmLocalA);

  if (o->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, o, pc, 4);
    x86p_wasm_state_store_mem(&l->state, 4, kX86pWasmLocalR);
    return;
  }
  x86p_wasm_state_store_reg(&l->state, o->reg, 4, kX86pWasmLocalR);
}
