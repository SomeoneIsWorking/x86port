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
#include "jit_chain_census.h"
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

/*
 * Where a block's exits go, counted as they are emitted.
 *
 * COUNTED HERE BECAUSE HERE IS WHERE IT IS KNOWN. A conditional branch does not
 * end a block in this backend: x86p_wasm_jcc_continue emits the taken exit
 * inline and the run carries on past it. So a block has as many exits as it has
 * branches, and the guest loop backedge that block chaining would collapse is
 * almost never the last of them.
 *
 * A census that walks a block's bytes and classifies its TERMINATOR therefore
 * sees none of them. Measured, that walk found ONE branch back to a block's own
 * entry in 113,272 blocks of a real title, over code that is known to contain a
 * skinning loop paying a dispatch per bone. A near-zero over a population that
 * cannot be near-zero is what says a classifier is looking at the wrong thing.
 *
 * ONLY kX86pJitExitBlockEnd IS A SUCCESSOR. An unsupported instruction and a
 * memory fault also write an EIP and return, and neither is an address the run
 * continues from; counting them would inflate exactly the number that decides
 * whether chaining is worth building.
 */
typedef struct X86pWasmExitCensus {
  unsigned total;
  unsigned to_immediate; /* the successor is a constant in the emitted code */

  /*
   * The constant successor ADDRESSES, for the runtime chain census: counting
   * how many exits name a constant cannot say whether the dispatches actually
   * paid went to one of them. Distinct addresses only, capped, with the ones
   * past the cap counted so a block with many exits is visible as such rather
   * than quietly truncated.
   */
  uint32_t targets[X86P_JIT_CHAIN_TARGETS];
  unsigned target_count;
  unsigned targets_overflowed;

  /*
   * THREE NESTED SUBSETS OF to_immediate, RANKING THREE DIFFERENT FIXES.
   *
   * `backward` is an exit to an address at or before the branch emitting it: a
   * guest loop backedge, wherever its head ended up. It is the size of the
   * prize -- every one of these pays a dispatch per iteration.
   *
   * `within_block` is the subset whose head is inside this block's own lowered
   * code. A backend can reach those without any other block existing, but only
   * by knowing the backedge before it emits the body, because WebAssembly has
   * no jump and a `loop` has to be opened ahead of its target.
   *
   * `to_entry` is the strict case, the block's own first address, and the only
   * one a single-pass lowering can take: open a `loop` around the whole body
   * and branch to it.
   *
   * WHICH ONE A GUEST LOOP FALLS INTO IS DECIDED BY WHERE THE BLOCK STARTED,
   * NOT BY THE GUEST. The running engine translates from the address it was
   * asked to dispatch to, so a loop head that is branched to becomes a block
   * entry and its backedge is a to_entry. An offline walk that splits a
   * function linearly from its first byte puts that same head mid-block, and a
   * head above a CALL puts it in an earlier block still. Measured over one
   * title's 16,451 functions walked that way: 36,113 backward of 167,287 exits,
   * of which 2,694 within_block and 69 to_entry. Only the first of those three
   * is a property of the guest; reporting either of the others alone makes a
   * binary full of loops look as though it had none.
   */
  unsigned backward;
  unsigned within_block;
  unsigned to_entry;

  unsigned computed; /* the successor is a register or a memory word */
} X86pWasmExitCensus;

typedef struct X86pWasmState {
  X86pWasmEmit *e;
  X86pWasmPlan plan;
  uint32_t entry; /* the block's own guest address, to recognise a loop */
  uint32_t pc;    /* the instruction being lowered: everything up to here exists */
  X86pWasmExitCensus exits;
} X86pWasmState;

/* `entry` is the guest address the block being lowered starts at. It is a
   parameter rather than a later setter because a state that never received it
   reports every self-loop as an exit to somewhere else, and reports it as a
   zero that reads like an answer. */
void x86p_wasm_state_init(X86pWasmState *s, X86pWasmEmit *e, const X86pWasmPlan *plan, uint32_t entry);

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

/*
 * The whole register at once, for the packed lowering. Pushes one v128.
 *
 * `xmm` is a contiguous 16-byte field per register, which is what makes this a
 * single load rather than the four the lane calls above emit; the offset stays
 * here so that remains the state owner's fact and not the lowering's.
 */
void x86p_wasm_state_load_xmm(X86pWasmState *s, unsigned index);

/*
 * THE ADDRESS OF A REGISTER, PUSHED AHEAD OF THE VALUE. WebAssembly's store
 * takes its address BENEATH its value, so a caller that computes a v128 and
 * then wants to store it must have pushed this first. Split from the store
 * below for exactly that reason, and the register index appears only here so
 * the pair cannot name two different registers.
 */
void x86p_wasm_state_xmm_addr(X86pWasmState *s, unsigned index);

/* Store the v128 on top of the stack at the address beneath it, consuming
   both. The address is whatever x86p_wasm_state_xmm_addr pushed. */
void x86p_wasm_state_store_v128(X86pWasmState *s);

/* Push the 16 guest bytes at kX86pWasmLocalAddr as one v128. Only valid on the
   contiguous mapping -- x86p_wasm_state_memory_is_direct is the question --
   because the sparse mapping is reachable only through the checked imports,
   which take and return i32. */
void x86p_wasm_state_load_mem_v128(X86pWasmState *s);
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

/*
 * The same proof, PUSHED AS AN i32 instead of taken: 1 when every byte of the
 * access is inside the mapping and carries `access`, 0 otherwise.
 *
 * For the instruction that must do something before its fault is reported.
 * FIST of a value it cannot represent records an invalid operation in the x87
 * status word, and the interpreter it is checked against records it even when
 * the address turns out not to be writable; an early return would skip the
 * conversion and lose the flag. So the verdict travels as a value to the
 * helper that does the conversion, and the helper decides the order.
 *
 * On the contiguous mapping kX86pWasmLocalAddr holds a linear-memory offset on
 * success and is meaningless on failure. On the sparse mapping it holds the
 * guest address either way, because the checked imports take guest addresses.
 */
void x86p_wasm_state_check(X86pWasmState *s, const X86pOperand *o, int w, unsigned access);

/*
 * Whether guest memory is the contiguous mapping, whose addresses the emitted
 * code reaches directly, rather than the sparse one, which is reached only
 * through the checked imports. A caller with a faster route on the first asks
 * here instead of reading the plan for itself.
 */
int x86p_wasm_state_memory_is_direct(const X86pWasmState *s);

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
