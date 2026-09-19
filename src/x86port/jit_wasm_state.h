/*
 * jit_wasm_state.h -- the guest machine, as emitted WebAssembly sees it.
 *
 * WHAT THIS OBJECT IS FOR. Every lowering unit needs the same four things: a
 * guest register at a width, a field of the lazy flag state, the linear
 * address a guest memory operand names, and the way out of a block. Those are
 * the only places that know X86pCpu's layout and the only places that know the
 * mapping a block was translated against, so they live behind one instance
 * rather than being spelled at each call site. A second site that computed a
 * field offset itself would be a second opinion about the guest's state
 * layout, and the disagreement reads or writes the neighbouring field.
 *
 * THE MAPPING IS 32-BIT HERE, AND THAT IS NOT A NARROWING. On a wasm host the
 * whole address space IS the imported memory, so a host pointer is a 32-bit
 * offset into it. The plan is therefore plain integers rather than a
 * host pointer -- which has the further effect that the lowering can be driven
 * on a machine that is not the wasm host at all, with a plan describing a
 * memory image that machine merely holds the bytes of. That is what makes the
 * lowering differentially testable against the interpreter without an
 * Emscripten toolchain: same emitted module, a memory image laid out by the
 * test, and a real engine to run it in.
 *
 * STACK EFFECTS ARE PART OF EVERY SIGNATURE. WebAssembly is a stack machine
 * with no register allocator, so "leaves one i32 on the stack" is as much a
 * part of an operation's contract as its arguments are, and each is stated.
 * Values that must survive an operation travel through the named locals in
 * X86pWasmLocal rather than being kept on the stack across one.
 */
#ifndef X86PORT_JIT_WASM_STATE_H
#define X86PORT_JIT_WASM_STATE_H

#include "decode.h"
#include "emit_wasm.h"
#include "flags.h"
#include "jit_wasm_module.h"
#include "jit_x64.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The guest mapping a block is translated against, baked into the block as
 * constants exactly as the other backends bake theirs in: an access becomes a
 * compare and an add rather than a call. A block is only valid for the mapping
 * it was translated for, and the block cache's flush is what discards blocks
 * when the guest memory moves or is resized. For sparse memory, memory_context
 * instead names the X86pMem passed to checked imports; base/lo/size are ignored.
 * The context and mappings stay alive and immutable during block entry.
 */
typedef struct X86pWasmPlan {
  uint32_t base;           /* linear-memory offset that guest address `lo` lives at */
  uint32_t lo;             /* first guest address covered */
  uint32_t size;           /* bytes covered from `lo` */
  uint32_t memory_context; /* nonzero: X86pMem offset for checked sparse imports */
  /* nonzero: byte-per-page permission table at this linear-memory offset,
     indexed from `lo` by `page_shift`. See X86pMem::perms -- the guard loads
     from it on every access, so a permission change needs no flush; only
     MOVING the table does. */
  uint32_t perms;
  uint32_t page_shift;
} X86pWasmPlan;

typedef struct X86pWasmState {
  X86pWasmEmit *e;
  X86pWasmPlan plan;
} X86pWasmState;

void x86p_wasm_state_init(X86pWasmState *s, X86pWasmEmit *e, const X86pWasmPlan *plan);

/* Push the X86pCpu address. */
void x86p_wasm_state_cpu(X86pWasmState *s);

/* Push the address of cpu->flags, which is what every imported helper that
   reads or writes flag state takes. */
void x86p_wasm_state_flags_addr(X86pWasmState *s);
void x86p_wasm_state_x87_addr(X86pWasmState *s);

/*
 * Push a guest register of width `w` (1, 2 or 4), zero-extended.
 *
 * At width 1 the index is an ENCODED byte-register number, so 4..7 are AH..BH
 * -- x86p_byte_reg owns that, and this asks it rather than restating it.
 */
void x86p_wasm_state_load_reg(X86pWasmState *s, int index, int w);

/* Store local `value` into a guest register of width `w`. A narrow store
   preserves the rest of the register by construction, because the host is
   little-endian and the slot is a dword -- the rule cpu.h states. */
void x86p_wasm_state_store_reg(X86pWasmState *s, int index, int w, X86pWasmLocal value);

/* SIMD lanes are 32-bit bit patterns; the state owner keeps CPU layout private. */
void x86p_wasm_state_load_xmm_lane(X86pWasmState *s, unsigned index, unsigned lane);
void x86p_wasm_state_store_xmm_lane(X86pWasmState *s, unsigned index, unsigned lane, X86pWasmLocal value);
void x86p_wasm_state_load_mxcsr(X86pWasmState *s);
void x86p_wasm_state_store_mxcsr(X86pWasmState *s, X86pWasmLocal value);

