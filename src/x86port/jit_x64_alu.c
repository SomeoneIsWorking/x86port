#include "alu.h"
#include "bit_ops.h"
#include "flags.h"
#include "jit_x64_internal.h"
#include <stddef.h>

static int alu_writes_dest(uint8_t op) {
  return op != (uint8_t)kX86pAluCmp && op != (uint8_t)kX86pAluTest;
}

/* Preserve the caller's nonvolatile registers and 16-byte stack alignment.
 * Bounds checks precede the local save, so fault exits need no extra unwind. */
void emit_alu_helper(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pEmit *e = c->e;
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;
  const int mem_dst = dst->kind == kX86pOperandMem;

  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
  }
  if (mem_dst) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    /* Keep the helper's shadow/argument area below the saved register. */
    x86p_emit_push_r64(e, kX64R12);
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 8 + X86P_JIT_HOST_CALL_FRAME_BYTES);
    x86p_emit_mov_r64_r64(e, kX64R12, HOSTPTR_REG);
    emit_load_w(e, X86P_JIT_HOST_ARG1, kX64R12, 0, w); /* a */
  } else {
    emit_load_w(e, X86P_JIT_HOST_ARG1, CPU_REG, reg_off_w(dst->reg, w), w); /* a */
  }
  if (src->kind == kX86pOperandMem) {
    emit_load_w(e, X86P_JIT_HOST_ARG2, HOSTPTR_REG, 0, w);
  } else if (src->kind == kX86pOperandImm) {
    /* Masked by the DESTINATION width, which is what x86p_alu does to `b`.
       Not by the immediate's own size: `83 /r` reports size 1 and carries an
       already sign-extended dword, so masking it to a byte would turn
       ADD EAX, -1 into ADD EAX, 255. */
    x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG2, src->imm & x86p_width_mask(w)); /* b */
  } else {
    /* At the SOURCE's own width. For the binary operations that is the
       destination's width, but a shift's count is CL -- one byte -- and
       loading four would pass the whole of ECX as the count. */
    emit_load_w(e, X86P_JIT_HOST_ARG2, CPU_REG, reg_off_w(src->reg, src->size), src->size);
  }
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG0, (uint32_t)insn->alu);
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG3, (uint32_t)w);
  x86p_emit_lea64(e, kX64Rax, CPU_REG, flags_off());
  x86p_jit_abi_emit_arg64_reg(e, X86P_JIT_HOST_ABI, 4, kX64Rax);
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_alu);
  x86p_emit_call_r64(e, kX64Rax);
  if (alu_writes_dest(insn->alu)) {
    if (mem_dst) {
      emit_store_w(e, kX64R12, 0, kX64Rax, w);
    } else {
      emit_store_w(e, CPU_REG, reg_off_w(dst->reg, w), kX64Rax, w);
    }
  }
  if (mem_dst) {
    x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 8 + X86P_JIT_HOST_CALL_FRAME_BYTES);
    x86p_emit_pop_r64(e, kX64R12);
  }
}

int is_inline_shift(uint8_t alu) {
  return alu == (uint8_t)kX86pAluShl || alu == (uint8_t)kX86pAluShr || alu == (uint8_t)kX86pAluSar;
}

/*
 * SHL, SHR and SAR on the host, recording the tuple x86p_alu records: the
 * masked operand, the masked count, the result and the kind. Every flag is
 * then derived from that tuple by flags.c exactly as before; only the
 * arithmetic moved. On the Dead Zone route shifts were nearly every call to
 * x86p_alu, and with its x86p_flags_set and x86p_flag_cf about 5% of cycles.
 *
 * A count of zero (after the architectural five-bit mask) writes no flags and
 * leaves the destination alone, so it skips everything but a memory operand's
 * bounds check: known at translation for an immediate, tested at run time for
 * CL.
 *
 * The narrow widths come out of the 32-bit host shift unchanged: the operand
 * is loaded zero-extended, so SHR cannot pull in stray bits and SHL's are
 * masked off; SAR sign-extends first, so a count past the width fills with the
 * sign as x86p_alu's does.
 */
