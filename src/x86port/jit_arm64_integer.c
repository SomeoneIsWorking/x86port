/* AArch64 integer arithmetic emission. The shared ALU semantic owner handles
 * operations that require a helper; this module owns emitted operand access,
 * lazy-flag updates, carry preservation, and arithmetic-fault registration. */
#include "jit_arm64_integer.h"
#include "alu.h"
#include "multiply.h"

/* "cmp DST, [base+disp]" has no single AArch64 instruction: load the operand
   into the encoder's other scratch (X8) and compare. Only ever the second of
   a two-instruction sequence with nothing live in X8 across it. */
static void emit_cmp_w_mem(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg base, int32_t disp) {
  x86p_a64_emit_load32(e, kA64X8, base, disp);
  x86p_a64_emit_cmp_w_w(e, a, kA64X8);
}

static void note_divide_fault(BlockCtx *c, X86pA64EmitSite site) {
  if (c->ndivide_faults < sizeof c->divide_faults / sizeof c->divide_faults[0]) {
    c->divide_faults[c->ndivide_faults++] = site;
    return;
  }
  c->e->overflow = 1;
}

/* Which host ALU opcode computes a guest ALU op, and which flag kind it
   records -- identical mapping to jit_x64.c's inline_alu_shape, targeting
   X86pA64Alu. CMP maps to a plain Sub with writes_dest=0, exactly as x64
   maps it to Sub rather than exposing a distinct "compare" opcode: the guest
   destination is simply never written back. */
int inline_alu_shape(uint8_t alu, X86pA64Alu *host, X86pFlagKind *kind, int *writes_dest) {
  *writes_dest = 1;
  switch (alu) {
  case kX86pAluAdd:
    *host = kA64Add;
    *kind = kX86pFlagsAdd;
    return 1;
  case kX86pAluSub:
    *host = kA64Sub;
    *kind = kX86pFlagsSub;
    return 1;
  case kX86pAluCmp:
    *host = kA64Sub;
    *kind = kX86pFlagsSub;
    *writes_dest = 0;
    return 1;
  case kX86pAluOr:
    *host = kA64Orr;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluAnd:
    *host = kA64And;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluTest:
    *host = kA64And;
    *kind = kX86pFlagsLogic;
    *writes_dest = 0;
    return 1;
  case kX86pAluXor:
    *host = kA64Eor;
    *kind = kX86pFlagsLogic;
    return 1;
  default:
    return 0;
  }
}

_Static_assert(offsetof(X86pFlags, b) == offsetof(X86pFlags, a) + 4u, "a and b form one pair store");
_Static_assert(offsetof(X86pFlags, w) == offsetof(X86pFlags, kind) + 1u, "kind and w form one halfword");

void emit_record_flags(BlockCtx *c, X86pA64Reg a, X86pA64Reg b, X86pA64Reg r, int kind, int w) {
  x86p_a64_emit_store_pair32(c->e, CPU_REG, FLAG_A, a, b);
  x86p_a64_emit_store32(c->e, CPU_REG, FLAG_R, r);
  x86p_a64_emit_store16_imm(c->e, CPU_REG, FLAG_KIND, (uint16_t)((unsigned)kind | ((unsigned)w << 8)));
  c->flag_regs_out.a = (int)a;
  c->flag_regs_out.b = (int)b;
  c->flag_regs_out.r = (int)r;
}

/*
 * Store `carry_in`: the CF the flag state held BEFORE this operation
 * overwrites it -- see jit_x64.c's emit_compute_carry_in for the full
 * rationale. The derivations mirror x86p_flag_cf exactly, at w == 4.
 */
