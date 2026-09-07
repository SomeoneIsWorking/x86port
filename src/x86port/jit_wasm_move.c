/*
 * jit_wasm_move.c -- the families that move or reshape a value without
 * arithmetic: MOV, MOVZX/MOVSX, LEA, XCHG, SETcc, LEAVE, CDQ/CWDE, CLD/STD
 * and NOP.
 *
 * None of these writes the lazy flag tuple, so none of them touches
 * `last_kind`: the predecessor for the NEXT instruction's carry-in is still
 * whatever wrote flags before them. A move that reset it would cost a helper
 * call for every `mov` between an `add` and an `adc`.
 */
#include "jit_wasm_internal.h"

#include <stddef.h>

void x86p_wasm_nop_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)l;
  (void)insn;
  (void)pc;
}

/* ---- MOV ----------------------------------------------------------------- */

int x86p_wasm_mov_accepts(const X86pInsn *insn) {
  const X86pOperand *dst;
  const X86pOperand *src;
  int w;
  if (insn->operands != 2) {
    return 0;
  }
  dst = &insn->operand[0];
  src = &insn->operand[1];
  w = (int)dst->size;
  if (!x86p_wasm_operand_ok(dst, w, 1) || !x86p_wasm_operand_ok(src, w, 0)) {
    return 0;
  }
  return !(dst->kind == kX86pOperandMem && src->kind == kX86pOperandMem);
}

void x86p_wasm_mov_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = (int)dst->size;

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, w);
    x86p_wasm_push_operand(l, src, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalR);
    return;
  }
  if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, src, pc, w);
    x86p_wasm_state_load_mem(&l->state, w);
  } else {
    x86p_wasm_push_operand(l, src, w);
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_state_store_reg(&l->state, dst->reg, w, kX86pWasmLocalR);
}

/* ---- MOVZX and MOVSX ----------------------------------------------------- */

int x86p_wasm_movx_accepts(const X86pInsn *insn) {
  const X86pOperand *dst;
  const X86pOperand *src;
  if (insn->operands != 2) {
    return 0;
  }
  dst = &insn->operand[0];
  src = &insn->operand[1];
  /* The destination of a widening move is always a register, and it must be
     WIDER than the source -- an equal-width form would be a plain MOV, and
     accepting it here would apply a shift pair of 32 bits. */
  if (dst->kind != kX86pOperandReg || !x86p_wasm_width_ok((int)dst->size)) {
    return 0;
  }
  if (src->size >= dst->size || (src->size != 1 && src->size != 2)) {
    return 0;
  }
  return x86p_wasm_operand_ok(src, (int)src->size, 0) && src->kind != kX86pOperandImm;
}

static void lower_movx(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc, int is_signed) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int sw = (int)src->size;

  if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, src, pc, sw);
    x86p_wasm_state_load_mem(&l->state, sw);
  } else {
    x86p_wasm_state_load_reg(&l->state, src->reg, sw);
  }
  if (is_signed) {
    /*
     * Sign-extension as a shift pair rather than i32.extend8_s/extend16_s.
     * Those opcodes are the sign-extension-ops proposal, which is universally
     * implemented now but is still a feature an engine may be configured
     * without; the shift pair is in the WebAssembly 1.0 core and computes the
     * same value. The choice is recorded here rather than left to look like an
     * oversight.
     */
    const int32_t bits = 32 - sw * 8;
    x86p_wasm_i32_const(l->e, bits);
    x86p_wasm_i32_op(l->e, kWasmI32Shl);
    x86p_wasm_i32_const(l->e, bits);
    x86p_wasm_i32_op(l->e, kWasmI32ShrS);
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  /* A 16-bit destination is written with a 16-bit store, which truncates by
     itself -- so no masking, and the other half of the register is preserved
     exactly as cpu.h requires. */
  x86p_wasm_state_store_reg(&l->state, dst->reg, (int)dst->size, kX86pWasmLocalR);
}

void x86p_wasm_movzx_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  lower_movx(l, insn, pc, 0);
}

void x86p_wasm_movsx_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  lower_movx(l, insn, pc, 1);
}

/* ---- LEA ----------------------------------------------------------------- */

int x86p_wasm_lea_accepts(const X86pInsn *insn) {
  const X86pOperand *dst;
  const X86pOperand *src;
  if (insn->operands != 2) {
    return 0;
  }
  dst = &insn->operand[0];
  src = &insn->operand[1];
  if (dst->kind != kX86pOperandReg || dst->size != 4) {
    return 0;
  }
  return src->kind == kX86pOperandMem && !src->addr16;
}

void x86p_wasm_lea_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)pc;
  /*
   * The address PARTS, with no segment base and no bounds check. LEA is the
   * one memory-operand form that does not access memory, so checking it would
   * fault on an address the guest deliberately never touched -- and guest code
   * really does use it as a three-input adder on values that are not addresses
   * at all.
   *
   * NOT VERIFIED, AND SAYING SO: the segment half of that rule cannot be
   * tested through this decoder. `64 8D 4B 08` -- an LEA with an FS prefix --
   * decodes with the operand's segment left at DS, so emitted code that DID
   * add the segment base would compute the same address and every test would
   * pass. The split exists because the rule is real, not because a case
   * distinguishes it.
   */
  x86p_wasm_state_address_parts(&l->state, &insn->operand[1]);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_state_store_reg(&l->state, insn->operand[0].reg, 4, kX86pWasmLocalR);
}

