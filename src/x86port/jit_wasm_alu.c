/*
 * jit_wasm_alu.c -- the arithmetic and logic families.
 *
 * TWO PATHS, AND THE SPLIT IS ABOUT AUTHORITY RATHER THAN SPEED.
 *
 * ADD, SUB, CMP, OR, AND, TEST and XOR are lowered inline, and so are NOT,
 * NEG, INC and DEC: the operation is
 * one WebAssembly instruction and the flag state it leaves behind is the plain
 * lazy tuple, stored field for field as x86p_flags_set stores it. Nothing
 * about the flag DERIVATIONS is reproduced -- only the tuple those derivations
 * read.
 *
 * So do SHL, SHR and SAR by an immediate count inside the width, whose kind
 * the count settles at translation time.
 *
 * ADC, SBB, the rotates and the other shifts call x86p_alu. Their flag rules
 * are not a tuple: ADC and SBB compute a real EFLAGS word eagerly because a carry-in
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

/*
 * An immediate SHL, SHR or SAR whose masked count is in [1, width - 1] bits:
 * its flag kind is known at translation time and its result is one shift.
 * Returns that count, or 0 for a shift x86p_alu answers -- a CL count, a zero
 * count (which writes no flags at all), a count at or past the width -- and
 * for every other operation.
 */
static uint32_t inline_shift_count(const X86pInsn *insn) {
  const X86pOperand *src = &insn->operand[1];
  uint32_t count;
  if (insn->alu != (uint8_t)kX86pAluShl && insn->alu != (uint8_t)kX86pAluShr && insn->alu != (uint8_t)kX86pAluSar) {
    return 0u;
  }
  if (src->kind != kX86pOperandImm) {
    return 0u;
  }
  count = src->imm & 0x1Fu;
  return count < (uint32_t)insn->operand[0].size * 8u ? count : 0u;
}

int x86p_wasm_alu_calls_helper(const X86pInsn *insn) {
  X86pWasmI32Op op;
  X86pFlagKind kind;
  int writes_dest;
  return !inline_shape(insn->alu, &op, &kind, &writes_dest) && inline_shift_count(insn) == 0u;
}

/* The tuple x86p_alu stores for a shift by `count`: (a, count, r). SAR
   replicates the sign of a narrower operand by moving it to bit 31 first. */
