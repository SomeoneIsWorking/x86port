#include "alu.h"
#include "bit_ops.h"
#include "flags.h"
#include "jit_x64_gpr.h"
#include "jit_x64_internal.h"
#include "jit_x64_x87_inline.h"
#include <stddef.h>

static int alu_writes_dest(uint8_t op) {
  return op != (uint8_t)kX86pAluCmp && op != (uint8_t)kX86pAluTest;
}

/*
 * Which host ALU opcode computes a guest ALU op, and which flag kind it
 * records. Returns 0 for the ops that are not inlined.
 *
 * ADC and SBB are deliberately absent. They do not use the lazy tuple at all --
 * x86p_alu computes a real EFLAGS word for them and stores it as Explicit,
 * because the triple cannot carry a carry-in. Reproducing that inline would be
 * a second implementation of the eager derivation, which is exactly what this
 * backend does not do; they keep calling x86p_alu.
 */
int inline_alu_shape(uint8_t alu, X86pHostAlu *host, X86pFlagKind *kind, int *writes_dest) {
  *writes_dest = 1;
  switch (alu) {
  case kX86pAluAdd:
    *host = kX64Add;
    *kind = kX86pFlagsAdd;
    return 1;
  case kX86pAluSub:
    *host = kX64Sub;
    *kind = kX86pFlagsSub;
    return 1;
  case kX86pAluCmp:
    *host = kX64Sub;
    *kind = kX86pFlagsSub;
    *writes_dest = 0;
    return 1;
  case kX86pAluOr:
    *host = kX64Or;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluAnd:
    *host = kX64And;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluTest:
    *host = kX64And;
    *kind = kX86pFlagsLogic;
    *writes_dest = 0;
    return 1;
  case kX86pAluXor:
    *host = kX64Xor;
    *kind = kX86pFlagsLogic;
    return 1;
  default:
    return 0;
  }
}

/*
 * Store `carry_in`: the CF the flag state held BEFORE this operation overwrites
 * it. x86p_flags_set records it because INC and DEC preserve CF, and guest code
 * really does put an INC between an ADD and an ADC.
 *
 * WHY THIS CAN BE INLINED AT ALL. CF's derivation depends on the PREVIOUS
 * operation's kind, which is runtime data in general -- but inside a block it
 * is known at translation time, because this file emitted the previous
 * operation and knows what kind it recorded. Only the first flag write in a
 * block faces an unknown predecessor, and that one calls x86p_flag_cf. So the
 * call happens once per block instead of once per instruction.
 *
 * The derivations mirror x86p_flag_cf exactly, at w == 4 where the width mask
 * is the identity. A shift predecessor takes the default arm and asks the real
 * function: its CF depends on the count and the width.
 */
static int emit_compute_carry_in(BlockCtx *c, int last_kind) {
  X86pEmit *e = c->e;
  switch (last_kind) {
  case kX86pFlagsNone:
  case kX86pFlagsLogic:
    /* Both give CF == 0 with no computation at all. */
    x86p_emit_mov_r32_imm32(e, CARRY_REG, 0u);
    return 0;
  case kX86pFlagsAdd:
    /* CF = r < a, unsigned */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_R);
    x86p_emit_alu_r32_mem(e, kX64Cmp, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_setcc_r8(e, (unsigned)kX86pCondB, CARRY_REG);
    return 0;
  case kX86pFlagsExplicit:
    /* A real EFLAGS word, which ADC and SBB leave behind: CF is bit 0 of `a`,
       so masking it IS the 0-or-1 the field wants. Known at translation time
       like any other kind -- x86p_alu always records Explicit for those two --
       so it needs no more of a helper call than an ADD does. */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_alu_r32_imm32(e, kX64And, CARRY_REG, X86P_CF);
    return 0;
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* PRESERVED. INC and DEC do not write CF, so the carry the state already
       holds IS the carry, and x86p_flag_cf returns exactly this byte. */
    x86p_emit_load8_zx(e, CARRY_REG, CPU_REG, FLAG_CARRY_IN);
    return 0;
  case kX86pFlagsSub:
    /* CF = a < b, unsigned */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_alu_r32_mem(e, kX64Cmp, CARRY_REG, CPU_REG, FLAG_B);
    x86p_emit_setcc_r8(e, (unsigned)kX86pCondB, CARRY_REG);
    return 0;
  default:
    /* Unknown predecessor: ask the one authority. Once per block. A call
       finds the host x87 stack empty, as the ABI requires. */
    x87_cache_flush(c);
    x86p_emit_lea64(e, X86P_JIT_HOST_ARG0, CPU_REG, flags_off());
    x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_flag_cf);
    x86p_emit_call_r64(e, kX64Rax);
    x86p_emit_mov_r32_r32(e, CARRY_REG, kX64Rax);
    return 1;
  }
}

