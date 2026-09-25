/*
 * jit_arm64_x87_inline.c -- see jit_arm64_x87_inline.h.
 *
 * Register roles inside one form. Nothing survives the guest instruction:
 *
 *   X4  &cpu->x87
 *   W5  TOP, as memory holds it (a push or pop changes it only after the last
 *       guard)
 *   X6  a memory operand's raw bits (X87_BITS_REG)
 *   X7  &cpu->x87 + phys: [X7 + tag0] is that register's tag
 *   X3  a register's slot, the routines' argument; X2 a second slot
 *   X0-X2, D0-D2  scratch the routines may clobber
 *
 * The routines preserve X3-X7 and D1, and write W0 = 0 for an answer and 1
 * for a refusal. A memory operand's address is prepared (EA_REG, HOSTPTR_REG
 * and their scratch, X8-X15) without touching any of these.
 *
 * A tag byte records only occupancy (x87.h): bit 0 set is empty, so a guard
 * tests that bit, and a write stores kX86pX87TagValid without classifying.
 */
#include "jit_arm64_x87_inline.h"

#include "cpu.h"
#include "emit_arm64.h"
#include "x87.h"
#include "x87_ext80_widen.h"

#include <stddef.h>
#include <stdint.h>

#define X87B kA64X4
#define TOP kA64X5
#define BITS X87_BITS_REG
#define TAGB kA64X7
#define SLOT kA64X3
#define SLOT2 kA64X2

static int32_t field(size_t offset) {
  return (int32_t)offset;
}
#define TAG0 field(offsetof(X86pX87, tag))
#define TOP_OFF field(offsetof(X86pX87, top))
#define CONTROL_OFF field(offsetof(X86pX87, control))
#define STATUS_OFF field(offsetof(X86pX87, status))
#define DOUBLE_OFF field(offsetof(X86pX87, double_arith))
#define CENSUS_OFF field(offsetof(X86pX87, op_census))

/* ---- refusals ------------------------------------------------------------- */

static void refuse(BlockCtx *c, X87Slow *s, X86pA64EmitSite site) {
  if (s->refusal_count < sizeof s->refusals / sizeof s->refusals[0]) {
    s->refusals[s->refusal_count++] = site;
    return;
  }
  c->e->overflow = 1;
}

void x87_slow_begin(BlockCtx *c, X87Slow *s) {
  unsigned i;
  if (!s->fast) {
    return;
  }
  s->done = x86p_a64_emit_b(c->e);
  for (i = 0; i < s->refusal_count; i++) {
    x86p_a64_emit_bind(c->e, s->refusals[i]);
  }
}

void x87_slow_end(BlockCtx *c, X87Slow *s) {
  if (s->fast) {
    x86p_a64_emit_bind(c->e, s->done);
  }
}

/* ---- storage-independent pieces: TOP and the tags ---------------------------- */

static void begin(BlockCtx *c) {
  x86p_a64_emit_lea64(c->e, X87B, CPU_REG, (int32_t)offsetof(X86pCpu, x87));
  x86p_a64_emit_load8_zx(c->e, TOP, X87B, TOP_OFF);
}

void emit_x87_pops(BlockCtx *c, unsigned pops) {
  X86pA64Emit *e = c->e;
  unsigned i;
  if (!pops) {
    return;
  }
  begin(c);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)kX86pX87TagEmpty);
  for (i = 0; i < pops; i++) {
    x86p_a64_emit_alu_x_x_lsl(e, kA64Add, TAGB, X87B, TOP, 0);
    x86p_a64_emit_store8_reg(e, TAGB, TAG0, kA64X0);
    x86p_a64_emit_alu_w_imm(e, kA64Add, TOP, 1u);
    x86p_a64_emit_alu_w_imm(e, kA64And, TOP, X86P_X87_REGS - 1u);
  }
  x86p_a64_emit_store8_reg(e, X87B, TOP_OFF, TOP);
}

#if X86P_X87_BINARY128

_Static_assert(sizeof(X86pX87Reg) == 16 && offsetof(X86pX87Reg, signif) == 0 && offsetof(X86pX87Reg, sign_exp) == 8,
               "a slot is the significand, then sign and exponent, at a sixteen-byte stride");
