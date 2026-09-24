/* See jit_wasm_x87_arith.h. */
#include "jit_wasm_x87_arith.h"

#include "jit_wasm_internal.h"
#include "jit_wasm_x87_load.h"
#include "jit_wasm_x87_slot.h"
#include "x87.h"
#include "x87_ext80_widen.h"

#include <stddef.h>
#include <stdint.h>

/* The emitted code reads the census pointer as an i32, which it is where the
   code runs: wasm32. A 64-bit host lowering for its own tests calls the helper. */
#if X86P_X87_BINARY128 && UINTPTR_MAX == UINT32_MAX

_Static_assert(X86P_X87_RC_NEAREST == 0u && X86P_X87_PC_SINGLE == 0u,
               "the emitted control test reads nearest and single precision as zero fields");
_Static_assert(sizeof(((X86pX87 *)0)->op_census) == 4u, "the emitted census test loads a 32-bit pointer");
_Static_assert(kX86pX87Add == 0 && kWasmF64Sub == kWasmF64Add + kX86pX87Sub &&
                   kWasmF64Mul == kWasmF64Add + kX86pX87Mul && kWasmF64Div == kWasmF64Add + kX86pX87Div,
               "an x87 operation is its f64 opcode's offset from f64.add");

/* Every field is addressed from the cpu local, the unit's own offset folded
   into the instruction's immediate: forming the unit's address once per access
   was a sixth of this instruction's bytes. */
enum {
  kX87 = (int)offsetof(X86pCpu, x87),
  kDoubleOffset = kX87 + (int)offsetof(X86pX87, double_arith),
  kCensusOffset = kX87 + (int)offsetof(X86pX87, op_census)
};

/* binary64's fields, and the ext80 exponents whose narrowing is one
   conversion and one exact scaling: unbiased -959 (so the scale 2^(e-63) is
   still a normal) to 1022 (so rounding up cannot reach infinity). */
enum {
  kF64FieldBits = 52,
  kF64ExpMax = 0x7FF,
  kF64Bias = 1023,
  kExt80ExpMask = 0x7FFF,
  kNarrowLowest = X86P_EXT80_BIAS - 959,
  kNarrowSpan = 1022 + 959,
  kF32FieldBits = 23,
  kF32ExpMax = 0xFF
};

#define ALIGN_NONE 0u

/* The locals, by role. The accumulating refusal stays on the operand stack. */
enum {
  kDst = kX86pWasmLocalTarget, /* the destination's physical index */
  kSrc = kX86pWasmLocalA,      /* a register source's physical index */
  kSignExp = kX86pWasmLocalR,  /* an ext80 operand's sign and exponent */
  kExponent = kX86pWasmLocalCarry,
  kRefused = kX86pWasmLocalB
};

static void cpu(X86pWasmLower *l) {
  x86p_wasm_state_cpu(&l->state);
}

static void constant(X86pWasmLower *l, int32_t value) {
  x86p_wasm_i32_const(l->e, value);
}

static void get(X86pWasmLower *l, int local) {
  x86p_wasm_local_get(l->e, (uint32_t)local);
}

static void set(X86pWasmLower *l, int local) {
  x86p_wasm_local_set(l->e, (uint32_t)local);
}

static void or_refused(X86pWasmLower *l) {
  x86p_wasm_i32_op(l->e, kWasmI32Or);
}

/* phys(i) = (top + i) & 7 into `local`, and whether that register is empty
   ORed into the refusal. */
static void physical(X86pWasmLower *l, unsigned index, int local) {
  x86p_wasm_x87_slot_index(l, (int)index, local);
  or_refused(l);
}

/* &reg[phys], from the index in `local`, into kX86pWasmLocalAddr. */
static void register_address(X86pWasmLower *l, int local) {
  x86p_wasm_x87_slot_register(l, local);
  set(l, kX86pWasmLocalAddr);
}

/* The gate the helper would otherwise decide: the mode, the census and the
   control word. Leaves the refusal on the stack. */
static void gate(X86pWasmLower *l) {
  cpu(l);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kDoubleOffset);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  cpu(l);
  x86p_wasm_i32_load(l->e, ALIGN_NONE, (uint32_t)kCensusOffset);
  or_refused(l);
  cpu(l);
  x86p_wasm_i32_load16_u(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87Control);
  x86p_wasm_local_tee(l->e, (uint32_t)kSignExp);
  constant(l, (int32_t)X86P_X87_RC_MASK);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  or_refused(l);
  get(l, kSignExp);
  constant(l, (int32_t)X86P_X87_PC_MASK);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  or_refused(l);
}

