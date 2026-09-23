/*
 * jit_x64_cond.c -- Jcc and SETcc for the x64 backend.
 *
 * Split out of jit_x64.c to match jit_arm64_cond.c: condition lowering is one
 * responsibility, and it is where a backend either reads a condition off the
 * host's own flags or pays for the authority.
 *
 * INLINE CONDITION EVALUATION. x86p_cond is the one authority on what a
 * condition means, and calling it is always correct -- which is why losing the
 * inline form would pass every differential and merely be slower. Measured on
 * the X-Men Legends II Dead Zone route before this existed, every one of 33,660
 * translated conditions paid a call into x86p_cond, and the condition and flag
 * accessors together were ~5% of the frame.
 *
 * The host IS an x86, so for the kinds below one host instruction over the
 * recorded operands leaves EFLAGS exactly as flags.c derives them, and the
 * guest condition code is the host condition code. Each arm names the flags.c
 * rule it restates; the block differentials against the interpreter hold the
 * two together.
 *
 *  - A narrow operation's operands are LEFT-ALIGNED first (shifted up by
 *    32 - 8w), so the 32-bit host operation produces the narrow CF, OF, ZF and
 *    SF, and discards whatever flags.c left above the width (it masks on read,
 *    not on store). That zeroes the low byte, so PF is never read from an
 *    aligned value: PF is the parity of the RESULT's low byte at every width.
 *  - The statically tracked predecessor only chooses WHICH lowering to emit.
 *    What executes is guarded by the recorded kind and width in guest state --
 *    one 16-bit compare, since the two bytes are adjacent -- and anything else
 *    takes the helper. A predecessor the translator got wrong is therefore a
 *    missed inline, never a wrong branch.
 */
#include "jit_x64_cond.h"

#include "cond.h"
#include "flags.h"
#include "jit_x64_abi.h"
#include "jit_x64_internal.h"

#include <stddef.h>

/* How a condition is read after one recorded kind. */
typedef enum CondLowering {
  kCondHelper = 0, /* no inline form: call x86p_cond */
  kCondParity,     /* `test r, r`, unaligned: PF of the result's low byte */
  kCondSub,        /* `cmp a, b`, aligned: every flag SUB/CMP wrote */
  kCondAdd,        /* `add a, b`, aligned: every flag ADD wrote */
  kCondResult,     /* `test r, r`, aligned: ZF, SF, and CF = OF = 0 */
  kCondFalse,      /* a flag the kind always clears */
  kCondTrue
} CondLowering;

static CondLowering lowering_for(uint8_t cond, int kind) {
  if (cond == (uint8_t)kX86pCondP || cond == (uint8_t)kX86pCondNP) {
    /* flags.c x86p_flag_pf: parity of r & 0xFF for every non-Explicit kind. */
    switch (kind) {
    case kX86pFlagsSub:
    case kX86pFlagsAdd:
    case kX86pFlagsLogic:
    case kX86pFlagsInc:
    case kX86pFlagsDec:
    case kX86pFlagsShl:
    case kX86pFlagsShr:
    case kX86pFlagsSar:
      return kCondParity;
    default:
      return kCondHelper;
    }
  }
  switch (kind) {
  case kX86pFlagsSub:
    /* flags.c: CF = a < b unsigned, OF = like signs broken, ZF/SF from r. */
    return kCondSub;
  case kX86pFlagsAdd:
    /* flags.c: CF = carry out, OF = like signs that produced the other. */
    return kCondAdd;
  case kX86pFlagsLogic:
    /* flags.c clears CF and OF for the logic kind, which is exactly what a
       host TEST leaves, so every remaining condition reads straight off it. */
    if (cond == (uint8_t)kX86pCondO || cond == (uint8_t)kX86pCondB) {
      return kCondFalse;
    }
    if (cond == (uint8_t)kX86pCondNO || cond == (uint8_t)kX86pCondNB) {
      return kCondTrue;
    }
    return kCondResult;
  case kX86pFlagsInc:
  case kX86pFlagsDec:
  case kX86pFlagsShl:
  case kX86pFlagsShr:
  case kX86pFlagsSar:
    /* INC/DEC preserve CF and a shift's CF and OF depend on its count; only
       ZF and SF, the `dec ecx; jnz` and `shr eax, 1; jz` idioms, read off the
       result. */
    if (cond == (uint8_t)kX86pCondZ || cond == (uint8_t)kX86pCondNZ || cond == (uint8_t)kX86pCondS ||
        cond == (uint8_t)kX86pCondNS) {
      return kCondResult;
    }
    return kCondHelper;
  default:
    return kCondHelper;
  }
}

static void load_aligned(X86pEmit *e, X86pHostReg reg, int32_t offset, int w) {
  x86p_emit_load32(e, reg, CPU_REG, offset);
  if (w != 4) {
    x86p_emit_shift_r32_imm8(e, kX64Shl, reg, (uint8_t)(32 - 8 * w));
  }
}

/* Host EFLAGS for `lowering`, then RAX = the condition as 0 or 1. RCX and RDX
   are scratch here exactly as they are across the helper call. */
