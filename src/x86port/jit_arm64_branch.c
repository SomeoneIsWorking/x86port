/* jit_arm64_branch.c -- block entry and exits, and the control transfers that
   end a block. The AArch64 counterpart of jit_x64_branch.c: the same chained
   exits (jit_chain.h), with X0 carrying the guest EIP, X1 the run header, W4
   the exit's pending slot and X2/X3 as scratch -- none of which an exit
   carries a value in, since guest state is in memory at every exit. */
#include "block_cache.h"
#include "jit_arm64_internal.h"
#include "jit_leaf_sites.h"

#include <stddef.h>

void emit_prologue(X86pA64Emit *e) {
  x86p_a64_emit_push_pair(e, CPU_REG, kA64Lr);
  x86p_a64_emit_mov_x_x(e, CPU_REG, kA64X0);
}

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

/* Record a branch to the block's shared return (emit_chain_leave). An
   unbound branch would go anywhere, so a full list refuses the block. */
static void leave_on(BlockCtx *c, X86pA64EmitSite site) {
  if (c->nchain_leaves >= sizeof c->chain_leaves / sizeof c->chain_leaves[0]) {
    c->e->overflow = 1;
    return;
  }
  c->chain_leaves[c->nchain_leaves++] = site;
}

/* The run's stop address and its budget, which every transfer honours, for
   the guest EIP in W0 with X1 holding the run header, clobbering only X2:
   each failure is a branch to the block's shared return. Then records the
   address as the run's last. The decremented budget is stored before the
   branch reads the flags, as x64's `dec [budget]; jz` does. */
static void emit_transfer_checks(BlockCtx *c) {
  X86pA64Emit *e = c->e;
  x86p_a64_emit_load32(e, kA64X2, kA64X1, (int32_t)offsetof(X86pJitChainRun, stop));
  x86p_a64_emit_cmp_w_w(e, kA64X0, kA64X2);
  leave_on(c, x86p_a64_emit_bcc(e, kA64CondEq));
  x86p_a64_emit_load64(e, kA64X2, kA64X1, (int32_t)offsetof(X86pJitChainRun, budget));
  x86p_a64_emit_subs_x_imm(e, kA64X2, 1u);
  x86p_a64_emit_store64(e, kA64X1, (int32_t)offsetof(X86pJitChainRun, budget), kA64X2);
  leave_on(c, x86p_a64_emit_bcc(e, kA64CondEq));
  x86p_a64_emit_store32(e, kA64X1, (int32_t)offsetof(X86pJitChainRun, last), kA64X0);
}

/*
 * The exit to the guest EIP in W0 through slot `slot` of the block's chain.
 *
 *   linked, and not the run's stop, and budget left -> br slot.host
 *   another address, `probe`, with a front array   -> the block's probe
 *   otherwise                                      -> return, naming the slot
 *
 * The slot's address is a constant of its own (X3) rather than a displacement
 * from the run header, which a table of many blocks puts beyond a load's
 * scaled 12-bit reach. Every producer of W0 writes a W register, so X0 is the
 * zero-extended EIP the 64-bit slot compare needs.
 */
static void emit_chained_exit(BlockCtx *c, int64_t slot, int probe) {
  X86pA64Emit *e = c->e;
  const uintptr_t run = x86p_jit_chain_base(c->chain);
  X86pA64EmitSite miss;

  x86p_a64_emit_mov_x_imm64(e, kA64X1, (uint64_t)run);
  x86p_a64_emit_lea64(e, kA64X3, kA64X1, x86p_jit_chain_slot_disp(c->chain, slot));
  x86p_a64_emit_mov_w_imm32(e, kA64X4, (uint32_t)slot + 1u);
  x86p_a64_emit_load64(e, kA64X2, kA64X3, (int32_t)offsetof(X86pJitChainSlot, guest));
  x86p_a64_emit_cmp_x_x(e, kA64X0, kA64X2);
  miss = x86p_a64_emit_bcc(e, kA64CondNe);
  emit_transfer_checks(c);
  x86p_a64_emit_load64(e, kA64X2, kA64X3, (int32_t)offsetof(X86pJitChainSlot, host));
  x86p_a64_emit_br(e, kA64X2);
  if (!probe || !x86p_jit_chain_front(c->chain)) {
    leave_on(c, miss);
    return;
  }
  if (c->nchain_probes >= sizeof c->chain_probes / sizeof c->chain_probes[0]) {
    e->overflow = 1; /* an unbound branch would go anywhere; refuse the block */
    return;
  }
  c->chain_probes[c->nchain_probes++] = miss;
}