/*
 * The ext80 register at physical index `local` as binary64 bits in `out`, and
 * whether it is not a zero or a normal in the exponents this narrows ORed into
 * the refusal.
 */
static void narrow_register(X86pWasmLower *l, int local, int out) {
  register_address(l, local);
  get(l, kX86pWasmLocalAddr);
  x86p_wasm_i64_load(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87Signif);
  set(l, out);
  get(l, kX86pWasmLocalAddr);
  x86p_wasm_i32_load16_u(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87SignExp);
  x86p_wasm_local_tee(l->e, (uint32_t)kSignExp);
  constant(l, kExt80ExpMask);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  set(l, kExponent);

  /* zero: significand and exponent both zero */
  get(l, out);
  x86p_wasm_i64_const(l->e, 0);
  x86p_wasm_i64_eq(l->e);
  get(l, kExponent);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  /* normal: the explicit integer bit, and the exponent in range */
  get(l, out);
  x86p_wasm_i64_const(l->e, 63);
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  get(l, kExponent);
  constant(l, kNarrowLowest);
  x86p_wasm_i32_op(l->e, kWasmI32Sub);
  constant(l, kNarrowSpan);
  x86p_wasm_i32_op(l->e, kWasmI32LeU);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_op(l->e, kWasmI32Or);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  or_refused(l);

  /* |value| = significand * 2^(e - bias - 63). A zero's scale is masked into
     a finite one, so the product is a zero rather than 0 * infinity. */
  get(l, out);
  x86p_wasm_f64_convert_i64_u(l->e);
  get(l, kExponent);
  constant(l, kF64Bias - X86P_EXT80_BIAS - 63);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, kF64ExpMax);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_const(l->e, kF64FieldBits);
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_f64_reinterpret_i64(l->e);
  x86p_wasm_f64_op(l->e, kWasmF64Mul);
  x86p_wasm_i64_reinterpret_f64(l->e);
  /* The sign, which the product of two positives does not have. */
  get(l, kSignExp);
  constant(l, 15);
  x86p_wasm_i32_op(l->e, kWasmI32ShrU);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_const(l->e, 63);
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_or(l->e);
  set(l, out);
}

/*
 * Whether the binary64 bits in `bits`, of a format with `field` fraction bits
 * and exponent maximum `exp_max`, are an infinity, a NaN or a subnormal: ORed
 * into the refusal, with the exponent left in kExponent.
 */
static void refuse_unordinary(X86pWasmLower *l, int bits, unsigned field, int32_t exp_max) {
  get(l, bits);
  x86p_wasm_i64_const(l->e, (int64_t)field);
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  constant(l, exp_max);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_tee(l->e, (uint32_t)kExponent);
  constant(l, exp_max);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
  or_refused(l);
  /* a zero exponent with a fraction: shifted to the top, the fraction is all
     that is left of a zero-extended operand */
  get(l, kExponent);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  get(l, bits);
  x86p_wasm_i64_const(l->e, (int64_t)(64u - field));
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_const(l->e, 0);
  x86p_wasm_i64_eq(l->e);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  or_refused(l);
}

/* A binary32 or binary64 memory operand, from kX86pWasmLocal64Bits into
   kX86pWasmLocal64Y as binary64 bits. */
static void memory_source(X86pWasmLower *l, int width) {
  if (width == 8) {
    refuse_unordinary(l, kX86pWasmLocal64Bits, kF64FieldBits, kF64ExpMax);
    get(l, kX86pWasmLocal64Bits);
  } else {
    refuse_unordinary(l, kX86pWasmLocal64Bits, kF32FieldBits, kF32ExpMax);
    get(l, kX86pWasmLocal64Bits);
    x86p_wasm_i32_wrap_i64(l->e);
    x86p_wasm_f32_reinterpret_i32(l->e);
    x86p_wasm_f64_promote_f32(l->e);
    x86p_wasm_i64_reinterpret_f64(l->e);
  }
  set(l, kX86pWasmLocal64Y);
}

static int computes(const X86pInsn *insn) {
  const X86pOperand *first = &insn->operand[0];
  if (insn->x87 != kX86pX87InsnArith || insn->x87_op >= (uint8_t)kX86pX87OpCount) {
    return 0;
  }
  if (insn->operands && first->kind == kX86pOperandMem) {
    return !insn->x87_mem_int && (first->size == 4u || first->size == 8u);
  }
  return 1;
}