_Static_assert((kX86pX87TagEmpty & 1) == 1 && kX86pX87TagValid == 0, "a guard reads bit 0 of a tag");
_Static_assert(X86P_X87_RC_NEAREST == 0u && X86P_X87_PC_SINGLE == 0u,
               "the control guards read nearest and single precision as zero fields");
_Static_assert(kX86pX87Add == 0 && kX86pX87Sub == 1 && kX86pX87Mul == 2 && kX86pX87Div == 3 && kA64FAdd == 0 &&
                   kA64FSub == 1 && kA64FMul == 2 && kA64FDiv == 3,
               "an x87 operation is its host operation");

/*
 * binary64's fields, and the ext80 exponents whose narrowing is one
 * conversion and one exact scaling -- jit_wasm_x87_arith.c's numbers: unbiased
 * -959 (so the scale 2^(e-63) is still a normal) to 1022 (so rounding up
 * cannot reach infinity).
 */
enum {
  kF64FieldBits = 52,
  kF64ExpMax = 0x7FF,
  kF64Bias = 1023,
  kExt80ExpMask = 0x7FFF,
  kNarrowLowest = X86P_EXT80_BIAS - 959,
  kNarrowSpan = 1022 + 959,
  /* ext80 exponent = binary64 exponent + this, for a normal. */
  kWidenRebias = X86P_EXT80_BIAS - kF64Bias,
  kF32ExpMax = 0xFF,
  /* The low 29 bits of a binary64 that lies exactly halfway between two
     binary32 values: rounding it again could round the other way. */
  kF32TieBits = 29,
  kF32Tie = 1 << 28,
  kCompareBits = X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3
};

/* ---- the tail routines ------------------------------------------------------ */

/*
 * The routines are reserved the first time a form needs one, and only when
 * the buffer has room for them beyond this instruction's and the epilogue's
 * bounds. A form that cannot have them has no fast path.
 */
static int routines_reserved(BlockCtx *c) {
  const size_t need = X86P_JIT_WORST_CASE_INSN_BYTES + X86P_JIT_EPILOGUE_BYTES + X86P_A64_X87_ROUTINE_BYTES;
  if (c->tail_reserve) {
    return 1;
  }
  if (c->e->len + need > c->e->cap) {
    return 0;
  }
  c->tail_reserve = X86P_A64_X87_ROUTINE_BYTES;
  return 1;
}

static void call_narrow(BlockCtx *c, X87Slow *s) {
  if (c->x87_narrow_count >= sizeof c->x87_narrow_calls / sizeof c->x87_narrow_calls[0]) {
    c->e->overflow = 1;
    return;
  }
  c->x87_narrow_calls[c->x87_narrow_count++] = x86p_a64_emit_bl(c->e);
  refuse(c, s, x86p_a64_emit_cbnz_w(c->e, kA64X0));
}

static void call_widen(BlockCtx *c, X87Slow *s) {
  if (c->x87_widen_count >= sizeof c->x87_widen_calls / sizeof c->x87_widen_calls[0]) {
    c->e->overflow = 1;
    return;
  }
  c->x87_widen_calls[c->x87_widen_count++] = x86p_a64_emit_bl(c->e);
  refuse(c, s, x86p_a64_emit_cbnz_w(c->e, kA64X0));
}

/*
 * NARROW: D0 = the ext80 at [X3], rounded to nearest binary64, or a refusal.
 * A normal whose exponent is in kNarrowLowest's span converts its significand
 * (UCVTF rounds 64 bits to 53, to nearest even) and scales by 2^(e-63), which
 * is exact there; a zero is a zero of its sign; anything else is refused.
 */
