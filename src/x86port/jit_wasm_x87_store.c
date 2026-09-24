/* See jit_wasm_x87_store.h. */
#include "jit_wasm_x87_store.h"

#include "jit_wasm_internal.h"
#include "x87.h"
#include "x87_ext80_narrow.h"

#include <stddef.h>

#if X86P_X87_BINARY128

/* The same two assertions the load side makes, for the same reason: this
   reads the architectural pair straight out of the register file, so where
   the two fields sit is part of the contract rather than an assumption. */
_Static_assert(sizeof(X86pX87Reg) == 16u, "the emitted x87 store reads a 16-byte register");
_Static_assert(offsetof(X86pX87Reg, signif) == 0u, "the emitted x87 store reads the significand first");
_Static_assert(offsetof(X86pX87Reg, sign_exp) == 8u, "the emitted x87 store reads sign_exp above it");

enum {
  kRegOffset = (int)offsetof(X86pX87, reg),
  kTagOffset = (int)offsetof(X86pX87, tag),
  kTopOffset = (int)offsetof(X86pX87, top),
  kControlOffset = (int)offsetof(X86pX87, control),
  kRegSize = (int)sizeof(X86pX87Reg),
  kSignifOffset = (int)offsetof(X86pX87Reg, signif),
  kSignExpOffset = (int)offsetof(X86pX87Reg, sign_exp),
  kExt80ExpMax = 0x7FFF
};

/* Alignment hints are zero throughout this backend; jit_wasm_state.c explains
   why a promise the guest does not make must not be emitted. */
#define ALIGN_NONE 0u

static void x87_base(X86pWasmLower *l) {
  x86p_wasm_state_x87_addr(&l->state);
}

static void constant(X86pWasmLower *l, int32_t value) {
  x86p_wasm_i32_const(l->e, value);
}

/* `flag` is OR-ed into the running refusal in kX86pWasmLocalB. Each caller
   leaves one i32 on the stack and this consumes it, so the refusal is built
   as a chain of ordinary values with no branch in it. */
static void refuse_if(X86pWasmLower *l) {
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i32_op(l->e, kWasmI32Or);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
}

/* &f->reg[ST(0)], from the physical index classify() has already computed. */
static void register_field(X86pWasmLower *l) {
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  constant(l, kRegSize);
  x86p_wasm_i32_op(l->e, kWasmI32Mul);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
}

/*
 * ST(0)'s physical index into kX86pWasmLocalTarget, an empty ST(0) ADDED to the
 * refusal already in kX86pWasmLocalB -- which the caller has initialised from
 * the address verdict, and which must not be overwritten here -- and the
 * register's two fields: the significand in kX86pWasmLocal64Bits and sign_exp
 * in kX86pWasmLocalR.
 *
 * Nothing here depends on the value, so both the zero arm and the ordinary
 * one start from it.
 */
static void locate_top(X86pWasmLower *l) {
  /* p = top & 7, which is ST(0)'s physical register. */
  x87_base(l);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
  constant(l, X86P_X87_REGS - 1);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalTarget);

  /* An empty ST(0) is a stack underflow: three status bits and no store. */
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTagOffset);
  constant(l, (int32_t)kX86pX87TagEmpty);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
  refuse_if(l);

  /* The two fields of reg[p]. Its address is recomputed for the second rather
     than held in a local: kX86pWasmLocalAddr is the DESTINATION for the whole
     instruction, and there is no spare i32 local to keep a second address in.
     Three instructions is the price of not needing one. */
  register_field(l);
  x86p_wasm_i64_load(l->e, ALIGN_NONE, (uint32_t)(kRegOffset + kSignifOffset));
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocal64Bits);
  register_field(l);
  x86p_wasm_i32_load16_u(l->e, ALIGN_NONE, (uint32_t)(kRegOffset + kSignExpOffset));
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
}

/* One i32: the fields locate_top() read are a zero of either sign and nothing
   refused the store. A zero narrows exactly in every rounding mode, so the RC
   field is not consulted for it. */
static void zero_and_permitted(X86pWasmLower *l) {
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalR);
  constant(l, kExt80ExpMax);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_eqz(l->e);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_i32_op(l->e, kWasmI32And);
}