static void emit_inline_value(X86pEmit *e, uint8_t cond, CondLowering lowering, int w) {
  switch (lowering) {
  case kCondFalse:
  case kCondTrue:
    x86p_emit_mov_r32_imm32(e, kX64Rax, lowering == kCondTrue ? 1u : 0u);
    return;
  case kCondParity:
    x86p_emit_load32(e, kX64Rcx, CPU_REG, FLAG_R);
    x86p_emit_test_r32_r32(e, kX64Rcx, kX64Rcx);
    break;
  case kCondSub:
  case kCondAdd:
    load_aligned(e, kX64Rcx, FLAG_A, w);
    load_aligned(e, kX64Rdx, FLAG_B, w);
    x86p_emit_alu_r32_r32(e, lowering == kCondSub ? kX64Cmp : kX64Add, kX64Rcx, kX64Rdx);
    break;
  case kCondResult:
    load_aligned(e, kX64Rcx, FLAG_R, w);
    x86p_emit_test_r32_r32(e, kX64Rcx, kX64Rcx);
    break;
  case kCondHelper:
    return;
  }
  /* MOV leaves EFLAGS alone; SETcc then writes only AL. */
  x86p_emit_mov_r32_imm32(e, kX64Rax, 0u);
  x86p_emit_setcc_r8(e, (unsigned)cond, kX64Rax);
}

static void emit_helper_value(X86pEmit *e, uint8_t cond) {
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG0, (uint32_t)cond);
  x86p_emit_lea64(e, X86P_JIT_HOST_ARG1, CPU_REG, flags_off());
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cond);
  x86p_emit_call_r64(e, kX64Rax);
}

/* RAX = the condition as 0 or 1: inline behind the recorded-kind guard when
   the block's predecessor has a lowering, otherwise through x86p_cond. A Jcc
   (`out_of_line`) puts the guard's helper side after the block's exits, off
   the straight-line code; a SETcc, which can repeat within a block, keeps it
   in place so the block tail stays bounded. */
static void emit_condition_value(BlockCtx *c, uint8_t cond, int last_kind, int last_w, int out_of_line) {
  X86pEmit *e = c->e;
  CondLowering lowering = kCondHelper;
  X86pEmitSite slow;

  if (last_kind >= 0 && (last_w == 1 || last_w == 2 || last_w == 4)) {
    lowering = lowering_for(cond, last_kind);
  }
  if (lowering == kCondHelper) {
    /* Counted rather than left at zero, which would read as "classified, and
       none were unknown". */
    c->cond_helper_calls++;
    if (last_kind < 0) {
      c->cond_unknown_kind++;
    }
    emit_helper_value(e, cond);
    return;
  }
  c->cond_inline++;
  x86p_emit_load16_zx(e, kX64Rcx, CPU_REG, flag_kind_off());
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rcx, flag_kind_word((unsigned)last_kind, (unsigned)last_w));
  slow = x86p_emit_jcc_rel32(e, (unsigned)kX86pCondNZ);
  emit_inline_value(e, cond, lowering, last_w);
  if (out_of_line && !c->has_cond_slow) {
    c->has_cond_slow = 1;
    c->cond_slow_guard = slow;
    c->cond_slow_resume = x86p_emit_here(e);
    c->cond_slow_cond = cond;
    return;
  }
  {
    X86pEmitSite done = x86p_emit_jmp_rel32(e);
    x86p_emit_bind(e, slow);
    emit_helper_value(e, cond);
    x86p_emit_bind(e, done);
  }
}

/* The guard's other side for a Jcc: a recorded kind the translator did not
   predict is a missed inline, answered by the authority, never fetched on
   the path every iteration runs. */
void x86p_x64_emit_cond_slow_path(BlockCtx *c) {
  if (!c->has_cond_slow) {
    return;
  }
  x86p_emit_bind(c->e, c->cond_slow_guard);
  emit_helper_value(c->e, c->cond_slow_cond);
  x86p_emit_bind_to(c->e, x86p_emit_jmp_rel32(c->e), c->cond_slow_resume);
}

void x86p_x64_emit_jcc(BlockCtx *c, uint8_t cond, uint32_t target, uint32_t fallthrough, int last_kind, int last_w) {
  c->conds++;
  emit_condition_value(c, cond, last_kind, last_w, 1);
  emit_two_way_exit(c, target, fallthrough);
}

/* SETcc materialises the canonical condition evaluator's 0/1 result without
   touching guest flags. A memory destination computes the condition first,
   then preserves it in RCX while the shared address/bounds path uses RAX. */
void x86p_x64_emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int last_kind, int last_w) {
  const X86pOperand *dst = &insn->operand[0];

  c->conds++;
  emit_condition_value(c, insn->cond, last_kind, last_w, 0);
  if (dst->kind == kX86pOperandMem) {
    x86p_emit_mov_r32_r32(c->e, CARRY_REG, kX64Rax);
    emit_mem_prepare_w(c, dst, insn_eip, 1);
    x86p_emit_store8_reg(c->e, HOSTPTR_REG, 0, CARRY_REG);
    return;
  }
  x86p_emit_store8_reg(c->e, CPU_REG, reg_off_w(dst->reg, 1), kX64Rax);
}