static void emit_narrow(X86pA64Emit *e, X86pA64EmitSite *refused, unsigned *nrefused) {
  X86pA64EmitSite not_normal;
  X86pA64EmitSite signed_;
  X86pA64EmitSite positive;
  x86p_a64_emit_load64(e, kA64X1, SLOT, 0);
  x86p_a64_emit_load16_zx(e, kA64X2, SLOT, 8);
  not_normal = x86p_a64_emit_tbz(e, kA64X1, 63);
  x86p_a64_emit_mov_w_w(e, kA64X0, kA64X2);
  x86p_a64_emit_alu_w_imm(e, kA64And, kA64X0, kExt80ExpMask);
  x86p_a64_emit_alu_w_imm(e, kA64Sub, kA64X0, kNarrowLowest);
  x86p_a64_emit_cmp_w_imm(e, kA64X0, kNarrowSpan);
  refused[(*nrefused)++] = x86p_a64_emit_bcc(e, kA64CondHi);
  x86p_a64_emit_ucvtf_d_x(e, 0, kA64X1);
  /* The scale's binary64 exponent field: (e - 16383 - 63) + 1023. */
  x86p_a64_emit_alu_w_imm(e, kA64Add, kA64X0, 1u);
  x86p_a64_emit_lsl_x_imm(e, kA64X0, kA64X0, kF64FieldBits);
  x86p_a64_emit_fmov_d_x(e, 2, kA64X0);
  x86p_a64_emit_fop_d(e, kA64FMul, 0, 0, 2);
  signed_ = x86p_a64_emit_b(e);

  x86p_a64_emit_bind(e, not_normal);
  refused[(*nrefused)++] = x86p_a64_emit_cbnz_x(e, kA64X1);
  x86p_a64_emit_mov_w_w(e, kA64X0, kA64X2);
  x86p_a64_emit_alu_w_imm(e, kA64And, kA64X0, kExt80ExpMask);
  refused[(*nrefused)++] = x86p_a64_emit_cbnz_w(e, kA64X0);
  x86p_a64_emit_fmov_d_zero(e, 0);

  x86p_a64_emit_bind(e, signed_);
  positive = x86p_a64_emit_tbz(e, kA64X2, 15);
  x86p_a64_emit_fneg_d(e, 0, 0);
  x86p_a64_emit_bind(e, positive);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, 0u);
  x86p_a64_emit_ret(e);
}

/*
 * WIDEN: the binary64 in D0 into the slot at [X3], exactly, or a refusal that
 * writes nothing. An infinity, a NaN and a subnormal are refused; the upper
 * six bytes of the slot are written zero, as x86p_x87_store_slot keeps them.
 */
static void emit_widen(X86pA64Emit *e, X86pA64EmitSite *refused, unsigned *nrefused) {
  X86pA64EmitSite zero;
  X86pA64EmitSite store;
  x86p_a64_emit_fmov_x_d(e, kA64X1, 0);
  x86p_a64_emit_ubfx_x(e, kA64X0, kA64X1, kF64FieldBits, 11);
  x86p_a64_emit_cmp_w_imm(e, kA64X0, kF64ExpMax);
  refused[(*nrefused)++] = x86p_a64_emit_bcc(e, kA64CondEq);
  zero = x86p_a64_emit_cbz_w(e, kA64X0);
  x86p_a64_emit_lsl_x_imm(e, kA64X2, kA64X1, 11);
  x86p_a64_emit_orr_x_bit(e, kA64X2, 63);
  x86p_a64_emit_alu_w_imm(e, kA64Add, kA64X0, kWidenRebias);
  store = x86p_a64_emit_b(e);

  x86p_a64_emit_bind(e, zero);
  x86p_a64_emit_lsl_x_imm(e, kA64X2, kA64X1, 1);
  refused[(*nrefused)++] = x86p_a64_emit_cbnz_x(e, kA64X2);

  x86p_a64_emit_bind(e, store);
  x86p_a64_emit_lsr_x_imm(e, kA64X1, kA64X1, 63);
  x86p_a64_emit_alu_w_w_lsl(e, kA64Orr, kA64X0, kA64X0, kA64X1, 15);
  x86p_a64_emit_store64(e, SLOT, 0, kA64X2);
  x86p_a64_emit_store64(e, SLOT, 8, kA64X0);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, 0u);
  x86p_a64_emit_ret(e);
}

void emit_x87_routines(BlockCtx *c) {
  X86pA64Emit *e = c->e;
  X86pA64EmitSite refused[8];
  unsigned nrefused = 0;
  unsigned i;
  if (c->x87_narrow_count) {
    for (i = 0; i < c->x87_narrow_count; i++) {
      x86p_a64_emit_bind(e, c->x87_narrow_calls[i]);
    }
    emit_narrow(e, refused, &nrefused);
  }
  if (c->x87_widen_count) {
    for (i = 0; i < c->x87_widen_count; i++) {
      x86p_a64_emit_bind(e, c->x87_widen_calls[i]);
    }
    emit_widen(e, refused, &nrefused);
  }
  if (nrefused) {
    for (i = 0; i < nrefused; i++) {
      x86p_a64_emit_bind(e, refused[i]);
    }
    x86p_a64_emit_mov_w_imm32(e, kA64X0, 1u);
    x86p_a64_emit_ret(e);
  }
}