/*
 * Everything about the value and the machine that can refuse the ordinary
 * arm, ADDED to kX86pWasmLocalB.
 *
 * It leaves the TARGET's biased exponent -- already rebiased from ext80 -- in
 * kX86pWasmLocalA, because the emitted store needs it and the helper
 * recomputes everything from the register file, so the two arms cannot
 * disagree about a case they do not share.
 */
static void classify(X86pWasmLower *l, X86pExt80Source target) {
  /* The RC field. Only round-to-nearest is this path's, and #162 measured the
     guest moving RC on 1.65% of its operations, so the other three are real. */
  x87_base(l);
  x86p_wasm_i32_load16_u(l->e, ALIGN_NONE, (uint32_t)kControlOffset);
  constant(l, (int32_t)X86P_X87_RC_MASK);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  constant(l, (int32_t)X86P_X87_RC_NEAREST);
  x86p_wasm_i32_op(l->e, kWasmI32Ne);
  refuse_if(l);

  /* An unnormal -- a stored exponent with no explicit integer bit -- is an
     invalid encoding rather than a value, and is refused before the exponent
     is even looked at, exactly as x86p_ext80_narrow_nearest refuses it. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const_shift(l->e, 63);
  x86p_wasm_i32_wrap_i64(l->e);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  refuse_if(l);

  /* The stored exponent, and the two classes it alone rules out: zero or
     subnormal, and infinity or NaN. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalR);
  constant(l, kExt80ExpMax);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  refuse_if(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  constant(l, kExt80ExpMax);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
  refuse_if(l);

  /* Rebias into the target format, and refuse everything that is not one of
     its normals: a subnormal result below, an infinity above. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  constant(l, (int32_t)target.bias - X86P_EXT80_BIAS);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalA);
  constant(l, 1);
  x86p_wasm_i32_op(l->e, kWasmI32LtS);
  refuse_if(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  constant(l, (int32_t)target.exp_max - 1);
  x86p_wasm_i32_op(l->e, kWasmI32GtS);
  refuse_if(l);
}

/*
 * Round to nearest, ties to even, leaving the rounded significand in
 * kX86pWasmLocal64Bits and refusing a carry out of it.
 *
 * The tie correction is a mask rather than a branch: when the discarded part
 * is EXACTLY half an ulp the result's new least significant bit is cleared,
 * which is what sends the tie to even, and `tie << drop` is that bit. The
 * whole sequence runs on the operand stack, with the unrounded significand
 * read out of its local twice, because there is no spare i64 local to hold an
 * intermediate in and a second one would be a zeroed slot in every block body.
 *
 * A carry means the significand became 2^64, which is the next exponent with
 * an empty fraction and may have left the target's range. x87_ext80_narrow.h
 * refuses it in C for the same reason this does here: one acceptance rule,
 * implemented twice, is only safe while the two rules are the same one.
 */
static void round_to_nearest(X86pWasmLower *l, X86pExt80Source target) {
  const int64_t drop = (int64_t)(63u - target.field);
  const int64_t half = (int64_t)((uint64_t)1u << (63u - target.field - 1u));
  const int64_t below = (int64_t)(((uint64_t)1u << (63u - target.field)) - 1u);

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, half);
  x86p_wasm_i64_add(l->e);

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, below);
  x86p_wasm_i64_and(l->e);
  x86p_wasm_i64_const(l->e, half);
  x86p_wasm_i64_eq(l->e);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_const(l->e, drop);
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_const(l->e, -1);
  x86p_wasm_i64_xor(l->e);

  x86p_wasm_i64_and(l->e);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocal64Bits);

  /* The carry test needs no comparison against the value before the add: the
     significand had its top bit set -- classify() refused every value that did
     not -- so a sum that no longer has it is the one that wrapped. */
  x86p_wasm_i64_const_shift(l->e, 63);
  x86p_wasm_i32_wrap_i64(l->e);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  refuse_if(l);
}

/* The path that existed before this file: the address, the guard's verdict and
   the widths, then the import that converts and writes. */
