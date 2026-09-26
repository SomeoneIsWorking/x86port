/*
 * jit_arm64_cond.c -- lowering an x86 condition code onto AArch64's own NZCV,
 * and the two instructions that consume one: Jcc and SETcc.
 *
 * Split out of jit_arm64.c because this is one responsibility with one
 * question to answer -- "can this condition be read off the host flags, and
 * as which condition code" -- and because it is the half of the backend that
 * restates a derivation owned elsewhere (flags.c) and therefore has to be
 * read against it.
 */
#include "jit_arm64_cond.h"

#include "cond.h"
#include "flags.h"
#include "jit_arm64_internal.h"

/*
 * INLINE CONDITION EVALUATION.
 *
 * x86p_cond is the one authority on what a condition means, and calling it is
 * always correct -- which is exactly why this needs saying out loud: a change
 * that lost the inline form would pass every differential comparison and
 * merely be slower. Measured on the arm64 Android build, x86p_cond was 59
 * samples of the port's own thread against 424 in all emitted code, because
 * every Jcc and SETcc paid a BLR into it.
 *
 * The saving exists because AArch64's NZCV is defined the same way x86's
 * arithmetic flags are, for the kinds below. What it costs is that the
 * mapping is a SECOND statement of the derivations in flags.c, so each arm
 * below names the flags.c rule it mirrors and the differential tests against
 * the interpreter are what hold the two together.
 *
 * The flag operands are in the X86pFlags struct in guest state, not in host
 * registers: dead-flag elimination only skips those stores when nothing reads
 * them, and a Jcc reading them is precisely the case where they were stored.
 *
 * A narrower operation's flags come from its masked value, so its operands are
 * LEFT-ALIGNED into the top of the word first: shifting a and b left by
 * 32 - 8w puts the guest's sign bit in bit 31 and zeros below it, and the
 * 32-bit compare then produces exactly the narrow operation's N, Z, C and V.
 * The shift also discards whatever the caller left above the width -- flags.c
 * masks on read rather than on store, so those bits are not guaranteed to be
 * clean -- which is why the alignment is done on both operands and not just
 * assumed away.
 */

/* After `cmp w(a), w(b)`: AArch64 NZCV equals what x86 SUB/CMP wrote, flag for
   flag -- V is the signed-overflow rule flags.c states for kX86pFlagsSub, N is
   msb(r), Z is r == 0, and x86's CF (borrow) is AArch64's !C, i.e. LO. */
static int cond_after_cmp_sub(uint8_t cond, X86pA64Cond *out) {
  switch ((X86pCond)cond) {
  case kX86pCondO:
    *out = kA64CondVs;
    return 1;
  case kX86pCondNO:
    *out = kA64CondVc;
    return 1;
  case kX86pCondB:
    *out = kA64CondCc;
    return 1;
  case kX86pCondNB:
    *out = kA64CondCs;
    return 1;
  case kX86pCondZ:
    *out = kA64CondEq;
    return 1;
  case kX86pCondNZ:
    *out = kA64CondNe;
    return 1;
  case kX86pCondBE:
    *out = kA64CondLs;
    return 1;
  case kX86pCondA:
    *out = kA64CondHi;
    return 1;
  case kX86pCondS:
    *out = kA64CondMi;
    return 1;
  case kX86pCondNS:
    *out = kA64CondPl;
    return 1;
  case kX86pCondL:
    *out = kA64CondLt;
    return 1;
  case kX86pCondGE:
    *out = kA64CondGe;
    return 1;
  case kX86pCondLE:
    *out = kA64CondLe;
    return 1;
  case kX86pCondG:
    *out = kA64CondGt;
    return 1;
  /* P and NP never reach here: x86p_a64_emit_condition_flags folds the
     result's low byte for them before consulting the kind. */
  default:
    return 0;
  }
}

/*
 * After `cmn w(a), w(b)`: AArch64 NZCV equals what x86 ADD wrote. This is NOT
 * the SUB table with different names -- for ADD, x86's CF is the carry OUT and
 * equals AArch64's C directly, where for SUB it is a borrow and equals !C. So
 * B maps to CS here and to CC there; getting that backwards is a divergence
 * only the differential would catch.
 *
 * BE (CF||ZF) and A (!CF&&!ZF) have no AArch64 condition: HI and LS encode
 * C&&!Z and !C||Z, which are the SUB-sense pair. Both refuse rather than
 * emitting a second comparison to build them.
 */