/* ---- the pieces every form is built from ------------------------------------ */

/* SLOT = &ST(i)'s register, TAGB = X87B + its physical index. */
static void locate(BlockCtx *c, unsigned i) {
  X86pA64Emit *e = c->e;
  x86p_a64_emit_mov_w_w(e, TAGB, TOP);
  if (i) {
    x86p_a64_emit_alu_w_imm(e, kA64Add, TAGB, i);
  }
  x86p_a64_emit_alu_w_imm(e, kA64And, TAGB, X86P_X87_REGS - 1u);
  x86p_a64_emit_alu_x_x_lsl(e, kA64Add, SLOT, X87B, TAGB, 4);
  x86p_a64_emit_alu_x_x_lsl(e, kA64Add, TAGB, X87B, TAGB, 0);
}

static void require_occupied(BlockCtx *c, X87Slow *s) {
  x86p_a64_emit_load8_zx(c->e, kA64X0, TAGB, TAG0);
  refuse(c, s, x86p_a64_emit_tbnz(c->e, kA64X0, 0));
}

static void require_empty(BlockCtx *c, X87Slow *s) {
  x86p_a64_emit_load8_zx(c->e, kA64X0, TAGB, TAG0);
  refuse(c, s, x86p_a64_emit_tbz(c->e, kA64X0, 0));
}

static void mark_valid(BlockCtx *c) {
  x86p_a64_emit_mov_w_imm32(c->e, kA64X0, (uint32_t)kX86pX87TagValid);
  x86p_a64_emit_store8_reg(c->e, TAGB, TAG0, kA64X0);
}

/* The register at SLOT is an exact binary64 number: nothing is lost when the
   narrowing routine rounds it. */
static void require_exact(BlockCtx *c, X87Slow *s) {
  x86p_a64_emit_load64(c->e, kA64X0, SLOT, 0);
  x86p_a64_emit_lsl_x_imm(c->e, kA64X0, kA64X0, kF64FieldBits + 1);
  refuse(c, s, x86p_a64_emit_cbnz_x(c->e, kA64X0));
}

/*
 * The register at `slot` is a value whose trip through the host's widest
 * float, which the FXCH/FCHS/FABS helpers make, is the identity: a normal
 * with its integer bit set, or a zero. A NaN, an infinity, a denormal and the
 * invalid encodings keep the helper.
 */
static void require_canonical(BlockCtx *c, X87Slow *s, X86pA64Reg slot) {
  X86pA64Emit *e = c->e;
  X86pA64EmitSite not_normal;
  X86pA64EmitSite ok;
  x86p_a64_emit_load64(e, kA64X0, slot, 0);
  x86p_a64_emit_load16_zx(e, kA64X1, slot, 8);
  x86p_a64_emit_alu_w_imm(e, kA64And, kA64X1, kExt80ExpMask);
  not_normal = x86p_a64_emit_tbz(e, kA64X0, 63);
  refuse(c, s, x86p_a64_emit_cbz_w(e, kA64X1));
  x86p_a64_emit_alu_w_imm(e, kA64Add, kA64X1, 1u);
  refuse(c, s, x86p_a64_emit_tbnz(e, kA64X1, 15));
  ok = x86p_a64_emit_b(e);
  x86p_a64_emit_bind(e, not_normal);
  x86p_a64_emit_alu_x_x(e, kA64Orr, kA64X0, kA64X1);
  refuse(c, s, x86p_a64_emit_cbnz_x(e, kA64X0));
  x86p_a64_emit_bind(e, ok);
}

/* The consumer's double-arithmetic mode is on, no census counts, rounding is
   to nearest and precision is not single: x86p_ext80_double_arith's terms. */
static void require_double_mode(BlockCtx *c, X87Slow *s) {
  X86pA64Emit *e = c->e;
  x86p_a64_emit_load8_zx(e, kA64X0, X87B, DOUBLE_OFF);
  refuse(c, s, x86p_a64_emit_cbz_w(e, kA64X0));
  x86p_a64_emit_load64(e, kA64X0, X87B, CENSUS_OFF);
  refuse(c, s, x86p_a64_emit_cbnz_x(e, kA64X0));
  /* PC (bits 8-9) and RC (bits 10-11) as one field: 1..3 is RC nearest with
     PC not single. */
  x86p_a64_emit_load16_zx(e, kA64X0, X87B, CONTROL_OFF);
  x86p_a64_emit_ubfx_w(e, kA64X0, kA64X0, 8, 4);
  x86p_a64_emit_alu_w_imm(e, kA64Sub, kA64X0, 1u);
  x86p_a64_emit_cmp_w_imm(e, kA64X0, 2u);
  refuse(c, s, x86p_a64_emit_bcc(e, kA64CondHi));
}

