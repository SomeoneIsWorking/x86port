/* See jit_wasm_x87_load.h. */
#include "jit_wasm_x87_load.h"

#include "jit_wasm_internal.h"
#include "x87.h"
#include "x87_ext80_widen.h"

#include <stddef.h>

#if X86P_X87_BINARY128

/*
 * The store the emitted code makes is ONE i64 per half of the register, and
 * the second of them covers the padding as well as the field. That is not an
 * optimisation, it is the contract: reg_of_ext80 zeroes the whole object
 * before writing its two fields, and the differential compares the register
 * file with memcmp, so a fast path that left the padding alone would differ
 * from the helper on bytes no value depends on. Writing eight bytes at
 * `sign_exp` reproduces the zeroing exactly -- as long as the object really is
 * 16 bytes with the pair at 0 and 8, which is what these assert.
 */
_Static_assert(sizeof(X86pX87Reg) == 16u, "the emitted x87 load writes a 16-byte register");
_Static_assert(offsetof(X86pX87Reg, signif) == 0u, "the emitted x87 load writes the significand first");
_Static_assert(offsetof(X86pX87Reg, sign_exp) == 8u, "the emitted x87 load writes sign_exp and its padding as one i64");

/* Every offset here is relative to the X86pX87 that x87_base() leaves on the
   stack, so the one inside X86pCpu is the state helper's business and not
   repeated. */
enum {
  kRegOffset = (int)offsetof(X86pX87, reg),
  kTagOffset = (int)offsetof(X86pX87, tag),
  kTopOffset = (int)offsetof(X86pX87, top),
  kRegSize = (int)sizeof(X86pX87Reg),
  kSignifOffset = (int)offsetof(X86pX87Reg, signif),
  kSignExpOffset = (int)offsetof(X86pX87Reg, sign_exp)
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

void x86p_wasm_x87_load_operand_bits(X86pWasmLower *l, int width) {
  if (width == 4) {
    x86p_wasm_state_load_mem(&l->state, 4);
    x86p_wasm_i64_extend_i32_u(l->e);
    x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocal64Bits);
    return;
  }
  /* The pair arrives low half first, so the high half is what is on top and
     the low half has to wait in an i32 local while it is widened. */
  x86p_wasm_state_load_mem_pair(&l->state, 8);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_const(l->e, 32);
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_or(l->e);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocal64Bits);
}

/*
 * Everything the inline arm cannot do, as one i32 in kX86pWasmLocalB.
 *
 * Leaves the stored exponent in kX86pWasmLocalA and the destination's physical
 * index in kX86pWasmLocalTarget, because both arms need them: the inline one to
 * rebias and to store, and the cold one for nothing at all -- which is the
 * point, since the helper recomputes them from the bits it is handed and the
 * two cannot disagree about a case they are not sharing.
 */
static void classify_operand(X86pWasmLower *l, X86pExt80Source source) {
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, (int64_t)source.field);
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  constant(l, (int32_t)source.exp_max);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalA);
  /* A zero exponent is a zero or a subnormal and the all-ones one is an
     infinity or a NaN. Each needs a significand the shift below does not
     produce, and each is rare enough in the guest's geometry to be worth a
     call rather than three more branches here. */
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  constant(l, (int32_t)source.exp_max);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
  x86p_wasm_i32_op(l->e, kWasmI32Or);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);

  /* p = (top - 1) & 7, exactly as x86p_x87_push_raw computes it: TOP is a
     uint8_t, so a TOP of zero underflows to 7 rather than to a negative. */
  x87_base(l);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTopOffset);
  constant(l, 1);
  x86p_wasm_i32_op(l->e, kWasmI32Sub);
  constant(l, X86P_X87_REGS - 1);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalTarget);

  /* A destination that is not empty is a stack overflow, which sets three
     status bits and pushes nothing. The helper owns that. */
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_i32_load8_u(l->e, ALIGN_NONE, (uint32_t)kTagOffset);
  constant(l, (int32_t)kX86pX87TagEmpty);
  x86p_wasm_i32_op(l->e, kWasmI32Ne);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i32_op(l->e, kWasmI32Or);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalB);
}

