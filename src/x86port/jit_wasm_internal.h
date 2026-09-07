/*
 * jit_wasm_internal.h -- the contract between jit_wasm_lower.c and its
 * per-family lowering units.
 *
 * NOT a public header. It exposes the per-block instance, the shared operand
 * helpers every family needs, and the dispatch table entry that binds an
 * instruction family to the unit that lowers it.
 *
 * WHY A TABLE AND NOT A SWITCH. The two machine-code backends answer "can this
 * be emitted?" in one function and "how" in another, and the two have to be
 * kept in step by reading them side by side -- a `can_emit` that says yes for
 * a family the switch has no arm for is a defect those backends have to check
 * for at run time and report. Here one table entry holds both answers for a
 * family, so the pair cannot drift: an entry with no emitter is not in the
 * table, and an instruction is lowerable exactly when its entry accepts it.
 */
#ifndef X86PORT_JIT_WASM_INTERNAL_H
#define X86PORT_JIT_WASM_INTERNAL_H

#include "cpu.h"
#include "decode.h"
#include "jit_wasm_lower.h"
#include "jit_wasm_module.h"
#include "jit_wasm_state.h"

#include <stdint.h>

/*
 * Per-block lowering state.
 *
 * `last_kind` is the flag kind the previously lowered instruction recorded, or
 * -1 when the predecessor is whatever ran before this block. It is what lets
 * the carry-in be derived inline instead of asked for: the derivation depends
 * only on the KIND, which is known at lowering time everywhere except at the
 * block's first flag write.
 */
typedef struct X86pWasmLower {
  X86pWasmModule *module;
  X86pWasmEmit *e;
  X86pWasmState state;
  const X86pMem *fetch;
  unsigned flag_helper_calls;
  int last_kind;
} X86pWasmLower;

/* One instruction family's entry: whether it accepts a particular instruction,
   how to lower it, and whether lowering it ENDS the block. */
typedef struct X86pWasmOpEntry {
  /* NULL means "every instruction of this family", which is only ever true for
     families with no operands at all. */
  int (*accepts)(const X86pInsn *insn);
  void (*lower)(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
  /* Non-zero when the lowered form writes the block's exit itself, so the
     block loop must stop rather than emit a fall-through epilogue after it. */
  int terminates;
} X86pWasmOpEntry;

/* The table, indexed by X86pInsnOp. Owned by jit_wasm_lower.c; the per-family
   units contribute their entries through the declarations below. */
const X86pWasmOpEntry *x86p_wasm_op_entry(uint8_t op);

/* ---- shared operand helpers (jit_wasm_lower.c) --------------------------- */

/*
 * Is this operand one the lowering can handle at width `w`?
 *
 * A register or an immediate always; a memory operand unless it was formed
 * with 16-BIT address registers, whose sum truncates to sixteen bits before
 * the segment base is added -- a rule this lowering does not implement, so it
 * is refused by name rather than translated as if the prefix were absent.
 */
int x86p_wasm_operand_ok(const X86pOperand *o, int w, int for_write);

/* Width in {1, 2, 4}: the integer widths this lowering covers. */
int x86p_wasm_width_ok(int w);

/* The value mask for a width. */
uint32_t x86p_wasm_width_mask(int w);

/*
 * Push the value of a non-memory operand at width `w`. An immediate is masked
 * HERE, at lowering time, because x86p_alu masks its `b` and the flag tuple
 * has to match: `83 /r` sign-extends an imm8 to a dword the operation then
 * narrows again.
 */
void x86p_wasm_push_operand(X86pWasmLower *l, const X86pOperand *o, int w);

/* Call an imported helper. Arguments must already be on the stack, in order. */
void x86p_wasm_call_import(X86pWasmLower *l, X86pWasmImport which);

/*
 * Leave the incoming CF in kX86pWasmLocalCarry, derived inline from
 * `l->last_kind` when that is known and asked of x86p_flag_cf when it is not.
 * Counts the helper call in the block's tally.
 *
 * Computed BEFORE the operation, while the old flag state is still intact, and
 * stored only after any bounds check has passed -- a refused access must leave
 * every flag field exactly as it was.
 */
void x86p_wasm_carry_in(X86pWasmLower *l);

/* ---- per-family lowering units ------------------------------------------- */

/* jit_wasm_alu.c */
int x86p_wasm_alu_accepts(const X86pInsn *insn);
void x86p_wasm_alu_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_alu_unary_accepts(const X86pInsn *insn);
void x86p_wasm_alu_unary_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

/* jit_wasm_move.c */
int x86p_wasm_mov_accepts(const X86pInsn *insn);
void x86p_wasm_mov_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_movx_accepts(const X86pInsn *insn);
void x86p_wasm_movzx_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_movsx_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_lea_accepts(const X86pInsn *insn);
void x86p_wasm_lea_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_xchg_accepts(const X86pInsn *insn);
void x86p_wasm_xchg_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_setcc_accepts(const X86pInsn *insn);
void x86p_wasm_setcc_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_leave_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_cdq_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_cwde_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_cld_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_std_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_nop_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

/* jit_wasm_stack.c */
int x86p_wasm_push_accepts(const X86pInsn *insn);
void x86p_wasm_push_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_pop_accepts(const X86pInsn *insn);
void x86p_wasm_pop_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
/* Leave ESP decremented by four with the pushed value stored, or exit with a
   memory fault. `value` is the local holding what to push. Shared with the
   CALL forms, which push a return address. */
void x86p_wasm_push_local(X86pWasmLower *l, X86pWasmLocal value, uint32_t pc);

/* jit_wasm_branch.c */
int x86p_wasm_jmp_accepts(const X86pInsn *insn);
void x86p_wasm_jmp_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_jcc_accepts(const X86pInsn *insn);
void x86p_wasm_jcc_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_jecxz_accepts(const X86pInsn *insn);
void x86p_wasm_jecxz_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_call_accepts(const X86pInsn *insn);
void x86p_wasm_call_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_ret_accepts(const X86pInsn *insn);
void x86p_wasm_ret_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

#endif /* X86PORT_JIT_WASM_INTERNAL_H */
