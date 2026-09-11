/*
 * jit_arm64_cond.c -- lowering an x86 condition code onto AArch64's own NZCV.
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
 * Width 4 only. A narrower operation's flags come from the masked value, and
 * a 32-bit CMP would take sign and zero from bits the guest operation never
 * wrote.
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
  /* PF is a parity of the low byte. AArch64 has no such flag and computing it
     is more work than the call it would replace. */
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
 * the most negative value", which is not the generic add/sub overflow -- so
 * only the conditions that read neither are inlined here. That still covers
 * the loop idiom `dec ecx; jnz`, which is why the kind is worth a case at all.
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

/* Materialise the condition as 0/1 in X0 without a helper call, or return 0
   having emitted nothing. */
int x86p_a64_emit_condition_inline(X86pA64Emit *e, uint8_t cond, int last_kind, int last_w) {
  X86pA64Cond cc = kA64CondAl;
  int constant = -1;

  if (last_w != 4) {
    return 0;
  }
  switch (last_kind) {
  case (int)kX86pFlagsSub:
    if (!cond_after_cmp_sub(cond, &cc)) {
      return 0;
    }
    x86p_a64_emit_load32(e, kA64X0, CPU_REG, FLAG_A);
    x86p_a64_emit_load32(e, kA64X1, CPU_REG, FLAG_B);
    x86p_a64_emit_cmp_w_w(e, kA64X0, kA64X1);
    break;
  case (int)kX86pFlagsAdd:
    if (!cond_after_cmn_add(cond, &cc)) {
      return 0;
    }
    x86p_a64_emit_load32(e, kA64X0, CPU_REG, FLAG_A);
    x86p_a64_emit_load32(e, kA64X1, CPU_REG, FLAG_B);
    x86p_a64_emit_cmn_w_w(e, kA64X0, kA64X1);
    break;
  case (int)kX86pFlagsLogic:
    if (!cond_after_cmp_logic(cond, &cc, &constant)) {
      return 0;
    }
    if (constant >= 0) {
      x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)constant);
      return 1;
    }
    x86p_a64_emit_load32(e, kA64X0, CPU_REG, FLAG_R);
    x86p_a64_emit_cmp_w_imm(e, kA64X0, 0u);
    break;
  case (int)kX86pFlagsInc:
  case (int)kX86pFlagsDec:
    if (!cond_after_cmp_result(cond, &cc)) {
      return 0;
    }
    x86p_a64_emit_load32(e, kA64X0, CPU_REG, FLAG_R);
    x86p_a64_emit_cmp_w_imm(e, kA64X0, 0u);
    break;
  default:
    return 0;
  }
  x86p_a64_emit_cset_w(e, cc, kA64X0);
  return 1;
}
