/*
 * jit_x64_cond.c -- Jcc and SETcc for the x64 backend.
 *
 * Split out of jit_x64.c to match jit_arm64_cond.c: condition lowering is one
 * responsibility, and it is where a backend either reads a condition off the
 * host's own flags or pays for the authority. This backend does not lower any
 * condition inline yet -- every one is a call to x86p_cond -- and the counters
 * say so rather than reading as though there were nothing to lower.
 */
#include "jit_x64_cond.h"

#include "cond.h"
#include "jit_x64_abi.h"
#include "jit_x64_internal.h"

/* Counted, not left at zero: see cond_helper_calls in jit_x64.h. */
static void emit_condition_value(BlockCtx *c, uint8_t cond) {
  X86pEmit *e = c->e;
  /* This backend has no record of the predecessor's flag kind at the
     condition site at all, so every condition it does not inline is one
     whose predecessor is unknown. Counted rather than left at zero, which
     would read as "classified, and none were unknown". */
  c->cond_helper_calls++;
  c->cond_unknown_kind++;
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG0, (uint32_t)cond);
  x86p_emit_lea64(e, X86P_JIT_HOST_ARG1, CPU_REG, flags_off());
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cond);
  x86p_emit_call_r64(e, kX64Rax);
}

void x86p_x64_emit_jcc(BlockCtx *c, uint8_t cond, uint32_t target, uint32_t fallthrough) {
  X86pEmit *e = c->e;
  c->conds++;
  emit_condition_value(c, cond);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  x86p_emit_mov_r32_imm32(e, kX64Rax, fallthrough);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, target);
  x86p_emit_cmovcc_r32_r32(e, (unsigned)kX86pCondNZ, kX64Rax, kX64Rcx);
  emit_epilogue_from(e, kX64Rax, kX86pJitExitBlockEnd);
}

/* SETcc materialises the canonical condition evaluator's 0/1 result without
   touching guest flags. A memory destination computes the condition first,
   then preserves it in RCX while the shared address/bounds path uses RAX. */
void x86p_x64_emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];

  c->conds++;
  emit_condition_value(c, insn->cond);
  if (dst->kind == kX86pOperandMem) {
    x86p_emit_mov_r32_r32(c->e, CARRY_REG, kX64Rax);
    emit_mem_prepare_w(c, dst, insn_eip, 1);
    x86p_emit_store8_reg(c->e, HOSTPTR_REG, 0, CARRY_REG);
    return;
  }
  x86p_emit_store8_reg(c->e, CPU_REG, reg_off_w(dst->reg, 1), kX64Rax);
}