static void require_nearest(BlockCtx *c, X87Slow *s) {
  x86p_a64_emit_load16_zx(c->e, kA64X0, X87B, CONTROL_OFF);
  x86p_a64_emit_ubfx_w(c->e, kA64X0, kA64X0, 10, 2);
  refuse(c, s, x86p_a64_emit_cbnz_w(c->e, kA64X0));
}

/*
 * D1 = the m32/m64 operand in BITS, or a refusal for an infinity, a NaN and a
 * subnormal (the double-arithmetic mode refuses a subnormal operand, and a
 * compare's exactness does not need one).
 */
static void operand_to_d1(BlockCtx *c, X87Slow *s, int width) {
  X86pA64Emit *e = c->e;
  X86pA64EmitSite zero;
  if (width == 4) {
    x86p_a64_emit_lsl_w_w_imm(e, kA64X0, BITS, 1);
    zero = x86p_a64_emit_cbz_w(e, kA64X0);
    x86p_a64_emit_lsr_w_imm(e, kA64X0, 24);
    refuse(c, s, x86p_a64_emit_cbz_w(e, kA64X0));
    x86p_a64_emit_cmp_w_imm(e, kA64X0, kF32ExpMax);
    refuse(c, s, x86p_a64_emit_bcc(e, kA64CondEq));
    x86p_a64_emit_bind(e, zero);
    x86p_a64_emit_fmov_s_w(e, 1, BITS);
    x86p_a64_emit_fcvt_d_s(e, 1, 1);
    return;
  }
  x86p_a64_emit_lsl_x_imm(e, kA64X0, BITS, 1);
  zero = x86p_a64_emit_cbz_x(e, kA64X0);
  x86p_a64_emit_lsr_x_imm(e, kA64X0, kA64X0, kF64FieldBits + 1);
  refuse(c, s, x86p_a64_emit_cbz_w(e, kA64X0));
  x86p_a64_emit_cmp_w_imm(e, kA64X0, kF64ExpMax);
  refuse(c, s, x86p_a64_emit_bcc(e, kA64CondEq));
  x86p_a64_emit_bind(e, zero);
  x86p_a64_emit_fmov_d_x(e, 1, BITS);
}

/* D0 = ST(i), rounded, SLOT its register; refused when empty. */
static void narrow_st(BlockCtx *c, X87Slow *s, unsigned i, int exact) {
  locate(c, i);
  require_occupied(c, s);
  if (exact) {
    require_exact(c, s);
  }
  call_narrow(c, s);
}

/* The C0..C3 a compare of D0 with D1 leaves: C0 less, C3 equal, C1 and C2
   clear. The operands are ordered numbers, so FCMP's MI and EQ are exactly
   those two relations. */
static void compare_status(BlockCtx *c) {
  X86pA64Emit *e = c->e;
  x86p_a64_emit_fcmp_d(e, 0, 1);
  x86p_a64_emit_load16_zx(e, kA64X0, X87B, STATUS_OFF);
  x86p_a64_emit_alu_w_imm(e, kA64And, kA64X0, (uint16_t)~kCompareBits);
  x86p_a64_emit_cset_w(e, kA64CondMi, kA64X1);
  x86p_a64_emit_alu_w_w_lsl(e, kA64Orr, kA64X0, kA64X0, kA64X1, 8);
  x86p_a64_emit_cset_w(e, kA64CondEq, kA64X1);
  x86p_a64_emit_alu_w_w_lsl(e, kA64Orr, kA64X0, kA64X0, kA64X1, 14);
  x86p_a64_emit_store16_reg(e, X87B, STATUS_OFF, kA64X0);
}

static int float_memory(const X86pInsn *insn) {
  return insn->operand[0].kind == kX86pOperandMem && !insn->x87_mem_int && insn->x87 != kX86pX87InsnLoadInt &&
         insn->x87 != kX86pX87InsnStoreInt;
}

