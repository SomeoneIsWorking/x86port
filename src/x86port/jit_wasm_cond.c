#include "jit_wasm_cond.h"

#include "cpu.h"
#include "jit_wasm_internal.h"

#include <stddef.h>

/*
 * Every field is read through this, which left-shifts an 8- or 16-bit operand
 * into the top of the word. That does two jobs with one instruction. It makes
 * the SIGNED comparisons right at every width, because an 8-bit 0xFF has to
 * compare as -1 and not as 255. And it makes the whole file independent of
 * whether the recorded operands had their high bits cleared: x86p_alu does
 * mask what it records, but a derivation that is only correct because of that
 * is a derivation coupled to another file's implementation detail, and this
 * one costs nothing at the 32-bit width that dominates.
 */
/* The shift that puts an operand of the recorded width at the top of the
   word; constants compared with such an operand are normalised the same way. */
static int norm_shift(const X86pWasmLower *l) {
  return (l->last_w >= 1 && l->last_w <= 4) ? 32 - 8 * l->last_w : 0;
}

static void push_norm(X86pWasmLower *l, size_t field) {
  const int shift = norm_shift(l);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + field));
  if (shift != 0) {
    x86p_wasm_i32_const(l->e, shift);
    x86p_wasm_i32_op(l->e, kWasmI32Shl);
  }
}

static void push_a(X86pWasmLower *l) {
  push_norm(l, offsetof(X86pFlags, a));
}

static void push_b(X86pWasmLower *l) {
  push_norm(l, offsetof(X86pFlags, b));
}

static void push_r(X86pWasmLower *l) {
  push_norm(l, offsetof(X86pFlags, r));
}

/* Compare the normalised result against zero, which is how every kind spells
   ZF, SF and the signed conditions that reduce to one of them. */
static void r_against_zero(X86pWasmLower *l, X86pWasmI32Op op) {
  push_r(l);
  x86p_wasm_i32_const(l->e, 0);
  x86p_wasm_i32_op(l->e, op);
}

static void a_against_b(X86pWasmLower *l, X86pWasmI32Op op) {
  push_a(l);
  push_b(l);
  x86p_wasm_i32_op(l->e, op);
}

/*
 * Parity is the odd one and stays odd here: it is the parity of the LOW BYTE
 * whatever the operand width, so this reads `r` unnormalised. `popcnt` gives
 * the bit count, `& 1` gives odd-or-even, and PF is set when that is EVEN --
 * which is why the affirmative form ends in eqz and the negative one does not.
 */
static void push_parity(X86pWasmLower *l, int negate) {
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, r)));
  x86p_wasm_i32_const(l->e, 0xFF);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_op(l->e, kWasmI32Popcnt);
  x86p_wasm_i32_const(l->e, 1);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  if (!negate) {
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  }
}

/*
 * SUB's overflow, as the architecture states it: the operands differed in sign
 * and the result took the second operand's. `(a^b) & (a^r)` is negative in
 * exactly those cases, which is one expression instead of three sign
 * extractions and two comparisons.
 */
static void push_sub_overflow(X86pWasmLower *l, X86pWasmI32Op op) {
  push_a(l);
  push_b(l);
  x86p_wasm_i32_op(l->e, kWasmI32Xor);
  push_a(l);
  push_r(l);
  x86p_wasm_i32_op(l->e, kWasmI32Xor);
  x86p_wasm_i32_op(l->e, kWasmI32And);
  x86p_wasm_i32_const(l->e, 0);
  x86p_wasm_i32_op(l->e, op);
}

/*
 * After a subtraction the signed conditions are the signed comparison of the
 * operands themselves -- SF != OF IS a < b -- and the unsigned ones are the
 * unsigned comparison. Deriving SF and OF separately and combining them would
 * be the same answer through five more instructions.
 */