static int cond_after_cmn_add(uint8_t cond, X86pA64Cond *out) {
  switch ((X86pCond)cond) {
  case kX86pCondO:
    *out = kA64CondVs;
    return 1;
  case kX86pCondNO:
    *out = kA64CondVc;
    return 1;
  case kX86pCondB:
    *out = kA64CondCs;
    return 1;
  case kX86pCondNB:
    *out = kA64CondCc;
    return 1;
  case kX86pCondZ:
    *out = kA64CondEq;
    return 1;
  case kX86pCondNZ:
    *out = kA64CondNe;
    return 1;
  case kX86pCondS:
    *out = kA64CondMi;
    return 1;
  case kX86pCondNS:
    *out = kA64CondPl;
    return 1;
  case kX86pCondL:
    *out = kA64CondLt;
    return 1;
  case kX86pCondGE:
    *out = kA64CondGe;
    return 1;
  case kX86pCondLE:
    *out = kA64CondLe;
    return 1;
  case kX86pCondG:
    *out = kA64CondGt;
    return 1;
  default:
    return 0;
  }
}

/*
 * After `cmp w(r), #0` for kX86pFlagsLogic: flags.c clears CF and OF for the
 * logic kind, and CMP against zero leaves C set and V clear -- so the two
 * conditions that read CF or OF alone are constants here, and the signed four
 * collapse onto the unsigned ones because V is 0 either way.
 */
static int cond_after_cmp_logic(uint8_t cond, X86pA64Cond *out, int *constant) {
  *constant = -1;
  switch ((X86pCond)cond) {
  case kX86pCondO:
    *constant = 0;
    return 1; /* OF is cleared */
  case kX86pCondNO:
    *constant = 1;
    return 1;
  case kX86pCondB:
    *constant = 0;
    return 1; /* CF is cleared */
  case kX86pCondNB:
    *constant = 1;
    return 1;
  case kX86pCondZ:
    *out = kA64CondEq;
    return 1;
  case kX86pCondNZ:
    *out = kA64CondNe;
    return 1;
  case kX86pCondBE:
    *out = kA64CondEq;
    return 1; /* CF||ZF, CF == 0 */
  case kX86pCondA:
    *out = kA64CondNe;
    return 1; /* !CF&&!ZF */
  case kX86pCondS:
    *out = kA64CondMi;
    return 1;
  case kX86pCondNS:
    *out = kA64CondPl;
    return 1;
  case kX86pCondL:
    *out = kA64CondMi;
    return 1; /* SF!=OF, OF == 0 */
  case kX86pCondGE:
    *out = kA64CondPl;
    return 1;
  case kX86pCondLE:
    *out = kA64CondLe;
    return 1; /* ZF||SF, V == 0 */
  case kX86pCondG:
    *out = kA64CondGt;
    return 1;
  default:
    return 0;
  }
}

/*
 * INC and DEC preserve CF and give OF its own rule in flags.c -- "wrapped to
 * the most negative value", which is not the generic add/sub overflow -- and
 * a shift's CF and OF depend on its count, so only the conditions that read
 * neither are inlined here. That still covers the idioms `dec ecx; jnz` and
 * `shr eax, 1; jz`, which is why the kinds are worth a case at all.
 */
static int cond_after_cmp_result(uint8_t cond, X86pA64Cond *out) {
  switch ((X86pCond)cond) {
  case kX86pCondZ:
    *out = kA64CondEq;
    return 1;
  case kX86pCondNZ:
    *out = kA64CondNe;
    return 1;
  case kX86pCondS:
    *out = kA64CondMi;
    return 1;
  case kX86pCondNS:
    *out = kA64CondPl;
    return 1;
  default:
    return 0;
  }
}

/*
 * PF, flags.c x86p_flag_pf: the parity of the result's LOW BYTE, at every
 * width, for every kind but Explicit and None. AArch64 has no parity flag, so
 * the byte is folded onto bit 0 -- three EORs with the value shifted right by
 * 4, 2 and 1 leave bit 0 the XOR of bits 0..7 and nothing above them -- and
 * `tst #1` sets Z exactly when that XOR is 0, i.e. when the byte has an even
 * number of ones, which is PF = 1.
 *
 * This is not the rare condition it looks like: MSVC compares floats with
 * `fnstsw ax; test ah, imm; jp`, so it is almost every JP/JNP in the title,
 * and every one of them was a call into x86p_cond.
 */