static void call_helper(X86pWasmLower *l, const X86pInsn *insn, int width, uint32_t pc) {
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalCarry);
  x86p_wasm_i32_const(l->e, width);
  x86p_wasm_i32_const(l->e, 0); /* integer: this path is only a float store */
  x86p_wasm_i32_const(l->e, (int32_t)insn->x87_pops);
  x86p_wasm_call_import(l, kX86pWasmImportX87StoreAt);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitMemoryFault);
  x86p_wasm_end(l->e);
}

/* The i64 of the destination's bits on the stack, at kX86pWasmLocalAddr. */
static void store_bits(X86pWasmLower *l, int width) {
  if (width == 4) {
    x86p_wasm_i32_wrap_i64(l->e);
    x86p_wasm_i32_store(l->e, ALIGN_NONE, 0u);
  } else {
    x86p_wasm_i64_store(l->e, ALIGN_NONE, 0u);
  }
}

/* The register's sign, as the destination's top bit, on the stack as an i64. */
static void sign_bit(X86pWasmLower *l, int width) {
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalR);
  constant(l, 15);
  x86p_wasm_i32_op(l->e, kWasmI32ShrU);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_const(l->e, (int64_t)(width * 8 - 1));
  x86p_wasm_i64_shl(l->e);
}

/* The pop x86p_x87_pop_raw performs, in the order it performs it: the slot
   becomes empty and TOP moves up one. */
static void retire_top(X86pWasmLower *l, const X86pInsn *insn) {
  if (insn->x87_pops == 0u) {
    return;
  }
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, (int32_t)kX86pX87TagEmpty);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTagOffset);
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  constant(l, 1);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, X86P_X87_REGS - 1);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
}

/* A zero: the sign alone, and the slot retired. */
static void store_zero(X86pWasmLower *l, const X86pInsn *insn, int width) {
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalAddr);
  sign_bit(l, width);
  store_bits(l, width);
  retire_top(l, insn);
}

/* The ordinary case: assemble the bits, write them, and retire the slot. */
static void store_narrowed(X86pWasmLower *l, const X86pInsn *insn, int width, X86pExt80Source target) {
  const int64_t fraction_mask = (int64_t)((((uint64_t)1u << target.field) - 1u));

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalAddr);
  /* sign, from the register's sign bit, at the top of the destination. */
  sign_bit(l, width);
  /* the exponent field, already rebiased and proved to be a normal. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_const(l->e, (int64_t)target.field);
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_or(l->e);
  /* and the fraction: ext80's explicit leading one is the format's implicit
     one, so what is stored is everything below it. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, (int64_t)(63u - target.field));
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i64_const(l->e, fraction_mask);
  x86p_wasm_i64_and(l->e);
  x86p_wasm_i64_or(l->e);
  store_bits(l, width);
  retire_top(l, insn);
}

int x86p_wasm_x87_store_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int width = (int)insn->operand[0].size;
  const X86pExt80Source target = x86p_ext80_source((unsigned)width);
  if (insn->x87 != kX86pX87InsnStore || insn->x87_mem_int || insn->x87_pops > 1u || target.field == 0u) {
    return 0;
  }
  if (!x86p_wasm_state_memory_is_direct(&l->state)) {
    return 0;
  }
  /* The guard's verdict travels as a value, as it does for the helper: the
     conversion happens before the fault is reported, and this path refuses
     rather than reordering that. */
  x86p_wasm_state_check(&l->state, &insn->operand[0], width, kX86pMemWrite);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalCarry);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
  locate_top(l);
  zero_and_permitted(l);
  x86p_wasm_if(l->e, kWasmVoid);
  store_zero(l, insn, width);
  x86p_wasm_else(l->e);
  classify(l, target);
  round_to_nearest(l, target);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_if(l->e, kWasmVoid);
  call_helper(l, insn, width, pc);
  x86p_wasm_else(l->e);
  store_narrowed(l, insn, width, target);
  x86p_wasm_end(l->e);
  x86p_wasm_end(l->e);
  l->x87_stores_inline++;
  return 1;
}

#else

int x86p_wasm_x87_store_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  /* No architectural pair to read: X86pX87Reg is the host's own long double
     here, and this backend's register file is that type. */
  (void)l;
  (void)insn;
  (void)pc;
  return 0;
}

#endif /* X86P_X87_BINARY128 */