/* ---- the forms ----------------------------------------------------------------- */

int emit_x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  const X86pOperand *o0 = &insn->operand[0];
  const int memory = o0->kind == kX86pOperandMem;
  const unsigned dst = insn->operands == 2 ? (unsigned)o0->reg : 0u;
  const unsigned src = insn->operands == 2 ? (unsigned)insn->operand[1].reg : (unsigned)o0->reg;
  if ((memory && !float_memory(insn)) || insn->x87_op > (uint8_t)kX86pX87Div || !routines_reserved(c)) {
    return 0;
  }
  s->fast = 1;
  begin(c);
  require_double_mode(c, s);
  if (memory) {
    operand_to_d1(c, s, o0->size);
  } else {
    narrow_st(c, s, src, 0);
    x86p_a64_emit_fmov_d_d(c->e, 1, 0);
  }
  narrow_st(c, s, dst, 0);
  if (insn->x87_reverse) {
    x86p_a64_emit_fop_d(c->e, (X86pA64FpOp)insn->x87_op, 0, 1, 0);
  } else {
    x86p_a64_emit_fop_d(c->e, (X86pA64FpOp)insn->x87_op, 0, 0, 1);
  }
  call_widen(c, s);
  if (insn->x87_pops) {
    emit_x87_pops(c, insn->x87_pops);
  }
  return 1;
}

int emit_x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  X86pA64Emit *e = c->e;
  if (!float_memory(insn) || !routines_reserved(c)) {
    return 0;
  }
  s->fast = 1;
  if (insn->operand[0].size == 4) {
    /* Exact, a binary32 subnormal included: it is a binary64 normal. */
    x86p_a64_emit_fmov_s_w(e, 0, BITS);
    x86p_a64_emit_fcvt_d_s(e, 0, 0);
  } else {
    x86p_a64_emit_fmov_d_x(e, 0, BITS);
  }
  begin(c);
  x86p_a64_emit_alu_w_imm(e, kA64Sub, TOP, 1u);
  locate(c, 0);
  require_empty(c, s);
  call_widen(c, s);
  x86p_a64_emit_alu_w_imm(e, kA64And, TOP, X86P_X87_REGS - 1u);
  x86p_a64_emit_store8_reg(e, X87B, TOP_OFF, TOP);
  mark_valid(c);
  return 1;
}

int emit_x87_inline_store(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  X86pA64Emit *e = c->e;
  if (!float_memory(insn) || !routines_reserved(c)) {
    return 0;
  }
  s->fast = 1;
  begin(c);
  require_nearest(c, s);
  narrow_st(c, s, 0, 0);
  if (insn->operand[0].size == 8) {
    x86p_a64_emit_fmov_x_d(e, CARRY_REG, 0);
    return 1;
  }
  {
    X86pA64EmitSite zero;
    x86p_a64_emit_fmov_x_d(e, kA64X0, 0);
    x86p_a64_emit_ubfx_x(e, kA64X0, kA64X0, 0, kF32TieBits);
    x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)kF32Tie);
    x86p_a64_emit_cmp_w_w(e, kA64X0, kA64X1);
    refuse(c, s, x86p_a64_emit_bcc(e, kA64CondEq));
    x86p_a64_emit_fcvt_s_d(e, 0, 0);
    x86p_a64_emit_fmov_w_s(e, CARRY_REG, 0);
    /* Not below binary32's normal range, where the rounding position moves
       up and the tie test above no longer covers it. An overflow needs no
       guard: it rounds to the same infinity from either width. */
    x86p_a64_emit_lsl_w_w_imm(e, kA64X0, CARRY_REG, 1);
    zero = x86p_a64_emit_cbz_w(e, kA64X0);
    x86p_a64_emit_lsr_w_imm(e, kA64X0, 24);
    refuse(c, s, x86p_a64_emit_cbz_w(e, kA64X0));
    x86p_a64_emit_bind(e, zero);
  }
  return 1;
}

int emit_x87_inline_compare_mem(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  if (!float_memory(insn) || !routines_reserved(c)) {
    return 0;
  }
  s->fast = 1;
  begin(c);
  operand_to_d1(c, s, insn->operand[0].size);
  narrow_st(c, s, 0, 1);
  compare_status(c);
  if (insn->x87_pops) {
    emit_x87_pops(c, insn->x87_pops);
  }
  return 1;
}

