/*
 * jit_wasm_cond.h -- one owner for "put this condition on the stack as 0 or 1".
 *
 * WHY THIS IS ITS OWN MODULE. Four families ask for a condition -- Jcc, SETcc,
 * CMOVcc and FCMOVcc -- and all four used to spell the question out as the
 * same three instructions ending in a call to the shared authority. A call is
 * correct and is the expensive part: the translated block lives in its own
 * WebAssembly module, so reaching x86p_cond leaves that module, and the game's
 * commonest shape by a distance is a compare followed immediately by a branch
 * on its result.
 *
 * WHAT MAKES AN INLINE FORM POSSIBLE AT ALL. The other backends read the
 * host's own flag register, which WebAssembly does not have. What it has
 * instead is the lazy flag state the guest already keeps in memory -- the two
 * operands, the result and the width -- plus the KIND of the operation that
 * wrote them, which the lowering knows at translation time in every case but
 * the block's first flag write. Given the kind, each condition is an integer
 * expression over those fields. x86p_wasm_carry_in already does exactly this
 * for one flag; this does it for all sixteen conditions.
 *
 * WHAT IT DOES NOT DO. Only the kinds it can derive without guessing: Sub
 * (every CMP and SUB) and Logic (every TEST, AND, OR, XOR). Add, the shifts,
 * INC/DEC, an explicit EFLAGS word and an unknown predecessor all fall through
 * to the authority, which is not a stopgap but the same division x86p_flag_cf
 * makes -- and the fall-through is what keeps the set extensible one kind at a
 * time without a correctness cliff.
 *
 * The counters are the proof it fired. A lowering that quietly stopped inlining
 * would be invisible in guest state and in every differential comparison,
 * because calling the authority is entirely correct and merely slow.
 */
#ifndef X86PORT_JIT_WASM_COND_H
#define X86PORT_JIT_WASM_COND_H

#include "cond.h"

struct X86pWasmLower;

/*
 * Emit code leaving the condition's value, 0 or 1, on the WebAssembly stack.
 *
 * Always emits something: the caller does not choose between forms and cannot
 * get the choice wrong. Returns 1 when it was derived inline and 0 when the
 * authority was called, which is what the block's counters record.
 */
int x86p_wasm_cond_value(struct X86pWasmLower *l, X86pCond cc);

/*
 * Whether this (kind, condition) pair has an inline form, without emitting.
 * Exists for the tests, which need to assert that a case they believe is
 * inline really is -- a differential that passes because everything fell back
 * to the authority proves the authority works and nothing else.
 */
int x86p_wasm_cond_is_inline(int last_kind, X86pCond cc);

#endif