/*
 * Push base + index*scale + disp, with NO segment base added.
 *
 * This is what LEA computes: the instruction is the one memory-operand form
 * that does not access memory, so it also gets no bounds check -- guest code
 * really does use it as a three-input adder on values that are not addresses
 * at all, and checking it would fault on an address the guest never touched.
 */
void x86p_wasm_state_address_parts(X86pWasmState *s, const X86pOperand *o);

/*
 * Push the LINEAR address a memory ACCESS uses: the parts above plus the FS or
 * GS base when the operand uses one. No bounds check, and no memory touched --
 * this is what the guard below starts from.
 */
void x86p_wasm_state_address(X86pWasmState *s, const X86pOperand *o);

/*
 * Bounds-check the operand for width `w` and explicit X86pMemAccess rights. Contiguous mode leaves
 * kX86pWasmLocalAddr holding its linear-memory address; sparse mode leaves the
 * guest address for checked memory imports that can cross backing spans.
 *
 * ONE unsigned compare covers both ends: the offset from `lo` is huge when the
 * guest address is below it, so underflow and overflow are caught together.
 * The comparison is against size - w, so an access that starts inside the
 * mapping and runs off the end is refused rather than truncated -- the rule
 * x86p_mem_read enforces.
 *
 * On failure the emitted code stores `insn_eip` and RETURNS
 * kX86pJitExitMemoryFault, so the guest EIP lands ON the faulting instruction.
 * There is no shared fault stub as the machine-code backends have: wasm's
 * control flow is structured, an early `return` costs the same as a branch to
 * one, and a stub would need a block wrapping the entire body to branch to.
 */
void x86p_wasm_state_guard(X86pWasmState *s, const X86pOperand *o, uint32_t insn_eip, int w, unsigned access);

/*
 * Bounds-check an address ALREADY in kX86pWasmLocalAddr, for the accesses
 * whose address is not an operand -- the stack, and a RET's return address.
 */
void x86p_wasm_state_guard_addr(X86pWasmState *s, uint32_t insn_eip, int w, unsigned access);

/* Push the guest value at kX86pWasmLocalAddr, zero-extended from width `w`. */
void x86p_wasm_state_load_mem(X86pWasmState *s, int w);

/*
 * Push the guest value at kX86pWasmLocalAddr as a low/high i32 pair, for the
 * widths a single i32 cannot hold.
 *
 * A pair rather than an i64 because every import takes and returns i32 -- see
 * x86p_wasm_import_type -- and at width 8 the two halves are the two loads an
 * engine issues for an unaligned i64 anyway. At widths 1 to 4 the high half is
 * a constant zero, so the caller does not branch on the width.
 */
void x86p_wasm_state_load_mem_pair(X86pWasmState *s, int w);

/* Store local `value` at kX86pWasmLocalAddr, at width `w`. */
void x86p_wasm_state_store_mem(X86pWasmState *s, int w, X86pWasmLocal value);

/*
 * Write the lazy flag tuple: `a`, `b` and `r` from their locals, plus the kind
 * and the width. Field for field what x86p_flags_set stores, which is the
 * whole reason emitted code may write it directly instead of calling.
 */
void x86p_wasm_state_store_flags(X86pWasmState *s, X86pFlagKind kind, int w);

/* Write kX86pWasmLocalCarry into the flag state's carry_in byte. Separate from
   store_flags because it is computed BEFORE a bounds check and stored only
   after one: a refused access must leave every flag field as it was. */
void x86p_wasm_state_store_carry(X86pWasmState *s);

/*
 * Write the direction flag, which CLD and STD set outright.
 *
 * A method of its own rather than a generic field store: DF is held apart from
 * the lazy flag model on purpose (cpu.h says why), and a caller reaching it
 * through a raw offset would be a second site that knows the state layout.
 */
void x86p_wasm_state_store_df(X86pWasmState *s, int value);

/* Store a guest EIP and leave the block with `exit`. */
void x86p_wasm_state_exit_imm(X86pWasmState *s, uint32_t eip, X86pJitExit exit);

/* The same, with the EIP already computed into a local. */
void x86p_wasm_state_exit_local(X86pWasmState *s, X86pWasmLocal eip, X86pJitExit exit);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_STATE_H */
