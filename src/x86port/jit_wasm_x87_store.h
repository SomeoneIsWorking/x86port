/*
 * jit_wasm_x87_store.h -- FST/FSTP m32 and m64, without leaving the module.
 *
 * The store-side twin of jit_wasm_x87_load.h, and it exists for the same
 * measured reason: after the load side was emitted inline the STORE side was
 * the larger half of the browser's x87 operand plumbing --
 * x86p_x87_operand_bytes_from_reg at 4.28% of the guest worker and
 * x86p_jit_x87_store_at at 3.60%, against the load path's 2.52%. Every FST
 * crossed out of its translated module to reach a softfloat conversion.
 *
 * IT IS NOT THE MIRROR IMAGE OF THE LOAD, because this direction ROUNDS. What
 * "the ordinary case" is, and why the other cases are somebody else's, lives
 * in x87_ext80_narrow.h -- one authority, which the C fast path in x87.c and
 * the WebAssembly emitted here both implement. The acceptance rule is shared
 * exactly so that neither arm can accept a value the other would refuse.
 *
 * WHAT THIS ONE ALSO DECLINES, beyond the value classes that owner names:
 *   - the sparse mapping, where the address is not a linear-memory offset and
 *     the store is an import call in its own right;
 *   - an integer destination (FIST), a different conversion which reports its
 *     own invalid-operation results;
 *   - an 80-bit destination, which is a copy with nothing to round and is
 *     already cheap;
 *   - a destination the guard says is not writable, so the fault is reported
 *     by exactly the code that reported it before;
 *   - more than one pop, which no store form produces.
 *
 * Everything declined reaches the helper unchanged, which is why this can only
 * ever cost a block a handful of comparisons and never an answer.
 */
#ifndef X86PORT_JIT_WASM_X87_STORE_H
#define X86PORT_JIT_WASM_X87_STORE_H

#include "decode.h"

#include <stdint.h>

struct X86pWasmLower;

/*
 * Emit `insn`, a memory-operand x87 store at `pc`, as the inline narrowing
 * plus a cold call. Returns 1 when it emitted the whole instruction and 0 when
 * it emitted NOTHING and the caller must lower it the ordinary way.
 *
 * As with the load, the refusal is decided from the instruction and the host
 * before a byte is written, so a caller that ignores the result produces a
 * block with the store in it twice rather than one that silently drops it.
 */
int x86p_wasm_x87_store_inline(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

#endif /* X86PORT_JIT_WASM_X87_STORE_H */
