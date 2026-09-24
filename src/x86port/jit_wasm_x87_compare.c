/* See jit_wasm_x87_compare.h. */
#include "jit_wasm_x87_compare.h"

#include "jit_wasm_internal.h"
#include "jit_wasm_x87_load.h"
#include "jit_wasm_x87_slot.h"
#include "x87.h"
#include "x87_ext80_widen.h"

#if X86P_X87_BINARY128

/* The locals, by role. Each is reused once its first role is over, and the
   comment at each reuse names the change. */
enum {
  kRefused = kX86pWasmLocalB,           /* the refusal, then "magnitude below" */
  kTop = kX86pWasmLocalTarget,          /* ST(0)'s physical index */
  kTopSignExp = kX86pWasmLocalR,        /* ST(0)'s sign_exp, then "less" */
  kTopExp = kX86pWasmLocalCarry,        /* ST(0)'s exponent, then "magnitude above" */
  kOperandExp = kX86pWasmLocalA,        /* the operand's exponent, then ST(0)'s sign */
  kOperandSignExp = kX86pWasmLocalAddr, /* the operand's sign_exp, then "signs differ" */
  kTopSignif = kX86pWasmLocal64X,       /* ST(0)'s significand */
  kOperand = kX86pWasmLocal64Bits       /* the operand's bits, then its significand */
};

enum { kExt80ExpMax = 0x7FFF };

/* Alignment hints are zero throughout this backend; jit_wasm_state.c explains
   why a promise the guest does not make must not be emitted. */
#define ALIGN_NONE 0u

static void constant(X86pWasmLower *l, int32_t value) {
  x86p_wasm_i32_const(l->e, value);
}

static void get(X86pWasmLower *l, int local) {
  x86p_wasm_local_get(l->e, (uint32_t)local);
}

static void set(X86pWasmLower *l, int local) {
  x86p_wasm_local_set(l->e, (uint32_t)local);
}

static void op(X86pWasmLower *l, X86pWasmI32Op code) {
  x86p_wasm_i32_op(l->e, code);
}

/*
 * ST(0) into kTop, kTopSignif and kTopSignExp, and on the stack everything
 * about it the inline arm does not answer: an empty register, an infinity or
 * NaN, a zero exponent with a significand (a denormal or pseudo-denormal), and
 * a nonzero exponent without the explicit integer bit (an unnormal).
 */
static void top_refusal(X86pWasmLower *l) {
  x86p_wasm_x87_slot_index(l, 0, kTop);
  x86p_wasm_x87_slot_register(l, kTop);
  x86p_wasm_i64_load(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87Signif);
  set(l, kTopSignif);
  x86p_wasm_x87_slot_register(l, kTop);
  x86p_wasm_i32_load16_u(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87SignExp);
  x86p_wasm_local_tee(l->e, (uint32_t)kTopSignExp);
  constant(l, kExt80ExpMax);
  op(l, kWasmI32And);
  x86p_wasm_local_tee(l->e, (uint32_t)kTopExp);
  constant(l, kExt80ExpMax);
  op(l, kWasmI32Eq);
  op(l, kWasmI32Or);

  get(l, kTopExp);
  op(l, kWasmI32Eqz);
  get(l, kTopSignif);
  x86p_wasm_i64_eqz(l->e);
  op(l, kWasmI32Eqz);
  op(l, kWasmI32And);
  op(l, kWasmI32Or);

  get(l, kTopExp);
  constant(l, 0);
  op(l, kWasmI32Ne);
  get(l, kTopSignif);
  x86p_wasm_i64_const_shift(l->e, 63);
  x86p_wasm_i32_wrap_i64(l->e);
  op(l, kWasmI32Eqz);
  op(l, kWasmI32And);
  op(l, kWasmI32Or);
}

/* The path that existed before this file, from the operand bits it was
   already handed: the helper converts, orders and pops. */
static void call_helper(X86pWasmLower *l, const X86pInsn *insn, int width, uint32_t pc) {
  x86p_wasm_state_x87_addr(&l->state);
  get(l, kOperand);
  x86p_wasm_i32_wrap_i64(l->e);
  get(l, kOperand);
  x86p_wasm_i64_const(l->e, 32);
  x86p_wasm_i64_shr_u(l->e);
  x86p_wasm_i32_wrap_i64(l->e);
  constant(l, width);
  constant(l, 0); /* integer: this path is only a float operand */
  constant(l, (int32_t)insn->x87_pops);
  x86p_wasm_call_import(l, kX86pWasmImportX87CompareMemBits);
  op(l, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitUnsupported);
  x86p_wasm_end(l->e);
}

/* (e0 CMP e1) | (e0 == e1 & s0 CMP s1): one magnitude against the other, by
   the pair (exponent, significand). Canonical normals and zeros only, which is
   what makes the pair's order the magnitudes' order. */
