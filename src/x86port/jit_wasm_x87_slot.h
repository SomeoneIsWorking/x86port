/*
 * jit_wasm_x87_slot.h -- the x87 register file, as the emitted code reaches it.
 *
 * Every inline x87 form the WebAssembly backend emits (load, store, compare,
 * binary64 arithmetic) finds a physical register from TOP, tests its tag,
 * reads or writes its two fields and may retire it. Each used to spell that
 * out for itself, three times over with two different base addresses. This is
 * the one spelling; the forms own only what they compute.
 *
 * Every access is addressed from the cpu local with the unit's own offset
 * folded into the instruction's immediate: forming the unit's address once per
 * access was a sixth of an arithmetic instruction's bytes.
 *
 * Only a host whose X86pX87Reg is the architectural pair has fields to reach,
 * so this whole interface exists under X86P_X87_BINARY128.
 */
#ifndef X86PORT_JIT_WASM_X87_SLOT_H
#define X86PORT_JIT_WASM_X87_SLOT_H

#include "cpu.h"
#include "x87.h"

#include <stddef.h>

#if X86P_X87_BINARY128

_Static_assert(sizeof(X86pX87Reg) == 16u, "the emitted x87 forms address a 16-byte register");
_Static_assert(offsetof(X86pX87Reg, signif) == 0u, "the emitted x87 forms find the significand first");
_Static_assert(offsetof(X86pX87Reg, sign_exp) == 8u, "the emitted x87 forms write sign_exp and its padding as one i64");

struct X86pWasmLower;

/* Immediate offsets from the address x86p_wasm_x87_slot_register() leaves,
   and from the cpu local for the two unit-wide fields. */
enum {
  kX86pWasmX87Signif = (int)(offsetof(X86pCpu, x87) + offsetof(X86pX87, reg) + offsetof(X86pX87Reg, signif)),
  kX86pWasmX87SignExp = (int)(offsetof(X86pCpu, x87) + offsetof(X86pX87, reg) + offsetof(X86pX87Reg, sign_exp)),
  kX86pWasmX87Control = (int)(offsetof(X86pCpu, x87) + offsetof(X86pX87, control)),
  kX86pWasmX87Status = (int)(offsetof(X86pCpu, x87) + offsetof(X86pX87, status))
};

/* The physical index of ST(delta), (top + delta) & 7, into i32 `local`, and on
   the stack whether that register is EMPTY. A push passes delta -1: TOP is a
   uint8_t, so a TOP of zero wraps to 7, as x86p_x87_push_raw computes it. */
void x86p_wasm_x87_slot_index(struct X86pWasmLower *l, int delta, int local);

/* The address of reg[`local`] minus the register file's own offset, on the
   stack, for an access at kX86pWasmX87Signif or kX86pWasmX87SignExp. */
void x86p_wasm_x87_slot_register(struct X86pWasmLower *l, int local);

/* tag[`local`] = `tag`. */
void x86p_wasm_x87_slot_set_tag(struct X86pWasmLower *l, int local, X86pX87Tag tag);

/* TOP = `local`, the index a push has just filled. */
void x86p_wasm_x87_slot_set_top(struct X86pWasmLower *l, int local);

/* The pop x86p_x87_pop_raw performs on ST(0), whose physical index is in
   `local`, in the order it performs it: the slot becomes empty and TOP moves
   up one. */
void x86p_wasm_x87_slot_retire(struct X86pWasmLower *l, int local);

/* The same pop when ST(0)'s index is not already known: it is read from TOP
   into i32 `local` first. */
void x86p_wasm_x87_slot_pop(struct X86pWasmLower *l, int local);

#endif /* X86P_X87_BINARY128 */

#endif /* X86PORT_JIT_WASM_X87_SLOT_H */
