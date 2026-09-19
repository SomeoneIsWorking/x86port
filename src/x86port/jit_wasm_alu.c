/*
 * jit_wasm_alu.c -- the arithmetic and logic families.
 *
 * TWO PATHS, AND THE SPLIT IS ABOUT AUTHORITY RATHER THAN SPEED.
 *
 * ADD, SUB, CMP, OR, AND, TEST and XOR are lowered inline: the operation is
 * one WebAssembly instruction and the flag state it leaves behind is the plain
 * lazy tuple, stored field for field as x86p_flags_set stores it. Nothing
 * about the flag DERIVATIONS is reproduced -- only the tuple those derivations
 * read.
 *
 * ADC, SBB and every shift and rotate call x86p_alu. Their flag rules are not
 * a tuple: ADC and SBB compute a real EFLAGS word eagerly because a carry-in
 * cannot be expressed lazily, and a shift by a masked count of zero writes no
 * flags at all -- a rule of the instruction, not of the derivation. Emitting
 * either inline would make this file a second authority on rules that already
 * have one, free to disagree in a way that surfaces thousands of instructions
 * later as a branch taken the other way.
 */
#include "alu.h"
#include "jit_wasm_internal.h"

#include <stddef.h>

/* Which shape an inlined guest ALU op has: the WebAssembly operation that
   computes it, the flag kind it records, and whether it writes its
   destination. Returns 0 for the ops that go through x86p_alu. */
static int inline_shape(uint8_t alu, X86pWasmI32Op *op, X86pFlagKind *kind, int *writes_dest) {
  *writes_dest = 1;
  switch (alu) {
  case kX86pAluAdd:
    *op = kWasmI32Add;
    *kind = kX86pFlagsAdd;
    return 1;
  case kX86pAluSub:
    *op = kWasmI32Sub;
    *kind = kX86pFlagsSub;
    return 1;
  case kX86pAluCmp:
    *op = kWasmI32Sub;
    *kind = kX86pFlagsSub;
    *writes_dest = 0;
    return 1;
  case kX86pAluOr:
    *op = kWasmI32Or;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluAnd:
    *op = kWasmI32And;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluTest:
    *op = kWasmI32And;
    *kind = kX86pFlagsLogic;
    *writes_dest = 0;
    return 1;
  case kX86pAluXor:
    *op = kWasmI32Xor;
    *kind = kX86pFlagsLogic;
    return 1;
  default:
    return 0;
  }
}

static int is_shift_or_rotate(uint8_t alu) {
  return alu >= (uint8_t)kX86pAluShl && alu <= (uint8_t)kX86pAluRcr;
}

int x86p_wasm_alu_accepts(const X86pInsn *insn) {
  const X86pOperand *dst;
  const X86pOperand *src;
  int w;
  if (insn->operands != 2 || insn->alu >= (uint8_t)kX86pAluOpCount) {
    return 0;
  }
  dst = &insn->operand[0];
  src = &insn->operand[1];
  w = (int)dst->size;
  if (!x86p_wasm_operand_ok(dst, w, 1)) {
    return 0;
  }
  if (is_shift_or_rotate(insn->alu)) {
    /*
     * The second operand of a shift is a COUNT, not a value of the
     * destination's width: the encodings are an imm8, an implicit 1, or CL.
     * Requiring it to match the destination would refuse every `SHL EAX, CL`.
     */
    if (src->kind == kX86pOperandImm) {
      return 1;
    }
    return src->kind == kX86pOperandReg && src->size == 1;
  }
  if (!x86p_wasm_operand_ok(src, w, 0)) {
    return 0;
  }
  /* Two memory operands is not an encoding x86 has, and the one local this
     lowering keeps an address in could not hold both if it were. */
  return !(dst->kind == kX86pOperandMem && src->kind == kX86pOperandMem);
}

/* Push the count operand of a shift or rotate. x86p_alu masks it to five bits
   itself -- that masking is architectural and belongs in one place -- so this
   only has to deliver the byte the encoding named. */
static void push_count(X86pWasmLower *l, const X86pOperand *src) {
  if (src->kind == kX86pOperandImm) {
    x86p_wasm_i32_const(l->e, (int32_t)(src->imm & 0xFFu));
    return;
  }
  x86p_wasm_state_load_reg(&l->state, src->reg, 1);
}

/* The x86p_alu path: op, a, b, w, &cpu->flags -> result. */
static void lower_via_helper(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = (int)dst->size;
  const int shift = is_shift_or_rotate(insn->alu);

  /*
   * The memory operand, whichever side it is on, is prepared ONCE and its
   * address reused for the read and the write-back. Recomputing it for the
   * store would recompute it from registers the operation may have modified --
   * `ADC [EAX+4], EAX` must store where it loaded.
   */
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, w, kX86pMemRead | kX86pMemWrite);
  } else if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, src, pc, w, kX86pMemRead);
  }

  x86p_wasm_i32_const(l->e, (int32_t)insn->alu);
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_load_mem(&l->state, w);
  } else {
    x86p_wasm_state_load_reg(&l->state, dst->reg, w);
  }
  if (shift) {
    push_count(l, src);
  } else if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_load_mem(&l->state, w);
  } else {
    x86p_wasm_push_operand(l, src, w);
  }
  x86p_wasm_i32_const(l->e, w);
  x86p_wasm_state_flags_addr(&l->state);
  x86p_wasm_call_import(l, kX86pWasmImportAlu);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalR);
  } else {
    x86p_wasm_state_store_reg(&l->state, dst->reg, w, kX86pWasmLocalR);
  }

  if (shift) {
    /*
     * A shift's recorded kind depends on its COUNT, which is not known until
     * the block runs: a zero count writes no flags, leaving whatever was
     * there. Genuinely unknown, so the next carry-in asks the real function.
     */
    x86p_wasm_lower_flags_written(l, -1, -1);
  } else {
    /* x86p_alu records Explicit for ADC and SBB unconditionally, so the next
       instruction's predecessor IS known -- treating it as unknown cost a
       helper call per ADC in every block. */
    x86p_wasm_lower_flags_written(l, (int)kX86pFlagsExplicit, -1);
  }
}