/* ---- XCHG ---------------------------------------------------------------- */

int x86p_wasm_xchg_accepts(const X86pInsn *insn) {
  const X86pOperand *a;
  const X86pOperand *b;
  int w;
  if (insn->operands != 2) {
    return 0;
  }
  a = &insn->operand[0];
  b = &insn->operand[1];
  w = (int)a->size;
  if (!x86p_wasm_operand_ok(a, w, 1) || !x86p_wasm_operand_ok(b, w, 1)) {
    return 0;
  }
  return !(a->kind == kX86pOperandMem && b->kind == kX86pOperandMem);
}

void x86p_wasm_xchg_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *a = &insn->operand[0];
  const X86pOperand *b = &insn->operand[1];
  const int w = (int)a->size;

  /*
   * Both values are read before either is written, and the memory side's
   * address is formed once. The exchange is not atomic and does not need to
   * be: the guest is single-threaded through this engine, and a LOCK prefix
   * on it would be a claim about other threads that this framework does not
   * make anywhere else either.
   */
  if (b->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, b, pc, w);
    x86p_wasm_state_load_mem(&l->state, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
    x86p_wasm_state_load_reg(&l->state, a->reg, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
    x86p_wasm_state_store_reg(&l->state, a->reg, w, kX86pWasmLocalB);
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalA);
    return;
  }
  if (a->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, a, pc, w);
    x86p_wasm_state_load_mem(&l->state, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
    x86p_wasm_state_load_reg(&l->state, b->reg, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
    x86p_wasm_state_store_reg(&l->state, b->reg, w, kX86pWasmLocalA);
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalB);
    return;
  }
  x86p_wasm_state_load_reg(&l->state, a->reg, w);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_state_load_reg(&l->state, b->reg, w);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_state_store_reg(&l->state, a->reg, w, kX86pWasmLocalB);
  x86p_wasm_state_store_reg(&l->state, b->reg, w, kX86pWasmLocalA);
}

/* ---- SETcc --------------------------------------------------------------- */

int x86p_wasm_setcc_accepts(const X86pInsn *insn) {
  const X86pOperand *dst;
  if (insn->operands != 1) {
    return 0;
  }
  dst = &insn->operand[0];
  return x86p_wasm_operand_ok(dst, 1, 1);
}

void x86p_wasm_setcc_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0];
  /* The canonical condition evaluator's 0/1 result, materialised without
     touching guest flags. */
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, 1);
  }
  x86p_wasm_i32_const(l->e, (int32_t)insn->cond);
  x86p_wasm_state_flags_addr(&l->state);
  x86p_wasm_call_import(l, kX86pWasmImportCond);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_store_mem(&l->state, 1, kX86pWasmLocalR);
    return;
  }
  x86p_wasm_state_store_reg(&l->state, dst->reg, 1, kX86pWasmLocalR);
}

/* ---- LEAVE, CDQ, CWDE, CLD, STD ------------------------------------------ */

void x86p_wasm_leave_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  /*
   * LEAVE is an ORDERED state transition, not a MOV followed by an ordinary
   * POP: ESP becomes EBP before the stack read, and REMAINS there if that read
   * faults. Only a successful read advances ESP and replaces EBP.
   */
  x86p_wasm_state_load_reg(&l->state, kX86pEbp, 4);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_state_store_reg(&l->state, kX86pEsp, 4, kX86pWasmLocalA);

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_state_guard_addr(&l->state, pc, 4);
  x86p_wasm_state_load_mem(&l->state, 4);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_i32_const(l->e, 4);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_state_store_reg(&l->state, kX86pEsp, 4, kX86pWasmLocalA);
  x86p_wasm_state_store_reg(&l->state, kX86pEbp, 4, kX86pWasmLocalR);
}

void x86p_wasm_cdq_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  (void)pc;
  x86p_wasm_state_load_reg(&l->state, kX86pEax, 4);
  x86p_wasm_i32_const(l->e, 31);
  x86p_wasm_i32_op(l->e, kWasmI32ShrS);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_state_store_reg(&l->state, kX86pEdx, 4, kX86pWasmLocalR);
}

void x86p_wasm_cwde_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  (void)pc;
  x86p_wasm_state_load_reg(&l->state, kX86pEax, 2);
  x86p_wasm_i32_const(l->e, 16);
  x86p_wasm_i32_op(l->e, kWasmI32Shl);
  x86p_wasm_i32_const(l->e, 16);
  x86p_wasm_i32_op(l->e, kWasmI32ShrS);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_state_store_reg(&l->state, kX86pEax, 4, kX86pWasmLocalR);
}

void x86p_wasm_cld_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  (void)pc;
  x86p_wasm_state_store_df(&l->state, 0);
}

void x86p_wasm_std_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  (void)pc;
  x86p_wasm_state_store_df(&l->state, 1);
}