static int kind_has_result_parity(int kind) {
  switch (kind) {
  case (int)kX86pFlagsSub:
  case (int)kX86pFlagsAdd:
  case (int)kX86pFlagsLogic:
  case (int)kX86pFlagsInc:
  case (int)kX86pFlagsDec:
  case (int)kX86pFlagsShl:
  case (int)kX86pFlagsShr:
  case (int)kX86pFlagsSar:
    return 1;
  default:
    return 0;
  }
}

static void emit_result_parity(X86pA64Emit *e, int r_reg) {
  X86pA64Reg r = kA64X0;
  if (r_reg == X86P_A64_FLAG_IN_MEMORY) {
    x86p_a64_emit_load32(e, kA64X0, CPU_REG, FLAG_R);
  } else {
    r = (X86pA64Reg)r_reg;
  }
  x86p_a64_emit_eor_w_w_lsr(e, kA64X0, r, r, 4u);
  x86p_a64_emit_eor_w_w_lsr(e, kA64X0, kA64X0, kA64X0, 2u);
  x86p_a64_emit_eor_w_w_lsr(e, kA64X0, kA64X0, kA64X0, 1u);
  x86p_a64_emit_tst_w_bit0(e, kA64X0);
}

/* Put a flag operand in `reg`, left-aligned for the recorded width: from the
   host register the writer left it in, or loaded from the CPU state. */
static void load_aligned(X86pA64Emit *e, X86pA64Reg reg, int32_t offset, int held_in, uint8_t shift) {
  if (held_in != X86P_A64_FLAG_IN_MEMORY) {
    x86p_a64_emit_shl_w_w_imm(e, reg, (X86pA64Reg)held_in, shift);
    return;
  }
  x86p_a64_emit_load32(e, reg, CPU_REG, offset);
  if (shift) {
    x86p_a64_emit_shl_w_imm(e, reg, shift);
  }
}

/* a into X0 and b into X1, ordered so neither overwrites the other's source
   first; a swapped pair (a in X1, b in X0) is reloaded from memory. */
static void load_operands_aligned(X86pA64Emit *e, const X86pA64FlagRegs *regs, uint8_t shift) {
  int a = regs->a;
  const int b = regs->b;
  if (a == (int)kA64X1 && b == (int)kA64X0) {
    a = X86P_A64_FLAG_IN_MEMORY;
  }
  if (b == (int)kA64X0) {
    load_aligned(e, kA64X1, FLAG_B, b, shift);
    load_aligned(e, kA64X0, FLAG_A, a, shift);
    return;
  }
  load_aligned(e, kA64X0, FLAG_A, a, shift);
  load_aligned(e, kA64X1, FLAG_B, b, shift);
}

int x86p_a64_emit_condition_flags(X86pA64Emit *e,
                                  uint8_t cond,
                                  int last_kind,
                                  int last_w,
                                  const X86pA64FlagRegs *regs,
                                  X86pA64Cond *out_cc,
                                  int *out_constant) {
  X86pA64Cond cc = kA64CondAl;
  int constant = -1;
  uint8_t shift;
  X86pA64FlagRegs in_memory;

  *out_constant = -1;
  if (!regs) {
    in_memory = x86p_a64_flags_in_memory();
    regs = &in_memory;
  }

  if (last_w != 1 && last_w != 2 && last_w != 4) {
    return 0;
  }
  if (cond == (uint8_t)kX86pCondP || cond == (uint8_t)kX86pCondNP) {
    if (!kind_has_result_parity(last_kind)) {
      return 0;
    }
    emit_result_parity(e, regs->r);
    *out_cc = cond == (uint8_t)kX86pCondP ? kA64CondEq : kA64CondNe;
    return 1;
  }
  shift = (uint8_t)(32 - 8 * last_w);
  switch (last_kind) {
  case (int)kX86pFlagsSub:
    if (!cond_after_cmp_sub(cond, &cc)) {
      return 0;
    }
    load_operands_aligned(e, regs, shift);
    x86p_a64_emit_cmp_w_w(e, kA64X0, kA64X1);
    break;
  case (int)kX86pFlagsAdd:
    if (!cond_after_cmn_add(cond, &cc)) {
      return 0;
    }
    load_operands_aligned(e, regs, shift);
    x86p_a64_emit_cmn_w_w(e, kA64X0, kA64X1);
    break;
  case (int)kX86pFlagsLogic:
    if (!cond_after_cmp_logic(cond, &cc, &constant)) {
      return 0;
    }
    if (constant >= 0) {
      *out_constant = constant;
      return 1;
    }
    load_aligned(e, kA64X0, FLAG_R, regs->r, shift);
    x86p_a64_emit_cmp_w_imm(e, kA64X0, 0u);
    break;
  case (int)kX86pFlagsInc:
  case (int)kX86pFlagsDec:
  case (int)kX86pFlagsShl:
  case (int)kX86pFlagsShr:
  case (int)kX86pFlagsSar:
    if (!cond_after_cmp_result(cond, &cc)) {
      return 0;
    }
    load_aligned(e, kA64X0, FLAG_R, regs->r, shift);
    x86p_a64_emit_cmp_w_imm(e, kA64X0, 0u);
    break;
  default:
    return 0;
  }
  *out_cc = cc;
  return 1;
}

