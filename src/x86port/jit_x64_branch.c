/* jit_x64_branch.c -- block entry and exits, and the control transfers that end
   a block. */
#include "block_cache.h"
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

/* The run's stop address and its budget, which every transfer honours, for
   the guest EIP in EAX with RCX holding the run header: each failure is a jump
   to the caller's return, stored into `leave`. Then records the address as
   the run's last. */
static void emit_transfer_checks(X86pEmit *e, X86pEmitSite leave[2]) {
  x86p_emit_alu_r32_mem(e, kX64Cmp, kX64Rax, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, stop));
  leave[0] = x86p_emit_jcc_rel32(e, kX86pCondZ);
  x86p_emit_dec_m64(e, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, budget));
  leave[1] = x86p_emit_jcc_rel32(e, kX86pCondZ);
  x86p_emit_store32(e, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, last), kX64Rax);
}

/*
 * The exit to the guest EIP in EAX (zero-extended), through slot `slot` of the
 * block's chain: RCX holds the run header, which the slots follow.
 *
 *   linked, and not the run's stop, and budget left -> jmp [slot.host]
 *   another address, `probe`, with a front array   -> the block's probe
 *   otherwise                                      -> return, naming the slot
 *
 * Only an exit whose target varies probes: a direct exit's slot is linked
 * once and then always names its one target, and a probe per block was 30%
 * more translated code on the Dead Zone route.
 */
static void emit_chained_exit(BlockCtx *c, int64_t slot, int probe) {
  X86pEmit *e = c->e;
  const int32_t disp = x86p_jit_chain_slot_disp(c->chain, slot);
  const int32_t pending = (int32_t)offsetof(X86pJitChainRun, pending);
  X86pEmitSite miss;
  X86pEmitSite leave[2];

  x86p_emit_mov_r64_imm64(e, kX64Rcx, (uint64_t)x86p_jit_chain_base(c->chain));
  x86p_emit_alu_r64_mem(e, kX64Cmp, kX64Rax, kX64Rcx, disp + (int32_t)offsetof(X86pJitChainSlot, guest));
  miss = x86p_emit_jcc_rel32(e, kX86pCondNZ);
  emit_transfer_checks(e, leave);
  x86p_emit_jmp_m64(e, kX64Rcx, disp + (int32_t)offsetof(X86pJitChainSlot, host));
  if (probe && x86p_jit_chain_front(c->chain)) {
    x86p_emit_bind(e, miss);
    x86p_emit_store32_imm(e, kX64Rcx, pending, (uint32_t)slot + 1u);
    if (c->nchain_probes >= sizeof c->chain_probes / sizeof c->chain_probes[0]) {
      e->overflow = 1; /* an unbound jump would go anywhere; refuse the block */
      return;
    }
    c->chain_probes[c->nchain_probes++] = x86p_emit_jmp_rel32(e);
  } else {
    x86p_emit_bind(e, miss);
  }
  x86p_emit_bind(e, leave[0]);
  x86p_emit_bind(e, leave[1]);
  x86p_emit_store32_imm(e, kX64Rcx, pending, (uint32_t)slot + 1u);
  emit_epilogue_from(e, kX64Rax, kX86pJitExitBlockEnd);
}

/*
 * The probe (jit_chain.h, THE PROBE), entered from an exit that missed its
 * slot with the target in EAX, the run header in RCX and the exit's slot
 * already named as pending. Clobbers RDX and RSI, which no exit carries.
 */
static void emit_chain_probe(BlockCtx *c) {
  X86pEmit *e = c->e;
  const size_t chain_entry = x86p_jit_chain_entry_offset();
  X86pEmitSite leave[5];
  size_t entry;
  unsigned i;
  if (!c->nchain_probes) {
    return;
  }
  if (chain_entry > 127u) {
    e->overflow = 1; /* the add below takes a sign-extended byte */
    return;
  }
  entry = x86p_emit_here(e);
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rax, c->entry_eip);
  leave[0] = x86p_emit_jcc_rel32(e, kX86pCondZ);
  x86p_emit_mov_r32_r32(e, kX64Rdx, kX64Rax);
  x86p_emit_shift_r32_imm8(e, kX64Shr, kX64Rdx, JC_BLOCK_FRONT_SHIFT);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rdx, JC_BLOCK_FRONT_SLOTS - 1u);
  _Static_assert(sizeof(JcBlockFront) == 16u, "the probe scales the front index by a shift of 4");
  x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rdx, 4u);
  x86p_emit_mov_r64_imm64(e, kX64Rsi, (uint64_t)(uintptr_t)x86p_jit_chain_front(c->chain));
  x86p_emit_alu_r64_r64(e, kX64Add, kX64Rdx, kX64Rsi);
  x86p_emit_alu_r64_mem(e, kX64Cmp, kX64Rax, kX64Rdx, (int32_t)offsetof(JcBlockFront, guest));
  leave[1] = x86p_emit_jcc_rel32(e, kX86pCondNZ);
  x86p_emit_load64(e, kX64Rdx, kX64Rdx, (int32_t)offsetof(JcBlockFront, host));
  x86p_emit_alu_r64_imm8(e, kX64Cmp, kX64Rdx, 0); /* a mark: the address is refused */
  leave[2] = x86p_emit_jcc_rel32(e, kX86pCondZ);
  emit_transfer_checks(e, leave + 3);
  x86p_emit_store32_imm(e, kX64Rcx, (int32_t)offsetof(X86pJitChainRun, pending), 0u);
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rdx, (int8_t)chain_entry);
  x86p_emit_jmp_r64(e, kX64Rdx);
  for (i = 0; i < 5u; i++) {
    x86p_emit_bind(e, leave[i]);
  }
  emit_epilogue_from(e, kX64Rax, kX86pJitExitBlockEnd);
  for (i = 0; i < c->nchain_probes; i++) {
    x86p_emit_bind_to(e, c->chain_probes[i], entry);
  }
}

void emit_tail_routines(BlockCtx *c) {
  emit_chain_probe(c);
  x87_cache_emit_routines(c);
}

static void emit_slot_exit(BlockCtx *c, X86pHostReg eip_reg, int probe) {
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
  emit_chained_exit(c, slot, probe);
}

void emit_exit_from(BlockCtx *c, X86pHostReg eip_reg) {
  emit_slot_exit(c, eip_reg, 1);
}

void emit_exit(BlockCtx *c, uint32_t next_eip) {
  if (!c->chain) {
    emit_epilogue(c->e, next_eip, kX86pJitExitBlockEnd);
    return;
  }
  x86p_emit_mov_r32_imm32(c->e, kX64Rax, next_eip);
  emit_slot_exit(c, kX64Rax, 0);
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