/* The non-memory side of an ALU operand, at width `w`. An immediate is masked
   HERE, at translation time, because x86p_alu masks its `b` and the tuple has
   to match: `83 /r` sign-extends an imm8 to a dword that the operation then
   narrows again. */
static void emit_read_alu_src(BlockCtx *c, X86pHostReg dst, const X86pOperand *o, int w) {
  if (o->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(c->e, dst, o->imm & x86p_width_mask(w));
    return;
  }
  gpr_load(c, dst, o->reg, w);
}

/*
 * The inlined form: native host arithmetic plus the lazy tuple written
 * directly, with no call at all.
 *
 * This is the same computation x86p_alu performs, not a second opinion about
 * it: the host ALU op is chosen to compute exactly what the guest op computes
 * at 32 bits, and the tuple stored is field for field what x86p_flags_set
 * stores. What it does NOT duplicate is any policy -- ADC/SBB, the
 * rotates and the flag DERIVATIONS all still live in one place and still go
 * through it. The differential is what keeps that claim honest.
 */
void emit_alu_inline(BlockCtx *c,
                     const X86pInsn *insn,
                     X86pHostAlu host,
                     X86pFlagKind kind,
                     int writes_dest,
                     int last_kind,
                     int flags_dead,
                     uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  /*
   * COMPUTED first, while the old flag state is still intact -- and STORED only
   * after the memory operand's bounds check, because a refused access must
   * leave every flag field exactly as it was. Writing carry_in before the check
   * left the flags half-updated on a fault: a divergence that only appears
   * three instructions later, when something finally reads CF.
   *
   * `flags_dead` (from flag_write_is_dead) means a later instruction rewrites
   * every EFLAGS bit before anything reads them: the whole tuple, carry_in
   * included, is skipped. The native arithmetic and its write-back stay.
   */
  /*
   * A binary ALU operation records Add, Sub or Logic, and
   * x86p_flags_carry_in_is_live says none of those ever reads carry_in again.
   * So the derivation AND the store are dead here -- only the unary INC/DEC
   * path below, which records a kind that does preserve CF, still pays for
   * them. Measured on the arm64 Android build, the derivation's
   * unknown-predecessor arm alone (one call to x86p_flag_cf per block, paid on
   * every entry to that block) was 5.07% of the port library's samples.
   */
  const int carry_live = x86p_flags_carry_in_is_live(kind);

  if (!flags_dead && carry_live) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c, last_kind);
  }

  /*
   * The memory operand, whichever side it is on, is prepared ONCE and its
   * pointer reused for both the read and the write-back. Recomputing the
   * address for the store would double the cost and, worse, would recompute it
   * from registers the operation may have just modified -- `ADD [EAX+4], EAX`
   * must store where it loaded.
   */
  const int w = dst->size;

  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (!flags_dead && carry_live) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rsi, HOSTPTR_REG, 0, w);
    emit_read_alu_src(c, kX64Rdx, src, w);
  } else if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    if (!flags_dead && carry_live) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rdx, HOSTPTR_REG, 0, w);
    gpr_load(c, kX64Rsi, dst->reg, w);
  } else {
    if (!flags_dead && carry_live) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_read_alu_src(c, kX64Rdx, src, w);
    gpr_load(c, kX64Rsi, dst->reg, w);
  }

  x86p_emit_mov_r32_r32(c->e, kX64Rax, kX64Rsi);
  x86p_emit_alu_r32_r32(c->e, host, kX64Rax, kX64Rdx); /* r */
  if (w != 4) {
    /*
     * The tuple must hold the values x86p_alu would have stored, and it masks
     * a, b and r to the operand width. `a` and `b` arrive masked because they
     * were loaded zero-extended; `r` is the 32-bit result of a 32-bit host
     * operation and is not. Every DERIVED flag masks by w and would agree
     * either way -- it is the raw tuple that would differ, which is exactly
     * the field a caller inspecting flag state reads.
     */
    x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rax, x86p_width_mask(w));
  }

  if (!flags_dead) {
    x86p_emit_store32(c->e, CPU_REG, FLAG_A, kX64Rsi);
    x86p_emit_store32(c->e, CPU_REG, FLAG_B, kX64Rdx);
    x86p_emit_store32(c->e, CPU_REG, FLAG_R, kX64Rax);
    x86p_emit_store16_imm(c->e, CPU_REG, flag_kind_off(), flag_kind_word((unsigned)kind, (unsigned)w));
  }

  if (writes_dest) {
    if (dst->kind == kX86pOperandMem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
    } else {
      gpr_store(c, dst->reg, kX64Rax, w);
    }
  }
}

/*
 * INC, DEC, NEG and NOT.
 *
 * NOT writes NO FLAGS AT ALL, which is why it is a separate case rather than
 * `XOR a, -1`: the XOR would clear CF and OF. It therefore leaves last_kind
 * alone as well -- an instruction that writes no flags does not become the
 * predecessor of the next one.
 *
 * INC and DEC PRESERVE CF, which is the entire reason `carry_in` exists: guest
 * code really does put an INC between an ADD and an ADC.
 *
 * Returns the flag kind recorded, or -1 for NOT, which records none.
 */
