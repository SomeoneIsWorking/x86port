#include "cond.h"
#include "jit_arm64_internal.h"
#include <stddef.h>
void emit_epilogue(X86pA64Emit *e, uint32_t next_eip, X86pJitExit exit) {
  x86p_a64_emit_store32_imm(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), next_eip);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)exit);
  x86p_a64_emit_pop_pair(e, CPU_REG, kA64Lr);
  x86p_a64_emit_ret(e);
}

void emit_epilogue_from(X86pA64Emit *e, X86pA64Reg eip_reg, X86pJitExit exit) {
  x86p_a64_emit_store32(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), eip_reg);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)exit);
  x86p_a64_emit_pop_pair(e, CPU_REG, kA64Lr);
  x86p_a64_emit_ret(e);
}

void emit_loop(BlockCtx *c, const X86pInsn *insn, uint32_t target, uint32_t next) {
  const int condition = insn->op == kX86pInsnLoope ? 1 : insn->op == kX86pInsnLoopne ? -1 : 0;
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, insn->address_width);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, (uint32_t)condition);
  x86p_a64_emit_mov_x_imm64(c->e, kA64X9, (uint64_t)(uintptr_t)&x86p_cpu_loop);
  x86p_a64_emit_blr(c->e, kA64X9);
  x86p_a64_emit_tst_w_w(c->e, kA64X0, kA64X0);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X0, next);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, target);
  x86p_a64_emit_csel_w(c->e, kA64CondNe, kA64X0, kA64X1, kA64X0);
  emit_epilogue_from(c->e, kA64X0, kX86pJitExitBlockEnd);
}