static void lower_shift(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc, uint32_t count) {
  const X86pOperand *dst = &insn->operand[0];
  const int w = (int)dst->size;
  const uint32_t spare = 32u - (uint32_t)w * 8u;
  const X86pFlagKind kind = (insn->alu == (uint8_t)kX86pAluShl)   ? kX86pFlagsShl
                            : (insn->alu == (uint8_t)kX86pAluShr) ? kX86pFlagsShr
                                                                  : kX86pFlagsSar;
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, w, kX86pMemRead | kX86pMemWrite);
    x86p_wasm_state_load_mem(&l->state, w);
  } else {
    x86p_wasm_state_load_reg(&l->state, dst->reg, w);
  }
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalA);
  if (kind == kX86pFlagsSar && spare != 0u) {
    x86p_wasm_i32_const(l->e, (int32_t)spare);
    x86p_wasm_i32_op(l->e, kWasmI32Shl);
    x86p_wasm_i32_const(l->e, (int32_t)(spare + count));
  } else {
    x86p_wasm_i32_const(l->e, (int32_t)count);
  }
  x86p_wasm_i32_op(l->e, kind == kX86pFlagsShl ? kWasmI32Shl : kind == kX86pFlagsShr ? kWasmI32ShrU : kWasmI32ShrS);
  if (kind != kX86pFlagsShr && w != 4) {
    x86p_wasm_i32_const(l->e, (int32_t)x86p_wasm_width_mask(w));
    x86p_wasm_i32_op(l->e, kWasmI32And);
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_i32_const(l->e, (int32_t)count);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_state_store_flags(&l->state, kind, w);
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalR);
  } else {
    x86p_wasm_state_store_reg(&l->state, dst->reg, w, kX86pWasmLocalR);
  }
  x86p_wasm_lower_flags_written(l, (int)kind, w);
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
     * A shift's recorded kind depends on its COUNT, which here is not known
     * until the block runs: a zero count writes no flags, leaving whatever was
     * there. Genuinely unknown, so the next carry-in dispatches on it.
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
  const uint32_t shift_count = inline_shift_count(insn);

  if (shift_count != 0u) {
    lower_shift(l, insn, pc, shift_count);
    return;
  }
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
   *
   * Only when x86p_flags_carry_in_is_live says the recorded kind reads it,
   * which no kind inline_shape records does. Deriving it anyway cost the first
   * flag write of nearly every block a call to x86p_flag_cf, whose predecessor
   * a block never knows: 2.8% of the browser's guest worker.
   */
  const int carry_live = x86p_flags_carry_in_is_live(kind);
  if (carry_live) {
    x86p_wasm_carry_in(l);
  }

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, w, kX86pMemRead | (writes_dest ? kX86pMemWrite : 0u));
    if (carry_live) {
      x86p_wasm_state_store_carry(&l->state);
    }
    x86p_wasm_state_load_mem(&l->state, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
    x86p_wasm_push_operand(l, src, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
  } else if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, src, pc, w, kX86pMemRead);
    if (carry_live) {
      x86p_wasm_state_store_carry(&l->state);
    }
    x86p_wasm_state_load_mem(&l->state, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
    x86p_wasm_state_load_reg(&l->state, dst->reg, w);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  } else {
    if (carry_live) {
      x86p_wasm_state_store_carry(&l->state);
    }
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
    /* NOT writes NO flags: as `XOR a, -1` it would clear CF and OF. */
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
     * The tuple x86p_alu_unary stores. NEG records the SUB it is, (0, a), so
     * CF falls out of the borrow. INC and DEC record (a, 1) and PRESERVE CF:
     * their kinds read carry_in, derived here from the state this instruction
     * is about to overwrite.
     */
    const X86pFlagKind kind = (insn->alu == (uint8_t)kX86pAluNeg)   ? kX86pFlagsSub
                              : (insn->alu == (uint8_t)kX86pAluInc) ? kX86pFlagsInc
                                                                    : kX86pFlagsDec;
    if (x86p_flags_carry_in_is_live(kind)) {
      x86p_wasm_carry_in(l);
      x86p_wasm_state_store_carry(&l->state);
    }
    if (kind == kX86pFlagsSub) {
      x86p_wasm_i32_const(l->e, 0);
      x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
    }
    if (dst->kind == kX86pOperandMem) {
      x86p_wasm_state_load_mem(&l->state, w);
    } else {
      x86p_wasm_state_load_reg(&l->state, dst->reg, w);
    }
    x86p_wasm_local_set(l->e, (uint32_t)(kind == kX86pFlagsSub ? kX86pWasmLocalB : kX86pWasmLocalA));
    if (kind != kX86pFlagsSub) {
      x86p_wasm_i32_const(l->e, 1);
      x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
    }
    x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
    x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
    x86p_wasm_i32_op(l->e, kind == kX86pFlagsInc ? kWasmI32Add : kWasmI32Sub);
    if (w != 4) {
      x86p_wasm_i32_const(l->e, (int32_t)x86p_wasm_width_mask(w));
      x86p_wasm_i32_op(l->e, kWasmI32And);
    }
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
    x86p_wasm_state_store_flags(&l->state, kind, w);
    x86p_wasm_lower_flags_written(l, (int)kind, w);
  }

  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_store_mem(&l->state, w, kX86pWasmLocalR);
  } else {
    x86p_wasm_state_store_reg(&l->state, dst->reg, w, kX86pWasmLocalR);
  }
}
