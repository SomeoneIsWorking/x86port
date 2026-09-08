/* Integer forms whose arithmetic or repeat semantics already have a shared
 * owner. Emitted code owns operand access and fault exits; helpers never
 * decode or dispatch a guest instruction stream. */
#include "jit_wasm_integer.h"

#include "alu.h"
#include "jit_wasm_internal.h"
#include "string_ops.h"

uint32_t x86p_wasm_multiply(
    X86pCpu *cpu, uint32_t left, uint32_t right, uint32_t width, uint32_t signed_multiply, uint32_t implicit) {
  uint32_t low = 0u, high = 0u;
  if (signed_multiply) {
    x86p_alu_imul(left, right, (int)width, &low, &high, &cpu->flags);
  } else {
    x86p_alu_mul(left, right, (int)width, &low, &high, &cpu->flags);
  }
  if (implicit) {
    if (width == 1u) {
      x86p_reg_write(cpu, kX86pEax, 2, low | (high << 8));
    } else {
      x86p_reg_write(cpu, kX86pEax, (int)width, low);
      x86p_reg_write(cpu, kX86pEdx, (int)width, high);
    }
  }
  return low;
}

int x86p_wasm_divide(X86pCpu *cpu, uint32_t divisor, uint32_t width, uint32_t signed_divide) {
  const uint32_t low = x86p_reg_read(cpu, kX86pEax, (int)width);
  const uint32_t high = width == 1u ? x86p_reg_read(cpu, 4, 1) : x86p_reg_read(cpu, kX86pEdx, (int)width);
  uint32_t quotient = 0u, remainder = 0u;
  const int ok = signed_divide ? x86p_alu_idiv(high, low, divisor, (int)width, &quotient, &remainder, &cpu->flags)
                               : x86p_alu_div(high, low, divisor, (int)width, &quotient, &remainder, &cpu->flags);
  if (!ok) {
    return 0;
  }
  x86p_reg_write(cpu, kX86pEax, (int)width, quotient);
  x86p_reg_write(cpu, width == 1u ? 4 : kX86pEdx, (int)width, remainder);
  return 1;
}

int x86p_wasm_string(X86pCpu *cpu, const X86pMem *mem, uint32_t operation, uint32_t repeat, uint32_t width) {
  X86pInsn insn = {0};
  insn.op = kX86pInsnString;
  insn.str = (uint8_t)operation;
  insn.rep = (uint8_t)repeat;
  insn.str_width = (uint8_t)width;
  insn.address_width = 32;
  return (int)x86p_string_execute(cpu, mem, &insn, NULL);
}

uint32_t x86p_wasm_get_flags(X86pCpu *cpu) {
  return x86p_eflags(&cpu->flags) | (cpu->df ? X86P_DF : 0u);
}

uint32_t x86p_wasm_set_flags(X86pCpu *cpu, uint32_t value) {
  x86p_flags_set_explicit(&cpu->flags, value);
  cpu->df = (value & X86P_DF) != 0u;
  return value;
}

static void read_operand(X86pWasmLower *l, const X86pOperand *operand, int width, uint32_t pc) {
  if (operand->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, operand, pc, width, kX86pMemRead);
    x86p_wasm_state_load_mem(&l->state, width);
  } else {
    x86p_wasm_push_operand(l, operand, width);
  }
}

int x86p_wasm_multiply_accepts(const X86pInsn *insn) {
  const int width = insn->operand[0].size;
  if (insn->operands == 1) {
    return insn->operand[0].kind != kX86pOperandImm && x86p_wasm_operand_ok(&insn->operand[0], width, 0);
  }
  return insn->op == kX86pInsnImul && (insn->operands == 2 || insn->operands == 3) && (width == 2 || width == 4) &&
         insn->operand[0].kind == kX86pOperandReg && x86p_wasm_operand_ok(&insn->operand[0], width, 1) &&
         insn->operand[1].kind != kX86pOperandImm && x86p_wasm_operand_ok(&insn->operand[1], width, 0) &&
         (insn->operands == 2 || insn->operand[2].kind == kX86pOperandImm);
}