int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];
  const int w = o->size;
  const int is_mem = (o->kind == kX86pOperandMem);
  X86pFlagKind kind;

  if (insn->alu == (uint8_t)kX86pAluNot) {
    if (is_mem) {
      emit_mem_prepare_w(c, o, insn_eip, w);
      emit_load_w(c->e, kX64Rax, HOSTPTR_REG, 0, w);
    } else {
      gpr_load(c, kX64Rax, o->reg, w);
    }
    /* XOR with all ones is the host's NOT; the guest's flag rule is honoured by
       storing nothing, not by choosing a different host opcode. */
    x86p_emit_alu_r32_imm32(c->e, kX64Xor, kX64Rax, 0xFFFFFFFFu);
    if (w != 4) {
      x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rax, x86p_width_mask(w));
    }
    if (is_mem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
    } else {
      gpr_store(c, o->reg, kX64Rax, w);
    }
    return -1;
  }

  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c, last_kind);
  }

  if (is_mem) {
    emit_mem_prepare_w(c, o, insn_eip, w);
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rsi, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    gpr_load(c, kX64Rsi, o->reg, w);
  }

  if (insn->alu == (uint8_t)kX86pAluNeg) {
    /* 0 - a, recorded as the SUB it is, so CF falls out of the borrow rather
       than being special-cased as "a was nonzero". */
    x86p_emit_mov_r32_imm32(c->e, kX64Rax, 0u);
    x86p_emit_alu_r32_r32(c->e, kX64Sub, kX64Rax, kX64Rsi);
    kind = kX86pFlagsSub;
  } else {
    x86p_emit_mov_r32_r32(c->e, kX64Rax, kX64Rsi);
    if (insn->alu == (uint8_t)kX86pAluInc) {
      x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rax, 1u);
      kind = kX86pFlagsInc;
    } else {
      x86p_emit_alu_r32_imm32(c->e, kX64Sub, kX64Rax, 1u);
      kind = kX86pFlagsDec;
    }
  }
  if (w != 4) {
    x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rax, x86p_width_mask(w));
  }

  /* NEG's operands are (0, a); INC and DEC's are (a, 1). The tuple must be
     what x86p_alu_unary would have stored, because every derived flag reads it
     and so does the next instruction's carry-in. */
  if (!flags_dead) {
    if (kind == kX86pFlagsSub) {
      x86p_emit_store32_imm(c->e, CPU_REG, FLAG_A, 0u);
      x86p_emit_store32(c->e, CPU_REG, FLAG_B, kX64Rsi);
    } else {
      x86p_emit_store32(c->e, CPU_REG, FLAG_A, kX64Rsi);
      x86p_emit_store32_imm(c->e, CPU_REG, FLAG_B, 1u);
    }
    x86p_emit_store32(c->e, CPU_REG, FLAG_R, kX64Rax);
    x86p_emit_store16_imm(c->e, CPU_REG, flag_kind_off(), flag_kind_word((unsigned)kind, (unsigned)w));
  }

  if (is_mem) {
    emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
  } else {
    gpr_store(c, o->reg, kX64Rax, w);
  }
  return (int)kind;
}

/* x86p_alu(op, a, b, w, &cpu->flags) -> result in EAX. Argument placement is
 * owned by jit_x64_abi.h. The operand loads run first because they read through
 * CPU_REG; the later argument setup writes registers they would otherwise have
 * to avoid. Preserve the caller's nonvolatile registers and 16-byte stack
 * alignment. Bounds checks precede the local save, so fault exits need no extra
 * unwind. The operands are read from memory, not the guest register cache
 * (jit_x64_gpr.h): a memory destination holds its host pointer in R12, which
 * is one of the cache's registers, and memory is always current. */
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
      gpr_store(c, dst->reg, kX64Rax, w);
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
    gpr_load(c, kX64Rsi, dst->reg, w);
  }
  if (by_cl) {
    gpr_load(c, kX64Rcx, src->reg, src->size);
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
    gpr_store(c, dst->reg, kX64Rax, w);
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
    gpr_load(c, X86P_JIT_HOST_ARG1, operand->reg, width);
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
  gpr_load(c, X86P_JIT_HOST_ARG2, src->reg, 4);
  if (count->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG3, count->imm);
  } else {
    gpr_load(c, X86P_JIT_HOST_ARG3, kX86pEcx, 1); /* CL */
  }
  x86p_jit_abi_emit_arg32_imm(c->e, X86P_JIT_HOST_ABI, 4, insn->op == kX86pInsnShld);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cpu_double_shift32);
  x86p_emit_call_r64(c->e, kX64Rax);
}