static int emit_compute_carry_in(X86pA64Emit *e, int last_kind) {
  switch (last_kind) {
  case kX86pFlagsNone:
  case kX86pFlagsLogic:
    x86p_a64_emit_mov_w_imm32(e, CARRY_REG, 0u);
    return 0;
  case kX86pFlagsAdd:
    /* CF = r < a, unsigned */
    x86p_a64_emit_load32(e, CARRY_REG, CPU_REG, FLAG_R);
    emit_cmp_w_mem(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_a64_emit_cset_w(e, kA64CondCc, CARRY_REG);
    return 0;
  case kX86pFlagsExplicit:
    /* A real EFLAGS word, which ADC and SBB leave behind: CF is bit 0. */
    x86p_a64_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_a64_emit_alu_w_imm(e, kA64And, CARRY_REG, X86P_CF);
    return 0;
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* PRESERVED: the carry the state already holds IS the carry. */
    x86p_a64_emit_load8_zx(e, CARRY_REG, CPU_REG, FLAG_CARRY_IN);
    return 0;
  case kX86pFlagsSub:
    /* CF = a < b, unsigned */
    x86p_a64_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    emit_cmp_w_mem(e, CARRY_REG, CPU_REG, FLAG_B);
    x86p_a64_emit_cset_w(e, kA64CondCc, CARRY_REG);
    return 0;
  default:
    /* Unknown predecessor: ask the one authority. Once per block. */
    x86p_a64_emit_lea64(e, kA64X0, CPU_REG, flags_off());
    emit_call(e, (void *)&x86p_flag_cf);
    x86p_a64_emit_mov_w_w(e, CARRY_REG, kA64X0);
    return 1;
  }
}

static uint32_t width_mask(int w) {
  return (w == 1) ? 0xFFu : ((w == 2) ? 0xFFFFu : 0xFFFFFFFFu);
}

/* The non-memory side of an ALU operand, at width `w`. */
static void emit_read_alu_src(BlockCtx *c, X86pA64Reg dst, const X86pOperand *o, int w) {
  if (o->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(c->e, dst, o->imm & width_mask(w));
    return;
  }
  emit_load_w(c->e, dst, CPU_REG, reg_off_w(o->reg, w), w);
}

/*
 * The inlined ALU form: native host arithmetic plus the lazy tuple written
 * directly, with no call at all -- see jit_x64.c's emit_alu_inline for the
 * ordering rationale (carry-in computed before the memory operand's bounds
 * check, stored only after). X0/X1 hold the two operand values (a, b); X2
 * holds the native result -- all three inside the X0..X7 pool, which no role
 * register ever occupies.
 */
void emit_alu_inline(BlockCtx *c,
                     const X86pInsn *insn,
                     X86pA64Alu host,
                     X86pFlagKind kind,
                     int writes_dest,
                     int last_kind,
                     int flags_dead,
                     uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;

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
  /* With the tuple dead, an ADD or SUB immediate never needs `b` in a
     register: the host instruction can take it directly. */
  const int fold_imm = flags_dead && src->kind == kX86pOperandImm && (host == kA64Add || host == kA64Sub);

  if (!flags_dead && carry_live) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (!flags_dead && carry_live) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
    if (!fold_imm) {
      emit_read_alu_src(c, kA64X1, src, w);
    }
  } else if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    if (!flags_dead && carry_live) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
    emit_load_w(c->e, kA64X1, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead && carry_live) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
    if (!fold_imm) {
      emit_read_alu_src(c, kA64X1, src, w);
    }
  }

  /* r */
  if (!fold_imm || !x86p_a64_emit_add_sub_w_imm(c->e, host == kA64Sub, kA64X2, kA64X0, src->imm & width_mask(w))) {
    if (fold_imm) {
      emit_read_alu_src(c, kA64X1, src, w);
    }
    x86p_a64_emit_alu_w_w_w(c->e, host, kA64X2, kA64X0, kA64X1);
  }
  if (w != 4) {
    x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X2, width_mask(w));
  }

  if (!flags_dead) {
    emit_record_flags(c, kA64X0, kA64X1, kA64X2, kind, w);
  }

  if (writes_dest) {
    if (dst->kind == kX86pOperandMem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kA64X2, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, w), kA64X2, w);
    }
  }
}

/*
 * SHL, SHR and SAR on the host, recording the tuple x86p_alu records: the
 * masked operand, the masked count, the result and the kind -- see
 * jit_x64_alu.c's emit_shift_inline, which this mirrors. Every flag is still
 * derived from that tuple by flags.c; only the arithmetic moved. Before this,
 * every shift was a call to x86p_alu through emit_alu_helper.
 *
 * A count of zero (after the architectural five-bit mask) writes no flags and
 * leaves the destination alone, so it keeps only a memory operand's bounds
 * check: known at translation for an immediate, tested at run time for CL.
 *
 * The narrow widths come out of the 32-bit host shift unchanged: the operand
 * is loaded zero-extended, so LSR cannot pull in stray bits and LSL's are
 * masked off; ASR sign-extends first, so a count past the width fills with the
 * sign as x86p_alu's does. The count is at most 31, inside the 32-bit
 * register's range, so the hardware's modulo-32 never applies.
 *
 * carry_in is not stored: x86p_flags_carry_in_is_live says a shift kind never
 * reads it, exactly as for the binary operations in emit_alu_inline.
 */
