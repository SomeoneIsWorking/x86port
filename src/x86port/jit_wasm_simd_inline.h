/*
 * jit_wasm_simd_inline.h -- the packed SSE forms emitted as WebAssembly SIMD
 * instead of called out of the module for.
 *
 * WHY THIS EXISTS. Every SSE arithmetic instruction in this backend becomes a
 * call to the `wasm_simd_arithmetic` import: four i32 loads to take the source
 * apart, eight arguments across the module boundary, lane-at-a-time scalar C in
 * the helper, and four i32 stores to put the destination back. WebAssembly has
 * had a 128-bit SIMD instruction set since 2021 and the guest instructions this
 * route actually runs map onto it one to one, so the crossing, the marshalling
 * and the lane loop are all avoidable -- the whole instruction becomes two
 * loads, one operation and one store.
 *
 * WHAT MADE IT SAFE TO WRITE. Measured on a real route, 317,883,827 packed
 * operations, the guest's MXCSR holds round-to-nearest with flush-to-zero and
 * denormals-are-zero clear on 100.00% of them, and four opcodes account for
 * every one. That is the one rounding mode WebAssembly's f32x4 arithmetic
 * produces. The census is recorded in the consuming project's issue 0167; it
 * was removed once it had answered, because a counter in the path it measures
 * is cost.
 *
 * THIS IS BIT-IDENTICAL TO THE HELPER IT REPLACES, NOT MERELY CLOSE. The helper
 * computes in the host's default binary32 environment -- `a[i] + b[i]` on four
 * floats, see simd_packed.c -- and reads nothing from MXCSR at all. f32x4.add
 * is IEEE 754 binary32 addition in the same environment. So no runtime guard on
 * the control word is emitted: one would fall back to an implementation that
 * ignores the control word too, which is theatre rather than correctness. What
 * the census bought is the knowledge that BOTH are right for this guest, and if
 * a route ever does set rounding control then the helper is equally wrong and
 * the fix belongs in the helper.
 *
 * WHAT IT DECLINES, AND WHY THAT MATTERS. A form this unit will not emit must
 * leave NOTHING behind, because the caller then lowers it the ordinary way and
 * a half-emitted instruction would corrupt the block. The contract below is
 * therefore all-or-nothing, and the return value says which happened.
 */
#ifndef X86PORT_JIT_WASM_SIMD_INLINE_H
#define X86PORT_JIT_WASM_SIMD_INLINE_H

#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

struct X86pWasmLower;

/*
 * Emit `insn` as WebAssembly SIMD, or emit nothing.
 *
 * Returns 1 when the whole instruction was emitted and the caller must not
 * lower it again; 0 when nothing at all was emitted. A memory operand's bounds
 * check is part of the emitted form, so a 1 also means the guard was placed.
 */
int x86p_wasm_simd_inline(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_SIMD_INLINE_H */
