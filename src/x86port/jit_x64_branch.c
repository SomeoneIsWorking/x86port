/* jit_x64_branch.c -- block entry and exits, and the control transfers that end
   a block. */
#include "cond.h"
#include "jit_x64_abi.h"
#include "jit_x64_internal.h"
#include "jit_x64_x87_inline.h"

#include <stddef.h>
void emit_epilogue(X86pEmit *e, uint32_t next_eip, X86pJitExit exit) {
  x86p_emit_store32_imm(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), next_eip);
  x86p_emit_mov_r32_imm32(e, kX64Rax, (uint32_t)exit);
  x86p_jit_abi_emit_leave(e, X86P_JIT_HOST_ABI, CPU_REG);
  x86p_emit_ret(e);
}

/* The same exit, but with the guest EIP already computed into a register --
   which is what a conditional branch produces. */
void emit_epilogue_from(X86pEmit *e, X86pHostReg eip_reg, X86pJitExit exit) {
  x86p_emit_store32(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), eip_reg);
  x86p_emit_mov_r32_imm32(e, kX64Rax, (uint32_t)exit);
  x86p_jit_abi_emit_leave(e, X86P_JIT_HOST_ABI, CPU_REG);
  x86p_emit_ret(e);
}

/*
 * The exit to the guest EIP in EAX (zero-extended), through slot `slot` of the
 * block's chain: RCX holds the run header, which the slots follow.
 *
 *   linked, and not the run's stop, and budget left -> jmp [slot.host]
 *   otherwise                                      -> return, naming the slot
 */
static void emit_chained_exit(BlockCtx *c, int64_t slot) {
  X86pEmit *e = c->e;
  const int32_t disp = x86p_jit_chain_slot_disp(c->chain, slot);
  X86pEmitSite unlinked[3];
  unsigned i;

  x86p_emit_mov_r64_imm64(e, kX64Rcx, (uint64_t)x86p_jit_chain_base(c->chain));
  x86p_emit_alu_r64_mem(e, kX64Cmp, kX64Rax, kX64Rcx, disp + (int32_t)offsetof(X86pJitChainSlot, guest));
  unlinked[0] = x86p_emit_jcc_rel32(e, kX86pCondNZ);
  x86p_emit_alu_r32_mem(e, kX64Cmp, kX64Rax, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, stop));
  unlinked[1] = x86p_emit_jcc_rel32(e, kX86pCondZ);
  x86p_emit_dec_m64(e, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, budget));
  unlinked[2] = x86p_emit_jcc_rel32(e, kX86pCondZ);
  x86p_emit_store32(e, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, last), kX64Rax);
  x86p_emit_jmp_m64(e, kX64Rcx, disp + (int32_t)offsetof(X86pJitChainSlot, host));
  for (i = 0; i < 3u; i++) {
    x86p_emit_bind(e, unlinked[i]);
  }
  x86p_emit_store32_imm(e, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, pending), (uint32_t)slot + 1u);
  emit_epilogue_from(e, kX64Rax, kX86pJitExitBlockEnd);
}

void emit_exit_from(BlockCtx *c, X86pHostReg eip_reg) {
  const int64_t slot = c->chain ? x86p_jit_chain_claim(c->chain) : -1;
  if (slot < 0) {
    c->chain_exits_unslotted += c->chain != NULL;
    emit_epilogue_from(c->e, eip_reg, kX86pJitExitBlockEnd);
    return;
  }
  c->chain_exits++;
  if (eip_reg != kX64Rax) {
    x86p_emit_mov_r32_r32(c->e, kX64Rax, eip_reg);
  }
  emit_chained_exit(c, slot);
}

void emit_exit(BlockCtx *c, uint32_t next_eip) {
  if (!c->chain) {
    emit_epilogue(c->e, next_eip, kX86pJitExitBlockEnd);
    return;
  }
  x86p_emit_mov_r32_imm32(c->e, kX64Rax, next_eip);
  emit_exit_from(c, kX64Rax);
}

void emit_block_end(BlockCtx *c, uint32_t next_eip, X86pJitExit exit) {
  if (exit == kX86pJitExitBlockEnd) {
    emit_exit(c, next_eip);
  } else {
    emit_epilogue(c->e, next_eip, exit);
  }
}

size_t x86p_jit_chain_entry_offset(void) {
  uint8_t prologue[32];
  X86pEmit e;
  x86p_emit_init(&e, prologue, sizeof prologue);
  x86p_jit_abi_emit_enter(&e, X86P_JIT_HOST_ABI, CPU_REG);
  return e.len;
}

/* EAX nonzero: to `taken`; zero: to `not_taken`. Two exits rather than one
   exit to a selected address, so each keeps its own slot. */
void emit_two_way_exit(BlockCtx *c, uint32_t taken, uint32_t not_taken) {
  X86pEmitSite branch;
  x86p_emit_test_r32_r32(c->e, kX64Rax, kX64Rax);
  branch = x86p_emit_jcc_rel32(c->e, kX86pCondNZ);
  emit_exit(c, not_taken);
  x86p_emit_bind(c->e, branch);
  emit_exit(c, taken);
}

void emit_loop(BlockCtx *c, const X86pInsn *insn, uint32_t target, uint32_t next) {
  const int condition = insn->op == kX86pInsnLoope ? 1 : insn->op == kX86pInsnLoopne ? -1 : 0;
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG1, insn->address_width);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG2, (uint32_t)condition);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cpu_loop);
  x86p_emit_call_r64(c->e, kX64Rax);
  emit_two_way_exit(c, target, next);
}

uint32_t x86p_jit_host_state(void) {
  return x87_inline_host_control();
}

X86pJitExit x86p_jit_enter(const X86pJitBlock *b, X86pCpu *cpu) {
  uint32_t (*fn)(X86pCpu *);
  if (!b || !b->entry || !cpu || b->host_state != x86p_jit_host_state()) {
    return kX86pJitExitUnsupported;
  }
  /* The cast goes through a function-pointer-sized integer because ISO C does
     not define object-to-function pointer conversion; every host this targets
     does, and saying so here keeps the compiler from warning at each call. */
  *(void **)&fn = b->entry;
  return (X86pJitExit)fn(cpu);
}