int emit_shift_inline(BlockCtx *c, const X86pInsn *insn, int flags_dead, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;
  const int by_cl = src->kind != kX86pOperandImm;
  const uint32_t count = src->imm & 0x1Fu;
  const X86pA64Shift op = insn->alu == (uint8_t)kX86pAluShl   ? kA64Lsl
                          : insn->alu == (uint8_t)kX86pAluShr ? kA64Lsr
                                                              : kA64Asr;
  const X86pFlagKind kind = op == kA64Lsl ? kX86pFlagsShl : op == kA64Lsr ? kX86pFlagsShr : kX86pFlagsSar;
  X86pA64EmitSite zero = {0};

  if (dst->kind == kX86pOperandMem) {
    /* Even at a zero count: the access still happens, and faults. */
    emit_mem_prepare_w(c, dst, insn_eip, w);
  }
  if (!by_cl && count == 0u) {
    return SHIFT_FLAGS_UNCHANGED;
  }
  if (dst->kind == kX86pOperandMem) {
    emit_load_w(e, kA64X0, HOSTPTR_REG, 0, w);
  } else {
    emit_load_w(e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
  }
  if (by_cl) {
    emit_load_w(e, kA64X1, CPU_REG, reg_off_w(src->reg, src->size), src->size);
    x86p_a64_emit_alu_w_imm(e, kA64And, kA64X1, 0x1Fu);
    zero = x86p_a64_emit_cbz_w(e, kA64X1);
  } else {
    x86p_a64_emit_mov_w_imm32(e, kA64X1, count);
  }

  x86p_a64_emit_mov_w_w(e, kA64X2, kA64X0);
  if (op == kA64Asr && w != 4) {
    x86p_a64_emit_shl_w_imm(e, kA64X2, (uint8_t)(32 - 8 * w));
    x86p_a64_emit_sar_w_imm(e, kA64X2, (uint8_t)(32 - 8 * w));
  }
  x86p_a64_emit_shift_w_w(e, op, kA64X2, kA64X2, kA64X1);
  if (op != kA64Lsr && w != 4) {
    x86p_a64_emit_alu_w_imm(e, kA64And, kA64X2, width_mask(w));
  }

  if (!flags_dead) {
    emit_record_flags(c, kA64X0, kA64X1, kA64X2, kind, w);
  }
  if (dst->kind == kX86pOperandMem) {
    emit_store_w(e, HOSTPTR_REG, 0, kA64X2, w);
  } else {
    emit_store_w(e, CPU_REG, reg_off_w(dst->reg, w), kA64X2, w);
  }
  if (by_cl) {
    /* A zero count skips here with the tuple unwritten, so the registers
       describe it on one path only. */
    x86p_a64_emit_bind(e, zero);
    c->flag_regs_out = x86p_a64_flags_in_memory();
    return SHIFT_FLAGS_UNKNOWN;
  }
  return (int)kind;
}

void emit_cdq(X86pA64Emit *e) {
  x86p_a64_emit_load32(e, kA64X0, CPU_REG, reg_off(kX86pEax));
  x86p_a64_emit_sar_w_imm(e, kA64X0, 31u);
  x86p_a64_emit_store32(e, CPU_REG, reg_off(kX86pEdx), kA64X0);
}

/* This helper owns only one already-decoded operation's value semantics --
   identical to jit_x64.c's jit_div32. */
static int jit_div32(X86pCpu *cpu, uint32_t divisor, uint32_t signed_divide) {
  uint32_t quotient = 0u;
  uint32_t remainder = 0u;

  int ok = signed_divide
               ? x86p_alu_idiv(cpu->reg[kX86pEdx], cpu->reg[kX86pEax], divisor, 4, &quotient, &remainder, &cpu->flags)
               : x86p_alu_div(cpu->reg[kX86pEdx], cpu->reg[kX86pEax], divisor, 4, &quotient, &remainder, &cpu->flags);
  if (!ok) {
    return 0;
  }
  cpu->reg[kX86pEax] = quotient;
  cpu->reg[kX86pEdx] = remainder;
  return 1;
}

void emit_div32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int signed_divide) {
  const X86pOperand *divisor = &insn->operand[0];
  X86pA64EmitSite failed;

  if (divisor->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, divisor, insn_eip, 4);
    x86p_a64_emit_load32(c->e, kA64X1, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load32(c->e, kA64X1, CPU_REG, reg_off(divisor->reg));
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, (uint32_t)signed_divide);
  emit_call(c->e, (void *)&jit_div32);
  x86p_a64_emit_tst_w_w(c->e, kA64X0, kA64X0);
  x86p_a64_emit_mov_w_imm32(c->e, FAULTPC_REG, insn_eip);
  failed = x86p_a64_emit_bcc(c->e, kA64CondEq);
  note_divide_fault(c, failed);
}

/* IMUL r, r/m[, imm] at 16 or 32 bits; see jit_x64.c's emit_imul_to_register
   for why the r/m operand is read first. With its flags dead the product is
   one MUL: the destination keeps only the low half, which does not depend on
   signedness, and only the flags (CF and OF: whether it fit) need
   x86p_imul_to_register. */