void x86p_wasm_alu_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = (int)dst->size;
  X86pWasmI32Op op;
  X86pFlagKind kind;
  int writes_dest;

  if (!inline_shape(insn->alu, &op, &kind, &writes_dest)) {
    lower_via_helper(l, insn, pc);
    return;
  }

  /*
   * The carry-in is COMPUTED first, while the old flag state is still intact,
   * and STORED only after the memory operand's bounds check -- a refused
   * access must leave every flag field exactly as it was. Storing it before
   * the check leaves the flags half-updated on a fault: a divergence that
   * appears several instructions later, when something finally reads CF.
   */
  x86p_wasm_carry_in(l);

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, w, kX86pMemRead | (writes_dest ? kX86pMemWrite : 0u));
    x86p_wasm_state_store_carry(&l->state);
    x86p_wasm_state_load_mem(&l->state, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
    x86p_wasm_push_operand(l, src, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
  } else if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, src, pc, w, kX86pMemRead);
    x86p_wasm_state_store_carry(&l->state);
    x86p_wasm_state_load_mem(&l->state, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
    x86p_wasm_state_load_reg(&l->state, dst->reg, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  } else {
    x86p_wasm_state_store_carry(&l->state);
    x86p_wasm_push_operand(l, src, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
    x86p_wasm_state_load_reg(&l->state, dst->reg, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  }

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i32_op(l->e, op);
  if (w != 4) {
    /*
     * The tuple must hold the values x86p_alu would have stored, and it masks
     * a, b and r to the operand width. `a` and `b` arrive masked because they
     * were loaded zero-extended; `r` is the 32-bit result of a 32-bit
     * operation and is not. Every DERIVED flag masks by w and would agree
     * either way -- it is the raw tuple that would differ, which is exactly
     * what a caller inspecting flag state reads.
     */
    x86p_wasm_i32_const(l->e, (int32_t)x86p_wasm_width_mask(w));
    x86p_wasm_i32_op(l->e, kWasmI32And);
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);

  x86p_wasm_state_store_flags(&l->state, kind, w);

  if (writes_dest) {
    if (dst->kind == kX86pOperandMem) {
      x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalR);
    } else {
      x86p_wasm_state_store_reg(&l->state, dst->reg, w, kX86pWasmLocalR);
    }
  }
  x86p_wasm_lower_flags_written(l, (int)kind, w);
}

int x86p_wasm_alu_unary_accepts(const X86pInsn *insn) {
  const X86pOperand *dst;
  if (insn->operands != 1 || insn->alu >= (uint8_t)kX86pAluUnOpCount) {
    return 0;
  }
  dst = &insn->operand[0];
  return x86p_wasm_operand_ok(dst, (int)dst->size, 1);
}

void x86p_wasm_alu_unary_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0];
  const int w = (int)dst->size;

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, w, kX86pMemRead | kX86pMemWrite);
  }

  if (insn->alu == (uint8_t)kX86pAluNot) {
    /*
     * NOT writes NO flags, which is why it is inlined and NEG is not: as
     * `XOR a, -1` it would clear CF and OF, and as a call it would cost one
     * for an operation with no flag state to get right.
     */
    if (dst->kind == kX86pOperandMem) {
      x86p_wasm_state_load_mem(&l->state, w);
    } else {
      x86p_wasm_state_load_reg(&l->state, dst->reg, w);
    }
    x86p_wasm_i32_const(l->e, -1);
    x86p_wasm_i32_op(l->e, kWasmI32Xor);
    if (w != 4) {
      x86p_wasm_i32_const(l->e, (int32_t)x86p_wasm_width_mask(w));
      x86p_wasm_i32_op(l->e, kWasmI32And);
    }
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  } else {
    /*
     * NEG, INC and DEC go through x86p_alu_unary. INC and DEC PRESERVE CF,
     * which they do by having x86p_flags_set carry the current CF into
     * carry_in -- a rule that reads the flag state it is about to overwrite,
     * and one this file must not restate. `l->last_kind` below is what those
     * kinds are; the derivation stays where it is.
     */
    x86p_wasm_i32_const(l->e, (int32_t)insn->alu);
    if (dst->kind == kX86pOperandMem) {
      x86p_wasm_state_load_mem(&l->state, w);
    } else {
      x86p_wasm_state_load_reg(&l->state, dst->reg, w);
    }
    x86p_wasm_i32_const(l->e, w);
    x86p_wasm_state_flags_addr(&l->state);
    x86p_wasm_call_import(l, kX86pWasmImportAluUnary);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
    x86p_wasm_lower_flags_written(l,
                                  (insn->alu == (uint8_t)kX86pAluNeg)   ? (int)kX86pFlagsSub
                                  : (insn->alu == (uint8_t)kX86pAluInc) ? (int)kX86pFlagsInc
                                                                        : (int)kX86pFlagsDec,
                                  w);
  }

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalR);
  } else {
    x86p_wasm_state_store_reg(&l->state, dst->reg, w, kX86pWasmLocalR);
  }
}