void x86p_wasm_multiply_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int width = insn->operand[0].size;
  if (insn->operands == 1) {
    read_operand(l, &insn->operand[0], width, pc);
    x86p_wasm_local_set(l->e, kX86pWasmLocalB);
    x86p_wasm_state_load_reg(&l->state, kX86pEax, width);
  } else if (insn->operands == 2) {
    read_operand(l, &insn->operand[1], width, pc);
    x86p_wasm_local_set(l->e, kX86pWasmLocalB);
    x86p_wasm_state_load_reg(&l->state, insn->operand[0].reg, width);
  } else {
    read_operand(l, &insn->operand[1], width, pc);
    x86p_wasm_local_set(l->e, kX86pWasmLocalA);
    x86p_wasm_push_operand(l, &insn->operand[2], width);
    x86p_wasm_local_set(l->e, kX86pWasmLocalB);
    x86p_wasm_local_get(l->e, kX86pWasmLocalA);
  }
  x86p_wasm_local_set(l->e, kX86pWasmLocalA);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_local_get(l->e, kX86pWasmLocalA);
  x86p_wasm_local_get(l->e, kX86pWasmLocalB);
  x86p_wasm_i32_const(l->e, width);
  x86p_wasm_i32_const(l->e, insn->op == kX86pInsnImul);
  x86p_wasm_i32_const(l->e, insn->operands == 1);
  x86p_wasm_call_import(l, kX86pWasmImportMultiply);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  if (insn->operands != 1) {
    x86p_wasm_state_store_reg(&l->state, insn->operand[0].reg, width, kX86pWasmLocalR);
  }
  l->last_kind = -1;
}

int x86p_wasm_divide_accepts(const X86pInsn *insn) {
  return insn->operands == 1 && insn->operand[0].kind != kX86pOperandImm &&
         x86p_wasm_operand_ok(&insn->operand[0], insn->operand[0].size, 0);
}

void x86p_wasm_divide_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int width = insn->operand[0].size;
  read_operand(l, &insn->operand[0], width, pc);
  x86p_wasm_local_set(l->e, kX86pWasmLocalB);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_local_get(l->e, kX86pWasmLocalB);
  x86p_wasm_i32_const(l->e, width);
  x86p_wasm_i32_const(l->e, insn->op == kX86pInsnIdiv);
  x86p_wasm_call_import(l, kX86pWasmImportDivide);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitDivideError);
  x86p_wasm_end(l->e);
  l->last_kind = -1;
}

int x86p_wasm_string_accepts(const X86pInsn *insn) {
  return insn->address_width == 32 &&
         x86p_string_is_supported((X86pStringOp)insn->str, (X86pRepKind)insn->rep, insn->str_width);
}

void x86p_wasm_string_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_const(l->e, (int32_t)x86p_wasm_memory_context(l));
  x86p_wasm_i32_const(l->e, insn->str);
  x86p_wasm_i32_const(l->e, insn->rep);
  x86p_wasm_i32_const(l->e, insn->str_width);
  x86p_wasm_call_import(l, kX86pWasmImportString);
  x86p_wasm_local_tee(l->e, kX86pWasmLocalR);
  x86p_wasm_i32_const(l->e, kX86pStringFault);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitMemoryFault);
  x86p_wasm_end(l->e);
  x86p_wasm_local_get(l->e, kX86pWasmLocalR);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitUnsupported);
  x86p_wasm_end(l->e);
  l->last_kind = -1;
}

int x86p_wasm_loop_accepts(const X86pInsn *insn) {
  return insn->operands == 1 && insn->operand[0].kind == kX86pOperandImm && insn->operand[0].relative &&
         (insn->address_width == 16 || insn->address_width == 32);
}

void x86p_wasm_loop_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const uint32_t next = pc + insn->length;
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_const(l->e, insn->address_width);
  x86p_wasm_i32_const(l->e, insn->op == kX86pInsnLoope ? 1 : insn->op == kX86pInsnLoopne ? -1 : 0);
  x86p_wasm_call_import(l, kX86pWasmImportLoop);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, next + insn->operand[0].imm, kX86pJitExitBlockEnd);
  x86p_wasm_end(l->e);
  x86p_wasm_state_exit_imm(&l->state, next, kX86pJitExitBlockEnd);
}

void x86p_wasm_pushfd_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_call_import(l, kX86pWasmImportGetFlags);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  x86p_wasm_push_local(l, kX86pWasmLocalR, pc);
}

void x86p_wasm_popfd_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)insn;
  x86p_wasm_state_load_reg(&l->state, kX86pEsp, 4);
  x86p_wasm_local_tee(l->e, kX86pWasmLocalA);
  x86p_wasm_local_set(l->e, kX86pWasmLocalAddr);
  x86p_wasm_state_guard_addr(&l->state, pc, 4, kX86pMemRead);
  x86p_wasm_state_load_mem(&l->state, 4);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_local_get(l->e, kX86pWasmLocalR);
  x86p_wasm_call_import(l, kX86pWasmImportSetFlags);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  x86p_wasm_local_get(l->e, kX86pWasmLocalA);
  x86p_wasm_i32_const(l->e, 4);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_set(l->e, kX86pWasmLocalA);
  x86p_wasm_state_store_reg(&l->state, kX86pEsp, 4, kX86pWasmLocalA);
  l->last_kind = kX86pFlagsExplicit;
}