static int emit_sub(X86pWasmLower *l, X86pCond cc) {
  switch (cc) {
  case kX86pCondO:
    push_sub_overflow(l, kWasmI32LtS);
    return 1;
  case kX86pCondNO:
    push_sub_overflow(l, kWasmI32GeS);
    return 1;
  case kX86pCondB:
    a_against_b(l, kWasmI32LtU);
    return 1;
  case kX86pCondNB:
    a_against_b(l, kWasmI32GeU);
    return 1;
  case kX86pCondZ:
    push_r(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    return 1;
  case kX86pCondNZ:
    r_against_zero(l, kWasmI32Ne);
    return 1;
  case kX86pCondBE:
    a_against_b(l, kWasmI32LeU);
    return 1;
  case kX86pCondA:
    a_against_b(l, kWasmI32GtU);
    return 1;
  case kX86pCondS:
    r_against_zero(l, kWasmI32LtS);
    return 1;
  case kX86pCondNS:
    r_against_zero(l, kWasmI32GeS);
    return 1;
  case kX86pCondP:
    push_parity(l, 0);
    return 1;
  case kX86pCondNP:
    push_parity(l, 1);
    return 1;
  case kX86pCondL:
    a_against_b(l, kWasmI32LtS);
    return 1;
  case kX86pCondGE:
    a_against_b(l, kWasmI32GeS);
    return 1;
  case kX86pCondLE:
    a_against_b(l, kWasmI32LeS);
    return 1;
  case kX86pCondG:
    a_against_b(l, kWasmI32GtS);
    return 1;
  case kX86pCondCount:
  default:
    return 0;
  }
}

/*
 * AND, OR, XOR and TEST clear CF and OF outright, so every condition here is
 * the result against zero and four of them are a constant the branch can be
 * folded around. With OF always 0, "SF != OF" is SF and "ZF or SF != OF" is
 * "result <= 0" -- the signed comparisons against zero say it directly.
 */
static int emit_logic(X86pWasmLower *l, X86pCond cc) {
  switch (cc) {
  case kX86pCondO:
  case kX86pCondB:
    x86p_wasm_i32_const(l->e, 0);
    return 1;
  case kX86pCondNO:
  case kX86pCondNB:
    x86p_wasm_i32_const(l->e, 1);
    return 1;
  case kX86pCondZ:
  case kX86pCondBE:
    push_r(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    return 1;
  case kX86pCondNZ:
  case kX86pCondA:
    r_against_zero(l, kWasmI32Ne);
    return 1;
  case kX86pCondS:
  case kX86pCondL:
    r_against_zero(l, kWasmI32LtS);
    return 1;
  case kX86pCondNS:
  case kX86pCondGE:
    r_against_zero(l, kWasmI32GeS);
    return 1;
  case kX86pCondP:
    push_parity(l, 0);
    return 1;
  case kX86pCondNP:
    push_parity(l, 1);
    return 1;
  case kX86pCondLE:
    r_against_zero(l, kWasmI32LeS);
    return 1;
  case kX86pCondG:
    r_against_zero(l, kWasmI32GtS);
    return 1;
  case kX86pCondCount:
  default:
    return 0;
  }
}

/*
 * ADD's overflow: both operands had the sign the result does not. As a sign
 * bit, `(a^r) & (b^r)`, which is left on the stack for the caller to compare.
 */
static void push_add_overflow_bits(X86pWasmLower *l) {
  push_a(l);
  push_r(l);
  x86p_wasm_i32_op(l->e, kWasmI32Xor);
  push_b(l);
  push_r(l);
  x86p_wasm_i32_op(l->e, kWasmI32Xor);
  x86p_wasm_i32_op(l->e, kWasmI32And);
}

/* SF != OF after an addition, as a sign bit: r ^ overflow-bits. That is the
   sign of the sum had it not been truncated, which is what JL asks. */
static void push_add_less(X86pWasmLower *l) {
  push_r(l);
  push_add_overflow_bits(l);
  x86p_wasm_i32_op(l->e, kWasmI32Xor);
  x86p_wasm_i32_const(l->e, 0);
  x86p_wasm_i32_op(l->e, kWasmI32LtS);
}

/* The carry an addition produced: the truncated sum wrapped below an operand. */
static void push_add_carry(X86pWasmLower *l, X86pWasmI32Op op) {
  push_r(l);
  push_a(l);
  x86p_wasm_i32_op(l->e, op);
}

/*
 * After an ADD the unsigned conditions come from the carry (r < a) and the
 * signed ones from the sign the untruncated sum would have had. Unlike SUB,
 * neither is a direct comparison of the operands, so BE, A, LE and G combine
 * two answers with a zero test.
 */
static int emit_add(X86pWasmLower *l, X86pCond cc) {
  switch (cc) {
  case kX86pCondO:
  case kX86pCondNO:
    push_add_overflow_bits(l);
    x86p_wasm_i32_const(l->e, 0);
    x86p_wasm_i32_op(l->e, cc == kX86pCondO ? kWasmI32LtS : kWasmI32GeS);
    return 1;
  case kX86pCondB:
    push_add_carry(l, kWasmI32LtU);
    return 1;
  case kX86pCondNB:
    push_add_carry(l, kWasmI32GeU);
    return 1;
  case kX86pCondBE:
    push_add_carry(l, kWasmI32LtU);
    push_r(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    x86p_wasm_i32_op(l->e, kWasmI32Or);
    return 1;
  case kX86pCondA:
    push_add_carry(l, kWasmI32GeU);
    r_against_zero(l, kWasmI32Ne);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    return 1;
  case kX86pCondL:
    push_add_less(l);
    return 1;
  case kX86pCondGE:
    push_add_less(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    return 1;
  case kX86pCondLE:
    push_add_less(l);
    push_r(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    x86p_wasm_i32_op(l->e, kWasmI32Or);
    return 1;
  case kX86pCondG:
    push_add_less(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    r_against_zero(l, kWasmI32Ne);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    return 1;
  default:
    /* Z, NZ, S, NS, P and NP read the result alone, the same as SUB. */
    return emit_sub(l, cc);
  }
}

/* INC and DEC leave CF as it was; the recording kept it in `carry_in`. */
static void push_carry_in(X86pWasmLower *l) {
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_load8_u(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, carry_in)));
}

/* `a` (normalised) against a constant normalised the same way. */
static void a_against(X86pWasmLower *l, int32_t normalised, X86pWasmI32Op op) {
  push_a(l);
  x86p_wasm_i32_const(l->e, normalised);
  x86p_wasm_i32_op(l->e, op);
}

/*
 * INC and DEC add or subtract one, so the signed conditions are a comparison
 * of the OPERAND with a constant: after INC, "SF != OF" -- the untruncated
 * a + 1 is negative -- is a <= -2, i.e. a < -1; after DEC it is a <= 0. OF is
 * the one wrap each can make: INC to the most negative value, DEC from it.
 * The unsigned conditions read the preserved carry.
 */
static int emit_incdec(X86pWasmLower *l, X86pCond cc, int dec) {
  const int shift = norm_shift(l);
  const int32_t one = (int32_t)((uint32_t)1u << shift);
  switch (cc) {
  case kX86pCondO:
  case kX86pCondNO:
    if (dec) {
      push_a(l);
    } else {
      push_r(l);
    }
    x86p_wasm_i32_const(l->e, (int32_t)0x80000000u);
    x86p_wasm_i32_op(l->e, cc == kX86pCondO ? kWasmI32Eq : kWasmI32Ne);
    return 1;
  case kX86pCondB:
    push_carry_in(l);
    return 1;
  case kX86pCondNB:
    push_carry_in(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    return 1;
  case kX86pCondBE:
    push_carry_in(l);
    push_r(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    x86p_wasm_i32_op(l->e, kWasmI32Or);
    return 1;
  case kX86pCondA:
    push_carry_in(l);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    r_against_zero(l, kWasmI32Ne);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    return 1;
  case kX86pCondL:
    a_against(l, dec ? 0 : -one, dec ? kWasmI32LeS : kWasmI32LtS);
    return 1;
  case kX86pCondGE:
    a_against(l, dec ? 0 : -one, dec ? kWasmI32GtS : kWasmI32GeS);
    return 1;
  case kX86pCondLE:
    a_against(l, dec ? one : -one, kWasmI32LeS);
    return 1;
  case kX86pCondG:
    a_against(l, dec ? one : -one, kWasmI32GtS);
    return 1;
  default:
    /* Z, NZ, S, NS, P and NP read the result alone. */
    return emit_sub(l, cc);
  }
}

int x86p_wasm_cond_is_inline(int last_kind, X86pCond cc) {
  if ((unsigned)cc >= (unsigned)kX86pCondCount) {
    return 0;
  }
  switch (last_kind) {
  case kX86pFlagsSub:
  case kX86pFlagsLogic:
  case kX86pFlagsAdd:
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    return 1;
  default:
    return 0;
  }
}

int x86p_wasm_cond_value(X86pWasmLower *l, X86pCond cc) {
  int inlined = 0;
  l->conds++;
  if (l->last_kind < 0) {
    l->cond_unknown_kind++;
  }
  if (x86p_wasm_cond_is_inline(l->last_kind, cc)) {
    switch (l->last_kind) {
    case kX86pFlagsSub:
      inlined = emit_sub(l, cc);
      break;
    case kX86pFlagsLogic:
      inlined = emit_logic(l, cc);
      break;
    case kX86pFlagsAdd:
      inlined = emit_add(l, cc);
      break;
    default:
      inlined = emit_incdec(l, cc, l->last_kind == (int)kX86pFlagsDec);
      break;
    }
  }
  if (inlined) {
    l->cond_inline++;
    return 1;
  }
  /* The authority, reached the way every site used to reach it. */
  x86p_wasm_i32_const(l->e, (int32_t)cc);
  x86p_wasm_state_flags_addr(&l->state);
  x86p_wasm_call(l->e, (uint32_t)kX86pWasmImportCond);
  return 0;
}