/* The indices the helper is given, and so the ones this reads. */
static unsigned destination(const X86pInsn *insn) {
  return insn->operands == 2 ? (unsigned)insn->operand[0].reg : 0u;
}

static unsigned register_source(const X86pInsn *insn) {
  return insn->operands == 2 ? (unsigned)insn->operand[1].reg : (insn->operands ? (unsigned)insn->operand[0].reg : 1u);
}

int x86p_wasm_x87_arith_begin(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *first = &insn->operand[0];
  const int memory = insn->operands && first->kind == kX86pOperandMem;
  if (!computes(insn)) {
    return 0;
  }
  if (memory) {
    x86p_wasm_state_guard(&l->state, first, pc, (int)first->size, kX86pMemRead);
    x86p_wasm_x87_load_operand_bits(l, (int)first->size);
  }
  gate(l);
  physical(l, destination(insn), kDst);
  if (memory) {
    memory_source(l, (int)first->size);
  } else {
    physical(l, register_source(insn), kSrc);
    narrow_register(l, kSrc, kX86pWasmLocal64Y);
  }
  narrow_register(l, kDst, kX86pWasmLocal64X);

  /* x op y, where `reverse` swaps the operands and nothing else. */
  get(l, insn->x87_reverse ? kX86pWasmLocal64Y : kX86pWasmLocal64X);
  x86p_wasm_f64_reinterpret_i64(l->e);
  get(l, insn->x87_reverse ? kX86pWasmLocal64X : kX86pWasmLocal64Y);
  x86p_wasm_f64_reinterpret_i64(l->e);
  x86p_wasm_f64_op(l->e, (X86pWasmF64Op)(kWasmF64Add + (int)insn->x87_op));
  x86p_wasm_i64_reinterpret_f64(l->e);
  set(l, kX86pWasmLocal64X);
  refuse_unordinary(l, kX86pWasmLocal64X, kF64FieldBits, kF64ExpMax);

  x86p_wasm_local_tee(l->e, (uint32_t)kRefused);
  x86p_wasm_if(l->e, kWasmVoid);
  return 1;
}

void x86p_wasm_x87_arith_end(X86pWasmLower *l, const X86pInsn *insn) {
  x86p_wasm_else(l->e);
  register_address(l, kDst);

  /* signif = e ? fraction << 11 | 1 << 63 : 0, where the explicit one
     replaces the exponent bit the shift brought to the top */
  get(l, kX86pWasmLocalAddr);
  get(l, kX86pWasmLocal64X);
  x86p_wasm_i64_const(l->e, 63 - kF64FieldBits);
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_const(l->e, INT64_MIN);
  x86p_wasm_i64_or(l->e);
  x86p_wasm_i64_const(l->e, 0);
  get(l, kExponent);
  x86p_wasm_select(l->e);
  x86p_wasm_i64_store(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87Signif);

  /* sign_exp = sign << 15 | (e ? e + rebias : 0), and its padding zeroed */
  get(l, kX86pWasmLocalAddr);
  get(l, kX86pWasmLocal64X);
  x86p_wasm_i64_const(l->e, 63);
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  constant(l, 15);
  x86p_wasm_i32_op(l->e, kWasmI32Shl);
  get(l, kExponent);
  constant(l, X86P_EXT80_BIAS - kF64Bias);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, 0);
  get(l, kExponent);
  x86p_wasm_select(l->e);
  x86p_wasm_i32_op(l->e, kWasmI32Or);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_store(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87SignExp);

  x86p_wasm_x87_slot_set_tag(l, kDst, kX86pX87TagValid);

  /* The pops, which cannot underflow: a popping form reads ST(0). */
  for (unsigned pop = 0; pop < insn->x87_pops; pop++) {
    x86p_wasm_x87_slot_pop(l, kSrc);
  }
  x86p_wasm_end(l->e);
}

#else

int x86p_wasm_x87_arith_begin(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  /* No architectural pair to read or write: X86pX87Reg is the host's own long
     double here. */
  (void)l;
  (void)insn;
  (void)pc;
  return 0;
}

void x86p_wasm_x87_arith_end(X86pWasmLower *l, const X86pInsn *insn) {
  (void)l;
  (void)insn;
}

#endif /* X86P_X87_BINARY128 && UINTPTR_MAX == UINT32_MAX */
