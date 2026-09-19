/* Bit and decimal arithmetic use their shared semantic owners. The lowering
 * resolves operands and guards writes before any guest flags can change. */
#include "jit_wasm_bitops.h"
#include "bcd.h"
#include "bit_ops.h"
#include "jit_wasm_internal.h"

uint32_t
x86p_wasm_double_shift(X86pCpu *cpu, uint32_t dst, uint32_t src, uint32_t count, uint32_t width, uint32_t left) {
  uint32_t flags = x86p_eflags(&cpu->flags), result = dst;
  int defined = 0;
  if (x86p_double_shift((int)left, dst, src, count, (int)width, &result, &flags, &defined)) {
    x86p_flags_set_explicit(&cpu->flags, flags);
  }
  return result;
}

uint32_t x86p_wasm_bit(X86pCpu *cpu, uint32_t value, uint32_t index, uint32_t operation) {
  uint32_t flags = x86p_eflags(&cpu->flags);
  const uint32_t result = x86p_bit_apply((X86pBitOp)operation, value, index, &flags);
  x86p_flags_set_explicit(&cpu->flags, flags);
  return result;
}

int x86p_wasm_bcd(X86pCpu *cpu, uint32_t operation, uint32_t immediate) {
  uint16_t ax = (uint16_t)x86p_reg_read(cpu, kX86pEax, 2);
  uint32_t flags = x86p_eflags(&cpu->flags);
  if (!x86p_bcd_apply((X86pBcdOp)operation, &ax, &flags, (uint8_t)immediate)) {
    return 0;
  }
  x86p_reg_write(cpu, kX86pEax, 2, ax);
  x86p_flags_set_explicit(&cpu->flags, flags);
  return 1;
}

int x86p_wasm_shift_accepts(const X86pInsn *insn) {
  const int width = insn->operand[0].size;
  return insn->operands == 3 && (width == 2 || width == 4) && x86p_wasm_operand_ok(&insn->operand[0], width, 1) &&
         insn->operand[1].kind == kX86pOperandReg && x86p_wasm_operand_ok(&insn->operand[1], width, 0) &&
         (insn->operand[2].kind == kX86pOperandImm ||
          (insn->operand[2].kind == kX86pOperandReg && insn->operand[2].reg == kX86pEcx && insn->operand[2].size == 1));
}

void x86p_wasm_shift_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0];
  const int width = dst->size;
  /* A zero count still reads the destination, but has no write permission
   * requirement. A variable count therefore gates the write guard separately. */
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, width, kX86pMemRead);
    x86p_wasm_state_load_mem(&l->state, width);
  } else {
    x86p_wasm_push_operand(l, dst, width);
  }
  x86p_wasm_local_set(l->e, kX86pWasmLocalA);
  x86p_wasm_push_operand(l, &insn->operand[2], 1);
  x86p_wasm_i32_const(l->e, 31);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_tee(l->e, kX86pWasmLocalB);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_local_get(l->e, kX86pWasmLocalB);
  x86p_wasm_i32_const(l->e, width * 8);
  x86p_wasm_i32_op(l->e, kWasmI32GtU);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitUnsupported);
  x86p_wasm_end(l->e);
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, dst, pc, width, kX86pMemWrite);
  }
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_local_get(l->e, kX86pWasmLocalA);
  x86p_wasm_push_operand(l, &insn->operand[1], width);
  x86p_wasm_local_get(l->e, kX86pWasmLocalB);
  x86p_wasm_i32_const(l->e, width);
  x86p_wasm_i32_const(l->e, insn->op == kX86pInsnShld);
  x86p_wasm_call_import(l, kX86pWasmImportDoubleShift);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_store_mem(&l->state, width, kX86pWasmLocalR);
  } else {
    x86p_wasm_state_store_reg(&l->state, dst->reg, width, kX86pWasmLocalR);
  }
  x86p_wasm_end(l->e);
  x86p_wasm_lower_flags_written(l, -1, -1);
}

int x86p_wasm_bit_accepts(const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0], *index = &insn->operand[1];
  const int width = dst->size;
  return insn->operands == 2 && insn->bit < kX86pBitOpCount && (width == 2 || width == 4) &&
         x86p_wasm_operand_ok(dst, width, 1) &&
         (index->kind == kX86pOperandImm || (index->kind == kX86pOperandReg && x86p_wasm_operand_ok(index, width, 0) &&
                                             (width == 4 || dst->kind == kX86pOperandReg)));
}

void x86p_wasm_bit_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0], *index = &insn->operand[1];
  const int width = dst->size;
  const unsigned access = kX86pMemRead | (insn->bit == kX86pBitTest ? 0u : kX86pMemWrite);
  x86p_wasm_push_operand(l, index, width);
  x86p_wasm_local_set(l->e, kX86pWasmLocalB);
  if (dst->kind == kX86pOperandMem) {
    x86p_wasm_state_address(&l->state, dst);
    if (index->kind == kX86pOperandReg) {
      /* Width32 register offsets address a signed bit string. Arithmetic
       * right shift performs floor division even for negative offsets. */
      x86p_wasm_local_get(l->e, kX86pWasmLocalB);
      x86p_wasm_i32_const(l->e, 5);
      x86p_wasm_i32_op(l->e, kWasmI32ShrS);
      x86p_wasm_i32_const(l->e, 2);
      x86p_wasm_i32_op(l->e, kWasmI32Shl);
      x86p_wasm_i32_op(l->e, kWasmI32Add);
    }
    x86p_wasm_local_set(l->e, kX86pWasmLocalAddr);
    x86p_wasm_state_guard_addr(&l->state, pc, width, access);
    x86p_wasm_state_load_mem(&l->state, width);
  } else {
    x86p_wasm_push_operand(l, dst, width);
  }
  x86p_wasm_local_set(l->e, kX86pWasmLocalA);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_local_get(l->e, kX86pWasmLocalA);
  x86p_wasm_local_get(l->e, kX86pWasmLocalB);
  x86p_wasm_i32_const(l->e, width * 8 - 1);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_const(l->e, insn->bit);
  x86p_wasm_call_import(l, kX86pWasmImportBit);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  if (insn->bit != kX86pBitTest) {
    if (dst->kind == kX86pOperandMem) {
      x86p_wasm_state_store_mem(&l->state, width, kX86pWasmLocalR);
    } else {
      x86p_wasm_state_store_reg(&l->state, dst->reg, width, kX86pWasmLocalR);
    }
  }
  x86p_wasm_lower_flags_written(l, (int)kX86pFlagsExplicit, -1);
}

int x86p_wasm_bcd_accepts(const X86pInsn *insn) {
  return insn->bcd < kX86pBcdOpCount &&
         (insn->operands == 0 || (insn->operands == 1 && insn->operand[0].kind == kX86pOperandImm));
}

void x86p_wasm_bcd_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_const(l->e, insn->bcd);
  x86p_wasm_i32_const(l->e, insn->operands ? (int32_t)insn->operand[0].imm : 10);
  x86p_wasm_call_import(l, kX86pWasmImportBcd);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitDivideError);
  x86p_wasm_end(l->e);
  x86p_wasm_lower_flags_written(l, (int)kX86pFlagsExplicit, -1);
}