static void magnitude(X86pWasmLower *l, X86pWasmI32Op exponents, int below) {
  get(l, kTopExp);
  get(l, kOperandExp);
  op(l, exponents);
  get(l, kTopExp);
  get(l, kOperandExp);
  op(l, kWasmI32Eq);
  get(l, kTopSignif);
  get(l, kOperand);
  if (below) {
    x86p_wasm_i64_lt_u(l->e);
  } else {
    x86p_wasm_i64_gt_u(l->e);
  }
  op(l, kWasmI32And);
  op(l, kWasmI32Or);
}

/* differ ? a_if_differ : (sign ? if_negative : if_positive), for one of the
   two answers. Each argument is a local except `a_if_differ`, which is ST(0)'s
   sign or its complement. */
static void order(X86pWasmLower *l, int complement_sign, int if_negative, int if_positive) {
  get(l, kOperandExp); /* now ST(0)'s sign */
  if (complement_sign) {
    op(l, kWasmI32Eqz);
  }
  get(l, if_negative);
  get(l, if_positive);
  get(l, kOperandExp);
  x86p_wasm_select(l->e);
  get(l, kOperandSignExp); /* now "signs differ" */
  x86p_wasm_select(l->e);
}

static void compare_ordered(X86pWasmLower *l, const X86pInsn *insn, int width) {
  /* The operand as ext80, from its bits and stored exponent. */
  x86p_wasm_x87_widened_sign_exp(l, width);
  set(l, kOperandSignExp);
  x86p_wasm_x87_widened_signif(l, width);
  set(l, kOperand);
  get(l, kOperandSignExp);
  constant(l, kExt80ExpMax);
  op(l, kWasmI32And);
  set(l, kOperandExp);

  /* Signs that differ decide the order by themselves -- unless both values
     are zeros, which are equal whatever their signs, and whose magnitudes
     compare equal below. kOperandSignExp becomes "signs differ". */
  get(l, kTopSignExp);
  get(l, kOperandSignExp);
  op(l, kWasmI32Xor);
  constant(l, 15);
  op(l, kWasmI32ShrU);
  get(l, kTopExp);
  get(l, kOperandExp);
  op(l, kWasmI32Or);
  constant(l, 0);
  op(l, kWasmI32Ne);
  op(l, kWasmI32And);
  set(l, kOperandSignExp);

  /* kRefused has no reader left on this arm and becomes "magnitude below";
     kTopExp's last reader is the second magnitude, so it becomes "above". */
  magnitude(l, kWasmI32LtU, 1);
  set(l, kRefused);
  magnitude(l, kWasmI32GtU, 0);
  set(l, kTopExp);

  /* kOperandExp becomes ST(0)'s sign, and kTopSignExp "less": a negative
     ST(0) is less exactly where its magnitude is above. */
  get(l, kTopSignExp);
  constant(l, 15);
  op(l, kWasmI32ShrU);
  set(l, kOperandExp);
  order(l, 0, kTopExp, kRefused);
  set(l, kTopSignExp);

  /* status = status & ~(C0|C1|C2|C3) | less << C0 | equal << C3, where
     "equal" is neither less nor greater. */
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_load16_u(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87Status);
  constant(l, (int32_t)(uint16_t)~(X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3));
  op(l, kWasmI32And);
  get(l, kTopSignExp);
  constant(l, 8);
  op(l, kWasmI32Shl);
  op(l, kWasmI32Or);
  order(l, 1, kRefused, kTopExp);
  get(l, kTopSignExp);
  op(l, kWasmI32Or);
  op(l, kWasmI32Eqz);
  constant(l, 14);
  op(l, kWasmI32Shl);
  op(l, kWasmI32Or);
  x86p_wasm_i32_store16(l->e, ALIGN_NONE, (uint32_t)kX86pWasmX87Status);

  if (insn->x87_pops != 0u) {
    x86p_wasm_x87_slot_retire(l, kTop);
  }
}

_Static_assert(X86P_X87_C0 == 1u << 8 && X86P_X87_C3 == 1u << 14, "the emitted comparison shifts C0 and C3 into place");

int x86p_wasm_x87_compare_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int width = (int)insn->operand[0].size;
  if (insn->x87 != kX86pX87InsnCompare || insn->x87_mem_int || insn->x87_pops > 1u ||
      x86p_ext80_source((unsigned)width).field == 0u) {
    return 0;
  }
  x86p_wasm_state_guard(&l->state, &insn->operand[0], pc, width, kX86pMemRead);
  x86p_wasm_x87_load_operand_bits(l, width);
  x86p_wasm_x87_operand_refusal(l, width);
  top_refusal(l);
  op(l, kWasmI32Or);
  x86p_wasm_if(l->e, kWasmVoid);
  call_helper(l, insn, width, pc);
  x86p_wasm_else(l->e);
  compare_ordered(l, insn, width);
  x86p_wasm_end(l->e);
  l->x87_compares_inline++;
  return 1;
}

#else

int x86p_wasm_x87_compare_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  /* No architectural pair to read: X86pX87Reg is the host's own long double
     here, and this backend's register file is that type. */
  (void)l;
  (void)insn;
  (void)pc;
  return 0;
}

#endif /* X86P_X87_BINARY128 */
