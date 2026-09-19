/* Conditional writes, flag transfers, multi-access stack operations and
 * architectural exits are emitted independently of the test dispatcher. */
#include "jit_wasm_control.h"
#include "jit_wasm_cond.h"
#include "jit_wasm_internal.h"
#include "privilege.h"

int x86p_wasm_trap(X86pCpu *cpu, uint32_t vector, uint32_t conditional) {
  if (conditional && !x86p_flag_of(&cpu->flags)) {
    return 0;
  }
  cpu->trap_vector = (uint8_t)vector;
  return 1;
}

int x86p_wasm_cmov_accepts(const X86pInsn *insn) {
  const int width = insn->operand[0].size;
  return insn->operands == 2 && (width == 2 || width == 4) && insn->operand[0].kind == kX86pOperandReg &&
         x86p_wasm_operand_ok(&insn->operand[0], width, 1) && insn->operand[1].kind != kX86pOperandImm &&
         x86p_wasm_operand_ok(&insn->operand[1], width, 0);
}

void x86p_wasm_cmov_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0], *src = &insn->operand[1];
  const int width = dst->size;
  /* Even an untaken CMOV reads its source and may fault there. */
  if (src->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, src, pc, width, kX86pMemRead);
    x86p_wasm_state_load_mem(&l->state, width);
  } else {
    x86p_wasm_push_operand(l, src, width);
  }
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  x86p_wasm_cond_value(l, (X86pCond)insn->cond);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_store_reg(&l->state, dst->reg, width, kX86pWasmLocalR);
  x86p_wasm_end(l->e);
}

void x86p_wasm_flags_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)pc;
  if (insn->op == kX86pInsnSahf || insn->op == kX86pInsnLahf) {
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_call_import(l, insn->op == kX86pInsnSahf ? kX86pWasmImportSahf : kX86pWasmImportLahf);
    if (insn->op == kX86pInsnSahf) {
      x86p_wasm_lower_flags_written(l, (int)kX86pFlagsExplicit, -1);
    }
    return;
  }
  if (insn->op == kX86pInsnSalc) {
    x86p_wasm_i32_const(l->e, 0);
    x86p_wasm_state_flags_addr(&l->state);
    x86p_wasm_call_import(l, kX86pWasmImportFlagCf);
    x86p_wasm_i32_op(l->e, kWasmI32Sub);
    x86p_wasm_local_set(l->e, kX86pWasmLocalR);
    x86p_wasm_state_store_reg(&l->state, kX86pEax, 1, kX86pWasmLocalR);
    return;
  }
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_call_import(l, kX86pWasmImportGetFlags);
  x86p_wasm_i32_const(l->e, insn->op == kX86pInsnClc ? (int32_t)~(uint32_t)X86P_CF : X86P_CF);
  x86p_wasm_i32_op(l->e, insn->op == kX86pInsnClc ? kWasmI32And : insn->op == kX86pInsnStc ? kWasmI32Or : kWasmI32Xor);
  x86p_wasm_call_import(l, kX86pWasmImportSetFlags);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  x86p_wasm_lower_flags_written(l, (int)kX86pFlagsExplicit, -1);
}

int x86p_wasm_enter_accepts(const X86pInsn *insn) {
  return insn->operands == 2 && insn->operand[0].kind == kX86pOperandImm && insn->operand[1].kind == kX86pOperandImm;
}

void x86p_wasm_stack_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  X86pWasmImport helper = insn->op == kX86pInsnPushad ? kX86pWasmImportPushad : kX86pWasmImportPopad;
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_const(l->e, (int32_t)x86p_wasm_memory_context(l));
  if (insn->op == kX86pInsnEnter) {
    x86p_wasm_i32_const(l->e, (int32_t)insn->operand[0].imm);
    x86p_wasm_i32_const(l->e, (int32_t)insn->operand[1].imm);
    helper = kX86pWasmImportEnter;
  }
  x86p_wasm_i32_const(l->e, 0); /* optional diagnostic fault-address output */
  x86p_wasm_call_import(l, helper);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitMemoryFault);
  x86p_wasm_end(l->e);
}

int x86p_wasm_trap_accepts(const X86pInsn *insn) {
  return insn->op != kX86pInsnInt || (insn->operands == 1 && insn->operand[0].kind == kX86pOperandImm);
}

void x86p_wasm_trap_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int32_t vector = insn->op == kX86pInsnInt    ? (int32_t)(insn->operand[0].imm & 255u)
                         : insn->op == kX86pInsnInt3 ? 3
                         : insn->op == kX86pInsnInt1 ? 1
                                                     : 4;
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_const(l->e, vector);
  x86p_wasm_i32_const(l->e, insn->op == kX86pInsnInto);
  x86p_wasm_call_import(l, kX86pWasmImportTrap);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitInterrupt);
  x86p_wasm_end(l->e);
  x86p_wasm_state_exit_imm(&l->state, pc + insn->length, kX86pJitExitBlockEnd);
}

void x86p_wasm_privilege_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_state_exit_imm(
      &l->state, pc, x86p_insn_is_privileged(insn) ? kX86pJitExitProtectionFault : kX86pJitExitUnsupported);
}

void x86p_wasm_cpu_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  (void)pc;
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_call_import(l, insn->op == kX86pInsnCpuid ? kX86pWasmImportCpuid : kX86pWasmImportRdtsc);
}