int emit_x87_inline_copy(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  X86pA64Emit *e = c->e;
  const unsigned index = (unsigned)insn->operand[0].reg;
  s->fast = 1;
  begin(c);
  if (insn->x87 == kX86pX87InsnLoad) {
    /* FLD ST(i): read before the push renumbers it. */
    locate(c, index);
    require_occupied(c, s);
    x86p_a64_emit_mov_x_x(e, SLOT2, SLOT);
    x86p_a64_emit_alu_w_imm(e, kA64Sub, TOP, 1u);
    x86p_a64_emit_alu_w_imm(e, kA64And, TOP, X86P_X87_REGS - 1u);
    locate(c, 0);
    require_empty(c, s);
    x86p_a64_emit_load_q_at(e, 0, SLOT2);
    x86p_a64_emit_store_q_at(e, SLOT, 0);
    x86p_a64_emit_store8_reg(e, X87B, TOP_OFF, TOP);
    mark_valid(c);
    return 1;
  }
  /* FST/FSTP ST(i). */
  locate(c, 0);
  require_occupied(c, s);
  x86p_a64_emit_mov_x_x(e, SLOT2, SLOT);
  locate(c, index);
  x86p_a64_emit_load_q_at(e, 0, SLOT2);
  x86p_a64_emit_store_q_at(e, SLOT, 0);
  mark_valid(c);
  if (insn->x87_pops) {
    emit_x87_pops(c, insn->x87_pops);
  }
  return 1;
}

int emit_x87_inline_register(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  X86pA64Emit *e = c->e;
  const unsigned index = insn->operands ? (unsigned)insn->operand[0].reg : 1u;
  switch (insn->x87) {
  case kX86pX87InsnCompare:
    if (!routines_reserved(c)) {
      return 0;
    }
    s->fast = 1;
    begin(c);
    narrow_st(c, s, index, 1);
    x86p_a64_emit_fmov_d_d(e, 1, 0);
    narrow_st(c, s, 0, 1);
    compare_status(c);
    if (insn->x87_pops) {
      emit_x87_pops(c, insn->x87_pops);
    }
    return 1;
  case kX86pX87InsnExchange:
    s->fast = 1;
    begin(c);
    locate(c, index);
    require_occupied(c, s);
    require_canonical(c, s, SLOT);
    x86p_a64_emit_mov_x_x(e, SLOT2, SLOT);
    locate(c, 0);
    require_occupied(c, s);
    require_canonical(c, s, SLOT);
    x86p_a64_emit_load_q_at(e, 0, SLOT);
    x86p_a64_emit_load_q_at(e, 1, SLOT2);
    x86p_a64_emit_store_q_at(e, SLOT, 1);
    x86p_a64_emit_store_q_at(e, SLOT2, 0);
    return 1;
  case kX86pX87InsnChangeSign:
  case kX86pX87InsnAbs:
    s->fast = 1;
    begin(c);
    locate(c, 0);
    require_occupied(c, s);
    require_canonical(c, s, SLOT);
    x86p_a64_emit_load16_zx(e, kA64X0, SLOT, 8);
    if (insn->x87 == kX86pX87InsnAbs) {
      x86p_a64_emit_alu_w_imm(e, kA64And, kA64X0, kExt80ExpMask);
    } else {
      x86p_a64_emit_alu_w_imm(e, kA64Eor, kA64X0, kExt80ExpMask + 1u);
    }
    x86p_a64_emit_store16_reg(e, SLOT, 8, kA64X0);
    return 1;
  default:
    return 0;
  }
}

#else

/* No ext80 register file to read in place: every form keeps its helper. */
int emit_x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  (void)c;
  (void)insn;
  (void)s;
  return 0;
}
int emit_x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  return emit_x87_inline_arith(c, insn, s);
}
int emit_x87_inline_store(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  return emit_x87_inline_arith(c, insn, s);
}
int emit_x87_inline_compare_mem(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  return emit_x87_inline_arith(c, insn, s);
}
int emit_x87_inline_copy(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  return emit_x87_inline_arith(c, insn, s);
}
int emit_x87_inline_register(BlockCtx *c, const X86pInsn *insn, X87Slow *s) {
  return emit_x87_inline_arith(c, insn, s);
}
void emit_x87_routines(BlockCtx *c) {
  (void)c;
}

#endif
