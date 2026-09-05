/* AArch64 integer arithmetic emission. The shared ALU semantic owner handles
 * operations that require a helper; this module owns emitted operand access,
 * lazy-flag updates, carry preservation, and arithmetic-fault registration. */
#include "jit_arm64_integer.h"
#include "alu.h"

static int alu_writes_dest(uint8_t op) {
  return op != (uint8_t)kX86pAluCmp && op != (uint8_t)kX86pAluTest;
}

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

  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
    emit_read_alu_src(c, kA64X1, src, w);
  } else if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
    emit_load_w(c->e, kA64X1, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
    emit_read_alu_src(c, kA64X1, src, w);
  }

  x86p_a64_emit_mov_w_w(c->e, kA64X2, kA64X0);
  x86p_a64_emit_alu_w_w(c->e, host, kA64X2, kA64X1); /* r */
  if (w != 4) {
    x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X2, width_mask(w));
  }

  if (!flags_dead) {
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_A, kA64X0);
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_B, kA64X1);
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_R, kA64X2);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_W, (uint8_t)w);
  }

  if (writes_dest) {
    if (dst->kind == kX86pOperandMem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kA64X2, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, w), kA64X2, w);
    }
  }
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

/* The shipping emitter calls the canonical widening-multiply semantics after
   capturing the explicit operand -- identical to jit_x64.c's jit_mul32. */
static void jit_mul32(X86pCpu *cpu, uint32_t operand) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  x86p_alu_mul(cpu->reg[kX86pEax], operand, 4, &low, &high, &cpu->flags);
  cpu->reg[kX86pEax] = low;
  cpu->reg[kX86pEdx] = high;
}

void emit_mul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *operand = &insn->operand[0];

  if (operand->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, operand, insn_eip, 4);
    x86p_a64_emit_load32(c->e, kA64X1, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load32(c->e, kA64X1, CPU_REG, reg_off(operand->reg));
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  emit_call(c->e, (void *)&jit_mul32);
}

static void jit_imul32(X86pCpu *cpu, uint32_t destination, uint32_t left, uint32_t right) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  x86p_alu_imul(left, right, 4, &low, &high, &cpu->flags);
  cpu->reg[destination] = low;
}

void emit_imul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *destination = &insn->operand[0];
  const X86pOperand *source = &insn->operand[1];

  if (insn->operands == 2) {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_a64_emit_load32(c->e, kA64X3, HOSTPTR_REG, 0);
    } else {
      x86p_a64_emit_load32(c->e, kA64X3, CPU_REG, reg_off(source->reg));
    }
    x86p_a64_emit_load32(c->e, kA64X2, CPU_REG, reg_off(destination->reg));
  } else {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_a64_emit_load32(c->e, kA64X2, HOSTPTR_REG, 0);
    } else {
      x86p_a64_emit_load32(c->e, kA64X2, CPU_REG, reg_off(source->reg));
    }
    x86p_a64_emit_mov_w_imm32(c->e, kA64X3, insn->operand[2].imm);
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, destination->reg);
  emit_call(c->e, (void *)&jit_imul32);
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

  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (is_mem) {
    emit_mem_prepare_w(c, o, insn_eip, w);
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead) {
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
    x86p_a64_emit_mov_w_w(c->e, kA64X1, kA64X0);
    if (insn->alu == (uint8_t)kX86pAluInc) {
      x86p_a64_emit_alu_w_imm(c->e, kA64Add, kA64X1, 1u);
      kind = kX86pFlagsInc;
    } else {
      x86p_a64_emit_alu_w_imm(c->e, kA64Sub, kA64X1, 1u);
      kind = kX86pFlagsDec;
    }
  }
  if (w != 4) {
    x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X1, width_mask(w));
  }

  /* NEG's operands are (0, a); INC and DEC's are (a, 1). X0 holds the operand
     `a` throughout -- reused below whichever branch ran -- and X1 holds the
     result. */
  if (!flags_dead) {
    if (kind == kX86pFlagsSub) {
      x86p_a64_emit_store32_imm(c->e, CPU_REG, FLAG_A, 0u);
      x86p_a64_emit_store32(c->e, CPU_REG, FLAG_B, kA64X0);
    } else {
      x86p_a64_emit_store32(c->e, CPU_REG, FLAG_A, kA64X0);
      x86p_a64_emit_store32_imm(c->e, CPU_REG, FLAG_B, 1u);
    }
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_R, kA64X1);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_W, (uint8_t)w);
  }

  if (is_mem) {
    emit_store_w(c->e, HOSTPTR_REG, 0, kA64X1, w);
  } else {
    emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kA64X1, w);
  }
  return (int)kind;
}

/* The shift / ADC / SBB path: register-only (can_emit keeps memory operands
   away from them), calling x86p_alu(op, a, b, w, &flags) -> result in X0. */
void emit_alu(X86pA64Emit *e, const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;

  emit_load_w(e, kA64X1, CPU_REG, reg_off_w(dst->reg, w), w); /* a */
  if (src->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(e, kA64X2, src->imm & width_mask(w)); /* b */
  } else {
    /* At the SOURCE's own width: a shift's count is CL, one byte. */
    emit_load_w(e, kA64X2, CPU_REG, reg_off_w(src->reg, src->size), src->size);
  }
  x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)w);
  x86p_a64_emit_lea64(e, kA64X4, CPU_REG, flags_off());
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)insn->alu);
  emit_call(e, (void *)&x86p_alu);
  if (alu_writes_dest(insn->alu)) {
    emit_store_w(e, CPU_REG, reg_off_w(dst->reg, w), kA64X0, w);
  }
}