/*
 * The probe (jit_chain.h, THE PROBE), entered from an exit that missed its
 * slot with the target in W0, the run header in X1 and the exit's slot in W4.
 * Every exit that jumps here is bound first, to its entry.
 */
static void emit_chain_probe(BlockCtx *c) {
  X86pA64Emit *e = c->e;
  unsigned i;
  if (!c->nchain_probes) {
    return;
  }
  for (i = 0; i < c->nchain_probes; i++) {
    x86p_a64_emit_bind(e, c->chain_probes[i]);
  }
  x86p_a64_emit_mov_w_w(e, kA64X2, kA64X0);
  x86p_a64_emit_lsr_w_imm(e, kA64X2, JC_BLOCK_FRONT_SHIFT);
  x86p_a64_emit_alu_w_imm(e, kA64And, kA64X2, JC_BLOCK_FRONT_SLOTS - 1u);
  _Static_assert(sizeof(JcBlockFront) == 16u, "the probe scales the front index by a shift of 4");
  x86p_a64_emit_shl_w_imm(e, kA64X2, 4u);
  x86p_a64_emit_mov_x_imm64(e, kA64X3, (uint64_t)(uintptr_t)x86p_jit_chain_front(c->chain));
  x86p_a64_emit_alu_x_x(e, kA64Add, kA64X2, kA64X3);
  x86p_a64_emit_load64(e, kA64X3, kA64X2, (int32_t)offsetof(JcBlockFront, guest));
  x86p_a64_emit_cmp_x_x(e, kA64X0, kA64X3);
  leave_on(c, x86p_a64_emit_bcc(e, kA64CondNe));
  /* The host goes to X3, which the transfer checks leave alone; they use X2.
     A NULL host marks an address the cache refuses. Rm = 31 is XZR in the
     shifted-register compare. */
  x86p_a64_emit_load64(e, kA64X3, kA64X2, (int32_t)offsetof(JcBlockFront, host));
  x86p_a64_emit_cmp_x_x(e, kA64X3, (X86pA64Reg)31);
  leave_on(c, x86p_a64_emit_bcc(e, kA64CondEq));
  emit_transfer_checks(c);
  x86p_a64_emit_store32(e, kA64X1, (int32_t)offsetof(X86pJitChainRun, pending), (X86pA64Reg)31); /* Rt = 31 is WZR */
  x86p_a64_emit_alu_x_imm(e, kA64Add, kA64X3, (uint32_t)x86p_jit_chain_entry_offset());
  x86p_a64_emit_br(e, kA64X3);
}

/* The one return every chained exit's failure shares: the slot in W4 named
   as pending, the guest EIP in W0 stored, and back to the dispatcher. */
static void emit_chain_leave(BlockCtx *c) {
  unsigned i;
  if (!c->nchain_leaves) {
    return;
  }
  for (i = 0; i < c->nchain_leaves; i++) {
    x86p_a64_emit_bind(c->e, c->chain_leaves[i]);
  }
  x86p_a64_emit_store32(c->e, kA64X1, (int32_t)offsetof(X86pJitChainRun, pending), kA64X4);
  emit_epilogue_from(c->e, kA64X0, kX86pJitExitBlockEnd);
}

static void emit_slot_exit(BlockCtx *c, X86pA64Reg eip_reg, int probe) {
  const int64_t slot = c->chain ? x86p_jit_chain_claim(c->chain) : -1;
  if (slot < 0) {
    c->chain_exits_unslotted += c->chain != NULL;
    emit_epilogue_from(c->e, eip_reg, kX86pJitExitBlockEnd);
    return;
  }
  c->chain_exits++;
  if (eip_reg != kA64X0) {
    x86p_a64_emit_mov_w_w(c->e, kA64X0, eip_reg);
  }
  emit_chained_exit(c, slot, probe);
}