/* ---- the consumers ---------------------------------------------------- */

static void emit_condition_value(BlockCtx *c, uint8_t cond, int last_kind, int last_w) {
  X86pA64Cond cc;
  int constant;
  if (x86p_a64_emit_condition_flags(c->e, cond, last_kind, last_w, &c->flag_regs_in, &cc, &constant)) {
    c->cond_inline++;
    if (constant >= 0) {
      x86p_a64_emit_mov_w_imm32(c->e, kA64X0, (uint32_t)constant);
    } else {
      x86p_a64_emit_cset_w(c->e, cc, kA64X0);
    }
    return;
  }
  c->cond_helper_calls++;
  if (last_kind < 0) {
    c->cond_unknown_kind++;
  }
  x86p_a64_emit_mov_w_imm32(c->e, kA64X0, (uint32_t)cond);
  x86p_a64_emit_lea64(c->e, kA64X1, CPU_REG, flags_off());
  emit_call(c->e, (void *)&x86p_cond);
}

void emit_jcc(BlockCtx *c, uint8_t cond, uint32_t target, uint32_t fallthrough, int last_kind, int last_w) {
  X86pA64Emit *e = c->e;
  X86pA64Cond cc;
  int constant;
  c->conds++;
  /*
   * A branch selects between two exits, so the inline form reads the host
   * condition DIRECTLY with a b.cc -- materialising 0/1 with a cset and then
   * testing it again would be three instructions to say what one already says.
   * A condition that is constant for the kind (CF and OF after a logic
   * operation) picks its successor here, at translation time.
   */
  if (x86p_a64_emit_condition_flags(e, cond, last_kind, last_w, &c->flag_regs_in, &cc, &constant)) {
    c->cond_inline++;
    if (constant >= 0) {
      emit_exit(c, constant ? target : fallthrough);
    } else {
      emit_exits_on(c, cc, target, fallthrough);
    }
    return;
  }
  c->cond_helper_calls++;
  if (last_kind < 0) {
    c->cond_unknown_kind++;
  }
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)cond);
  x86p_a64_emit_lea64(e, kA64X1, CPU_REG, flags_off());
  emit_call(e, (void *)&x86p_cond);
  x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
  emit_exits_on(c, kA64CondNe, target, fallthrough);
}

/* SETcc materialises the canonical condition evaluator's 0/1 result without
   touching guest flags. A memory destination computes the condition first,
   then preserves it in CARRY_REG while the shared address/bounds path uses
   X0. */
void emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int last_kind, int last_w) {
  const X86pOperand *dst = &insn->operand[0];

  c->conds++;
  emit_condition_value(c, insn->cond, last_kind, last_w);
  if (dst->kind == kX86pOperandMem) {
    x86p_a64_emit_mov_w_w(c->e, CARRY_REG, kA64X0);
    emit_mem_prepare_w(c, dst, insn_eip, 1);
    x86p_a64_emit_store8_reg(c->e, HOSTPTR_REG, 0, CARRY_REG);
    return;
  }
  x86p_a64_emit_store8_reg(c->e, CPU_REG, reg_off_w(dst->reg, 1), kA64X0);
}
