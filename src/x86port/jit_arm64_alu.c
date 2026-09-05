#include "alu.h"
#include "bit_ops.h"
#include "flags.h"
#include "jit_arm64_internal.h"
#include <stddef.h>

static int alu_writes_dest(uint8_t op) {
  return op != (uint8_t)kX86pAluCmp && op != (uint8_t)kX86pAluTest;
}

/* Preserve the caller's nonvolatile registers and 16-byte stack alignment.
 * Bounds checks precede the local save, so fault exits need no extra unwind. */
void emit_alu_helper(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;
  const int mem_dst = dst->kind == kX86pOperandMem;

  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
  }
  if (mem_dst) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    x86p_a64_emit_push_pair(e, kA64X20, kA64X21);
    x86p_a64_emit_mov_x_x(e, kA64X20, HOSTPTR_REG);
    emit_load_w(e, kA64X1, kA64X20, 0, w); /* a */
  } else {
    emit_load_w(e, kA64X1, CPU_REG, reg_off_w(dst->reg, w), w); /* a */
  }
  if (src->kind == kX86pOperandMem) {
    emit_load_w(e, kA64X2, HOSTPTR_REG, 0, w);
  } else if (src->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(e, kA64X2, src->imm & x86p_width_mask(w)); /* b */
  } else {
    /* At the SOURCE's own width: a shift's count is CL, one byte. */
    emit_load_w(e, kA64X2, CPU_REG, reg_off_w(src->reg, src->size), src->size);
  }
  x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)w);
  x86p_a64_emit_lea64(e, kA64X4, CPU_REG, flags_off());
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)insn->alu);
  emit_call(e, (void *)&x86p_alu);
  if (alu_writes_dest(insn->alu)) {
    if (mem_dst) {
      emit_store_w(e, kA64X20, 0, kA64X0, w);
    } else {
      emit_store_w(e, CPU_REG, reg_off_w(dst->reg, w), kA64X0, w);
    }
  }
  if (mem_dst) {
    x86p_a64_emit_pop_pair(e, kA64X20, kA64X21);
  }
}

void emit_cpu_transfer(BlockCtx *c, uint8_t op) {
  void (*fn)(X86pCpu *) = op == kX86pInsnSahf    ? x86p_cpu_sahf
                          : op == kX86pInsnLahf  ? x86p_cpu_lahf
                          : op == kX86pInsnRdtsc ? x86p_cpu_rdtsc
                                                 : x86p_cpu_cpuid;
  if (op == kX86pInsnCld || op == kX86pInsnStd) {
    x86p_a64_emit_store8_imm(c->e, CPU_REG, (int32_t)offsetof(X86pCpu, df), op == kX86pInsnStd);
    return;
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  emit_call(c->e, (void *)fn);
}

static void jit_mul32(X86pCpu *cpu, uint32_t operand, uint32_t signed_multiply, uint32_t width) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  if (signed_multiply) {
    x86p_alu_imul(x86p_reg_read(cpu, kX86pEax, (int)width), operand, (int)width, &low, &high, &cpu->flags);
  } else {
    x86p_alu_mul(x86p_reg_read(cpu, kX86pEax, (int)width), operand, (int)width, &low, &high, &cpu->flags);
  }
  if (width == 1) {
    x86p_reg_write(cpu, kX86pEax, 2, low | (high << 8));
  } else {
    x86p_reg_write(cpu, kX86pEax, (int)width, low);
    x86p_reg_write(cpu, kX86pEdx, (int)width, high);
  }
}

void emit_mul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *operand = &insn->operand[0];
  const int width = operand->size;

  if (operand->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, operand, insn_eip, width);
    emit_load_w(c->e, kA64X1, HOSTPTR_REG, 0, width);
  } else {
    emit_load_w(c->e, kA64X1, CPU_REG, reg_off_w(operand->reg, width), width);
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, insn->op == kX86pInsnImul);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X3, (uint32_t)width);
  emit_call(c->e, (void *)&jit_mul32);
}

int double_shift_is_emittable(const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0], *src = &insn->operand[1], *count = &insn->operand[2];
  return insn->operands == 3 && dst->size == 4 &&
         ((dst->kind == kX86pOperandReg && dst->reg >= 0 && dst->reg < 8) ||
          (dst->kind == kX86pOperandMem && !dst->addr16)) &&
         src->kind == kX86pOperandReg && src->size == 4 && src->reg >= 0 && src->reg < 8 &&
         (count->kind == kX86pOperandImm ||
          (count->kind == kX86pOperandReg && count->reg == kX86pEcx && count->size == 1));
}
void emit_double_shift(BlockCtx *c, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *dst = &insn->operand[0], *src = &insn->operand[1], *count = &insn->operand[2];
  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, pc, 4);
  } else {
    x86p_a64_emit_lea64(c->e, HOSTPTR_REG, CPU_REG, reg_off(dst->reg));
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_x_x(c->e, kA64X1, HOSTPTR_REG);
  x86p_a64_emit_load32(c->e, kA64X2, CPU_REG, reg_off(src->reg));
  if (count->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(c->e, kA64X3, count->imm);
  } else {
    x86p_a64_emit_load8_zx(c->e, kA64X3, CPU_REG, reg_off(kX86pEcx));
  }
  x86p_a64_emit_mov_w_imm32(c->e, kA64X4, insn->op == kX86pInsnShld);
  emit_call(c->e, (void *)&x86p_cpu_double_shift32);
}