void emit_exit_from(BlockCtx *c, X86pA64Reg eip_reg) {
  emit_slot_exit(c, eip_reg, 1);
}

void emit_exit(BlockCtx *c, uint32_t next_eip) {
  if (!c->chain) {
    emit_epilogue(c->e, next_eip, kX86pJitExitBlockEnd);
    return;
  }
  x86p_a64_emit_mov_w_imm32(c->e, kA64X0, next_eip);
  emit_slot_exit(c, kA64X0, 0);
}

void emit_block_end(BlockCtx *c, uint32_t next_eip, X86pJitExit exit) {
  if (exit == kX86pJitExitBlockEnd) {
    emit_exit(c, next_eip);
  } else {
    emit_epilogue(c->e, next_eip, exit);
  }
}

void emit_exits_on(BlockCtx *c, X86pA64Cond cc, uint32_t taken, uint32_t not_taken) {
  const X86pA64EmitSite branch = x86p_a64_emit_bcc(c->e, cc);
  emit_exit(c, not_taken);
  x86p_a64_emit_bind(c->e, branch);
  emit_exit(c, taken);
}

/* The leaf is entered as the callee would be, EIP included; guest state is
   in memory, since the CALL ends the block. Taken: on at the return address;
   declined: the ordinary CALL, to the target. */
static void emit_leaf_call(BlockCtx *c, X86pJitLeafFn leaf, uint32_t return_eip, uint32_t target) {
  x86p_a64_emit_store32_imm(c->e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), target);
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  emit_call(c->e, (void *)leaf);
  c->leaf_calls++;
  x86p_a64_emit_tst_w_w(c->e, kA64X0, kA64X0);
  emit_exits_on(c, kA64CondNe, return_eip, target);
}

void emit_call_exit(BlockCtx *c, uint32_t return_eip, uint32_t target) {
  const X86pJitLeafFn leaf = c->leaf ? c->leaf(target, c->leaf_user) : NULL;
  if (leaf) {
    emit_leaf_call(c, leaf, return_eip, target);
    return;
  }
  emit_exit(c, target);
}

/*
 * A CALL through a register or memory with a leaf site (jit_leaf_sites.h), its
 * return address pushed and its target in TARGET_REG; x86-64's emit_leaf_site
 * is the same sequence, with the refill out of line in the tail:
 *
 *       x2 = site; if target != site.target: refill (in the tail)
 *       x3 = site.leaf; if none, the ordinary exit
 *       [cpu.eip] = target
 *   call_leaf:                          ; x3 = the leaf
 *       taken = x3(cpu): exit to the return address; declined: reload
 *   reload:
 *       target = [cpu.eip]              ; a call clobbered TARGET_REG
 *   ordinary:
 *       the ordinary exit
 */
static void emit_leaf_site(BlockCtx *c, X86pJitLeafSite *site, uint32_t return_eip) {
  X86pA64Emit *e = c->e;
  const int32_t eip_at = (int32_t)offsetof(X86pCpu, eip);
  X86pA64EmitSite reload;
  X86pA64EmitSite ordinary;

  x86p_a64_emit_mov_x_imm64(e, kA64X2, (uint64_t)(uintptr_t)site);
  x86p_a64_emit_load32(e, kA64X3, kA64X2, (int32_t)offsetof(X86pJitLeafSite, target));
  x86p_a64_emit_cmp_w_w(e, TARGET_REG, kA64X3);
  c->site_miss = x86p_a64_emit_bcc(e, kA64CondNe);
  x86p_a64_emit_load64(e, kA64X3, kA64X2, (int32_t)offsetof(X86pJitLeafSite, leaf));
  x86p_a64_emit_cmp_x_x(e, kA64X3, (X86pA64Reg)31); /* Rm = 31 is XZR here */
  ordinary = x86p_a64_emit_bcc(e, kA64CondEq);
  x86p_a64_emit_store32(e, CPU_REG, eip_at, TARGET_REG);

  c->site_call_leaf = e->len;
  x86p_a64_emit_mov_x_x(e, kA64X0, CPU_REG);
  x86p_a64_emit_blr(e, kA64X3);
  x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
  reload = x86p_a64_emit_bcc(e, kA64CondEq); /* declined */
  emit_exit(c, return_eip);

  x86p_a64_emit_bind(e, reload);
  c->site_reload = e->len;
  x86p_a64_emit_load32(e, TARGET_REG, CPU_REG, eip_at);
  x86p_a64_emit_bind(e, ordinary);
  c->site_ordinary = e->len;
  emit_exit_from(c, TARGET_REG);
  c->site_refill = site;
}

