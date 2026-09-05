#include "cond.h"
#include "jit_x64_internal.h"
#include <stddef.h>
void emit_epilogue(X86pEmit *e, uint32_t next_eip, X86pJitExit exit) {
  x86p_emit_store32_imm(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), next_eip);
  x86p_emit_mov_r32_imm32(e, kX64Rax, (uint32_t)exit);
  x86p_emit_pop_r64(e, CPU_REG);
  x86p_emit_ret(e);
}

/* The same exit, but with the guest EIP already computed into a register --
   which is what a conditional branch produces. */
void emit_epilogue_from(X86pEmit *e, X86pHostReg eip_reg, X86pJitExit exit) {
  x86p_emit_store32(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), eip_reg);
  x86p_emit_mov_r32_imm32(e, kX64Rax, (uint32_t)exit);
  x86p_emit_pop_r64(e, CPU_REG);
  x86p_emit_ret(e);
}

void emit_loop(BlockCtx *c, const X86pInsn *insn, uint32_t target, uint32_t next) {
  const int condition = insn->op == kX86pInsnLoope ? 1 : insn->op == kX86pInsnLoopne ? -1 : 0;
  x86p_emit_mov_r64_r64(c->e, kX64Rdi, CPU_REG);
  x86p_emit_mov_r32_imm32(c->e, kX64Rsi, insn->address_width);
  x86p_emit_mov_r32_imm32(c->e, kX64Rdx, (uint32_t)condition);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cpu_loop);
  x86p_emit_call_r64(c->e, kX64Rax);
  x86p_emit_test_r32_r32(c->e, kX64Rax, kX64Rax);
  x86p_emit_mov_r32_imm32(c->e, kX64Rax, next);
  x86p_emit_mov_r32_imm32(c->e, kX64Rcx, target);
  x86p_emit_cmovcc_r32_r32(c->e, kX86pCondNZ, kX64Rax, kX64Rcx);
  emit_epilogue_from(c->e, kX64Rax, kX86pJitExitBlockEnd);
}