int emit_shift_inline(BlockCtx *c, const X86pInsn *insn, int flags_dead, uint32_t insn_eip) {
  X86pEmit *e = c->e;
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;
  const int by_cl = src->kind != kX86pOperandImm;
  const uint32_t count = src->imm & 0x1Fu;
  const X86pHostShift op = insn->alu == (uint8_t)kX86pAluShl   ? kX64Shl
                           : insn->alu == (uint8_t)kX86pAluShr ? kX64Shr
                                                               : kX64Sar;
  const X86pFlagKind kind = op == kX64Shl ? kX86pFlagsShl : op == kX64Shr ? kX86pFlagsShr : kX86pFlagsSar;
  X86pEmitSite zero = {0};

  if (dst->kind == kX86pOperandMem) {
    /* Even at a zero count: the access still happens, and faults. */
    emit_mem_prepare_w(c, dst, insn_eip, w);
  }
  if (!by_cl && count == 0u) {
    return SHIFT_FLAGS_UNCHANGED;
  }
  if (dst->kind == kX86pOperandMem) {
    emit_load_w(e, kX64Rsi, HOSTPTR_REG, 0, w);
  } else {
    emit_load_w(e, kX64Rsi, CPU_REG, reg_off_w(dst->reg, w), w);
  }
  if (by_cl) {
    emit_load_w(e, kX64Rcx, CPU_REG, reg_off_w(src->reg, src->size), src->size);
    x86p_emit_alu_r32_imm32(e, kX64And, kX64Rcx, 0x1Fu);
    zero = x86p_emit_jcc_rel32(e, (unsigned)kX86pCondZ);
  }

  x86p_emit_mov_r32_r32(e, kX64Rax, kX64Rsi);
  if (op == kX64Sar && w != 4) {
    x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rax, (uint8_t)(32 - 8 * w));
    x86p_emit_shift_r32_imm8(e, kX64Sar, kX64Rax, (uint8_t)(32 - 8 * w));
  }
  if (by_cl) {
    x86p_emit_shift_r32_cl(e, op, kX64Rax);
  } else {
    x86p_emit_shift_r32_imm8(e, op, kX64Rax, (uint8_t)count);
  }
  if (op != kX64Shr && w != 4) {
    x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, x86p_width_mask(w));
  }

  if (!flags_dead) {
    x86p_emit_store32(e, CPU_REG, FLAG_A, kX64Rsi);
    if (by_cl) {
      x86p_emit_store32(e, CPU_REG, FLAG_B, kX64Rcx);
    } else {
      x86p_emit_store32_imm(e, CPU_REG, FLAG_B, count);
    }
    x86p_emit_store32(e, CPU_REG, FLAG_R, kX64Rax);
    x86p_emit_store16_imm(e, CPU_REG, flag_kind_off(), flag_kind_word((unsigned)kind, (unsigned)w));
  }
  if (dst->kind == kX86pOperandMem) {
    emit_store_w(e, HOSTPTR_REG, 0, kX64Rax, w);
  } else {
    emit_store_w(e, CPU_REG, reg_off_w(dst->reg, w), kX64Rax, w);
  }
  if (by_cl) {
    x86p_emit_bind(e, zero);
    return SHIFT_FLAGS_UNKNOWN;
  }
  return (int)kind;
}

void emit_cpu_transfer(BlockCtx *c, uint8_t op) {
  void (*fn)(X86pCpu *) = op == kX86pInsnSahf    ? x86p_cpu_sahf
                          : op == kX86pInsnLahf  ? x86p_cpu_lahf
                          : op == kX86pInsnRdtsc ? x86p_cpu_rdtsc
                                                 : x86p_cpu_cpuid;
  if (op == kX86pInsnCld || op == kX86pInsnStd) {
    x86p_emit_store8_imm(c->e, CPU_REG, (int32_t)offsetof(X86pCpu, df), op == kX86pInsnStd);
    return;
  }
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)fn);
  x86p_emit_call_r64(c->e, kX64Rax);
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
    emit_load_w(c->e, X86P_JIT_HOST_ARG1, HOSTPTR_REG, 0, width);
  } else {
    emit_load_w(c->e, X86P_JIT_HOST_ARG1, CPU_REG, reg_off_w(operand->reg, width), width);
  }
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG2, insn->op == kX86pInsnImul);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG3, (uint32_t)width);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&jit_mul32);
  x86p_emit_call_r64(c->e, kX64Rax);
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
    x86p_emit_lea64(c->e, HOSTPTR_REG, CPU_REG, reg_off(dst->reg));
  }
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG1, HOSTPTR_REG);
  x86p_emit_load32(c->e, X86P_JIT_HOST_ARG2, CPU_REG, reg_off(src->reg));
  if (count->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG3, count->imm);
  } else {
    x86p_emit_load8_zx(c->e, X86P_JIT_HOST_ARG3, CPU_REG, reg_off(kX86pEcx));
  }
  x86p_jit_abi_emit_arg32_imm(c->e, X86P_JIT_HOST_ABI, 4, insn->op == kX86pInsnShld);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cpu_double_shift32);
  x86p_emit_call_r64(c->e, kX64Rax);
}