/* The site's refill, entered on a miss with X2 still the site:
 *
 *       spent refills: ordinary
 *       [cpu.eip] = target; x3 = fill(site, target)
 *       a leaf: call_leaf; none: reload
 */
static void emit_site_refill(BlockCtx *c) {
  X86pA64Emit *e = c->e;
  if (!c->site_refill) {
    return;
  }
  x86p_a64_emit_bind(e, c->site_miss);
  x86p_a64_emit_load32(e, kA64X3, kA64X2, (int32_t)offsetof(X86pJitLeafSite, refills));
  x86p_a64_emit_cmp_w_imm(e, kA64X3, X86P_JIT_LEAF_SITE_REFILLS);
  x86p_a64_emit_bind_to(e, x86p_a64_emit_bcc(e, kA64CondCs), c->site_ordinary);
  x86p_a64_emit_store32(e, CPU_REG, (int32_t)offsetof(X86pCpu, eip), TARGET_REG);
  x86p_a64_emit_mov_x_x(e, kA64X0, kA64X2);
  x86p_a64_emit_mov_w_w(e, kA64X1, TARGET_REG);
  emit_call(e, (void *)&x86p_jit_leaf_site_fill);
  x86p_a64_emit_mov_x_x(e, kA64X3, kA64X0);
  x86p_a64_emit_cmp_x_x(e, kA64X3, (X86pA64Reg)31);
  x86p_a64_emit_bind_to(e, x86p_a64_emit_bcc(e, kA64CondNe), c->site_call_leaf);
  x86p_a64_emit_bind_to(e, x86p_a64_emit_b(e), c->site_reload);
}

void emit_tail_routines(BlockCtx *c) {
  emit_site_refill(c);
  emit_chain_probe(c);
  emit_chain_leave(c);
}

void emit_call_indirect_exit(BlockCtx *c, uint32_t return_eip) {
  X86pJitLeafSite *const site = c->leaf_sites ? x86p_jit_leaf_sites_claim(c->leaf_sites) : NULL;
  if (site) {
    c->leaf_site_count++;
    emit_leaf_site(c, site, return_eip);
    return;
  }
  emit_exit_from(c, TARGET_REG);
}

/* A block ends at its first conditional branch: two exits. */
unsigned x86p_jit_chain_slots_per_block(void) {
  return 2u;
}

/* A transfer is a BR, in the frame the dispatcher's call opened. */
uint64_t x86p_jit_chain_transfer_limit(void) {
  return UINT64_MAX;
}

size_t x86p_jit_chain_entry_offset(void) {
  uint8_t prologue[16];
  X86pA64Emit e;
  x86p_a64_emit_init(&e, prologue, sizeof prologue);
  emit_prologue(&e);
  return e.len;
}

void emit_loop(BlockCtx *c, const X86pInsn *insn, uint32_t target, uint32_t next) {
  const int condition = insn->op == kX86pInsnLoope ? 1 : insn->op == kX86pInsnLoopne ? -1 : 0;
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, insn->address_width);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, (uint32_t)condition);
  x86p_a64_emit_mov_x_imm64(c->e, kA64X9, (uint64_t)(uintptr_t)&x86p_cpu_loop);
  x86p_a64_emit_blr(c->e, kA64X9);
  x86p_a64_emit_tst_w_w(c->e, kA64X0, kA64X0);
  emit_exits_on(c, kA64CondNe, target, next);
}