void emit_imul_to_register(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int flags_dead) {
  const X86pOperand *destination = &insn->operand[0];
  const X86pOperand *source = &insn->operand[1];
  const int width = destination->size;
  const X86pA64Reg source_arg = insn->operands == 2 ? kA64X2 : kA64X1;

  if (flags_dead) {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, width);
      emit_load_w(c->e, kA64X1, HOSTPTR_REG, 0, width);
    } else {
      emit_load_w(c->e, kA64X1, CPU_REG, reg_off_w(source->reg, width), width);
    }
    if (insn->operands == 2) {
      emit_load_w(c->e, kA64X2, CPU_REG, reg_off_w(destination->reg, width), width);
    } else {
      x86p_a64_emit_mov_w_imm32(c->e, kA64X2, insn->operand[2].imm);
    }
    x86p_a64_emit_mul_w(c->e, kA64X0, kA64X1, kA64X2);
    emit_store_w(c->e, CPU_REG, reg_off_w(destination->reg, width), kA64X0, width);
    return;
  }

  if (source->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, source, insn_eip, width);
    emit_load_w(c->e, source_arg, HOSTPTR_REG, 0, width);
  } else {
    emit_load_w(c->e, source_arg, CPU_REG, reg_off_w(source->reg, width), width);
  }
  if (insn->operands == 2) {
    emit_load_w(c->e, kA64X1, CPU_REG, reg_off_w(destination->reg, width), width);
  } else {
    x86p_a64_emit_mov_w_imm32(c->e, kA64X2, insn->operand[2].imm);
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X3, (uint32_t)destination->reg);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X4, (uint32_t)width);
  emit_call(c->e, (void *)&x86p_imul_to_register);
}

/* INC, DEC, NEG and NOT -- see jit_x64.c's emit_alu_unary_inline for the CF
   preservation rationale. Returns the flag kind recorded, or -1 for NOT. */
int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];
  const int w = o->size;
  const int is_mem = (o->kind == kX86pOperandMem);
  X86pFlagKind kind;

  if (insn->alu == (uint8_t)kX86pAluNot) {
    if (is_mem) {
      emit_mem_prepare_w(c, o, insn_eip, w);
      emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
    } else {
      emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(o->reg, w), w);
    }
    x86p_a64_emit_alu_w_imm(c->e, kA64Eor, kA64X0, 0xFFFFFFFFu);
    if (w != 4) {
      x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X0, width_mask(w));
    }
    if (is_mem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kA64X0, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kA64X0, w);
    }
    return -1;
  }

  /* NEG records the Sub kind, which never reads carry_in again
     (x86p_flags_carry_in_is_live); only INC and DEC preserve CF through it. */
  const int carry_live = insn->alu != (uint8_t)kX86pAluNeg;
  if (!flags_dead && carry_live) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (is_mem) {
    emit_mem_prepare_w(c, o, insn_eip, w);
    if (!flags_dead && carry_live) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead && carry_live) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(o->reg, w), w);
  }

  if (insn->alu == (uint8_t)kX86pAluNeg) {
    /* 0 - a, recorded as the SUB it is, so CF falls out of the borrow. */
    x86p_a64_emit_mov_w_imm32(c->e, kA64X1, 0u);
    x86p_a64_emit_alu_w_w(c->e, kA64Sub, kA64X1, kA64X0);
    kind = kX86pFlagsSub;
  } else {
    const int is_dec = insn->alu != (uint8_t)kX86pAluInc;
    (void)x86p_a64_emit_add_sub_w_imm(c->e, is_dec, kA64X1, kA64X0, 1u); /* 1 always encodes */
    kind = is_dec ? kX86pFlagsDec : kX86pFlagsInc;
  }
  if (w != 4) {
    x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X1, width_mask(w));
  }

  /* NEG's operands are (0, a); INC and DEC's are (a, 1). X0 holds the operand
     `a` throughout -- reused below whichever branch ran -- and X1 holds the
     result. */
  if (!flags_dead) {
    /* The constant operand goes through X2, which neither branch uses. */
    if (kind == kX86pFlagsSub) {
      x86p_a64_emit_mov_w_imm32(c->e, kA64X2, 0u);
      emit_record_flags(c, kA64X2, kA64X0, kA64X1, (int)kind, w);
    } else {
      x86p_a64_emit_mov_w_imm32(c->e, kA64X2, 1u);
      emit_record_flags(c, kA64X0, kA64X2, kA64X1, (int)kind, w);
    }
  }

  if (is_mem) {
    emit_store_w(c->e, HOSTPTR_REG, 0, kA64X1, w);
  } else {
    emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kA64X1, w);
  }
  return (int)kind;
}