/* The path that existed before this file: the operand's bits back out as the
   i32 pair the import takes, and the same refusal-by-name on a width the
   conversion does not know. */
static void call_helper(X86pWasmLower *l, int width, uint32_t pc) {
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i32_wrap_i64(l->e);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, 32);
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  constant(l, width);
  constant(l, 0); /* integer: this path is only reached for a float operand */
  constant(l, 0); /* pops: only the non-popping forms come here */
  x86p_wasm_call_import(l, kX86pWasmImportX87LoadBits);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitUnsupported);
  x86p_wasm_end(l->e);
}

/* The ordinary case: widen, push, tag valid. The three stores are what
   x86p_x87_push_raw does on its success path, in the order it does them. */
static void push_widened(X86pWasmLower *l, int width, X86pExt80Source source) {
  const int64_t mantissa_mask = (int64_t)((((uint64_t)1u << source.field) - 1u));
  const int64_t implicit_one = (int64_t)((uint64_t)1u << source.field);

  /* &reg[p], held because both halves are written through it. */
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  constant(l, kRegSize);
  x86p_wasm_i32_op(l->e, kWasmI32Mul);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalAddr);

  /* signif = (mant | 1 << field) << (63 - field): ext80 keeps the leading one
     explicitly, at the top of the significand, where the source formats imply
     it at the top of the fraction. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, mantissa_mask);
  x86p_wasm_i64_and(l->e);
  x86p_wasm_i64_const(l->e, implicit_one);
  x86p_wasm_i64_or(l->e);
  x86p_wasm_i64_const(l->e, (int64_t)(63u - source.field));
  x86p_wasm_i64_shl(l->e);
  x86p_wasm_i64_store(l->e, ALIGN_NONE, (uint32_t)(kRegOffset + kSignifOffset));

  /* sign_exp = sign << 15 | (exp - bias + EXT80_BIAS), and the i64 store puts
     the object's six padding bytes back to zero with it. */
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocal64Bits);
  x86p_wasm_i64_const(l->e, (int64_t)(width * 8 - 1));
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  constant(l, 15);
  x86p_wasm_i32_op(l->e, kWasmI32Shl);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  constant(l, (int32_t)(X86P_EXT80_BIAS - (int32_t)source.bias));
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_i32_op(l->e, kWasmI32Or);
  x86p_wasm_i64_extend_i32_u(l->e);
  x86p_wasm_i64_store(l->e, ALIGN_NONE, (uint32_t)(kRegOffset + kSignExpOffset));

  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTopOffset);

  /* The tag is VALID and not classify()'s answer, because the arm that got
     here already excluded both of the other two: an exponent that is neither
     zero nor all ones cannot produce an ext80 zero or special. */
  x87_base(l);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  constant(l, (int32_t)kX86pX87TagValid);
  x86p_wasm_i32_store8(l->e, ALIGN_NONE, (uint32_t)kTagOffset);
}

int x86p_wasm_x87_load_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int width = (int)insn->operand[0].size;
  const X86pExt80Source source = x86p_ext80_source((unsigned)width);
  if (insn->x87 != kX86pX87InsnLoad || insn->x87_mem_int || insn->x87_pops != 0u || source.field == 0u) {
    return 0;
  }
  x86p_wasm_state_guard(&l->state, &insn->operand[0], pc, width, kX86pMemRead);
  x86p_wasm_x87_load_operand_bits(l, width);
  classify_operand(l, source);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_if(l->e, kWasmVoid);
  call_helper(l, width, pc);
  x86p_wasm_else(l->e);
  push_widened(l, width, source);
  x86p_wasm_end(l->e);
  l->x87_loads_inline++;
  return 1;
}

#else

int x86p_wasm_x87_load_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  /* No architectural pair to store into: X86pX87Reg is the host's own long
     double here, and this backend's register file is that type. */
  (void)l;
  (void)insn;
  (void)pc;
  return 0;
}

#endif /* X86P_X87_BINARY128 */
