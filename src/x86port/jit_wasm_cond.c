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
static void push_norm(X86pWasmLower *l, size_t field) {
  const int shift = (l->last_w >= 1 && l->last_w <= 4) ? 32 - 8 * l->last_w : 0;
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

int x86p_wasm_cond_is_inline(int last_kind, X86pCond cc) {
  if ((unsigned)cc >= (unsigned)kX86pCondCount) {
    return 0;
  }
  return last_kind == (int)kX86pFlagsSub || last_kind == (int)kX86pFlagsLogic;
}

int x86p_wasm_cond_value(X86pWasmLower *l, X86pCond cc) {
  int inlined = 0;
  l->conds++;
  if (x86p_wasm_cond_is_inline(l->last_kind, cc)) {
    inlined = (l->last_kind == (int)kX86pFlagsSub) ? emit_sub(l, cc) : emit_logic(l, cc);
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
