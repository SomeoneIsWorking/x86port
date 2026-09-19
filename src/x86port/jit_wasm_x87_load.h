/*
 * jit_wasm_x87_load.h -- FLD m32 and FLD m64, without leaving the module.
 *
 * WHY THIS IS ITS OWN UNIT. Every other x87 form the WebAssembly backend
 * emits is argument setup and a call: the semantics live in the shared x87
 * owners and the emitted code says which one to run. This one is the
 * exception, and it is an exception on measured grounds rather than on taste.
 *
 * The widening a float load performs is pure integer bit manipulation --
 * x87_ext80_widen.h explains why it cannot round -- and after that module
 * replaced the softfloat conversion it was still 8.4% of the browser's guest
 * worker, the largest single function in it. What remained was not the
 * arithmetic. It was the crossing: a translated block lives in its own
 * WebAssembly module, so reaching x86p_wasm_x87_load_bits is a cross-module
 * call, and the guest performs one per FLD.
 *
 * ONLY THE ORDINARY CASE IS EMITTED. A subnormal, a zero, an infinity, a NaN
 * and a stack overflow all still go to the helper, because each of them needs
 * a different answer and putting five answers inline would trade the crossing
 * for a block twice the size. The emitted test for "ordinary" is two integer
 * comparisons on the exponent and one on the destination's tag; everything it
 * rejects reaches exactly the code that ran before.
 *
 * So there are still two implementations of the normal case and that is the
 * cost of this file. They are held together by two things rather than by
 * review: x86p_ext80_source publishes the six numbers both of them use, and
 * tests/test_wasm_x87.c runs the emitted form against the interpreter over the
 * operand values that separate the arms -- including the ones that must NOT
 * take the inline path.
 */
#ifndef X86PORT_JIT_WASM_X87_LOAD_H
#define X86PORT_JIT_WASM_X87_LOAD_H

#include "decode.h"

#include <stdint.h>

struct X86pWasmLower;

/*
 * Emit `insn`, a memory-operand x87 load at `pc`, as the inline widening plus
 * a cold call. Returns 1 when it emitted the whole instruction and 0 when it
 * emitted NOTHING and the caller must lower it the ordinary way.
 *
 * The refusal is decided entirely from the instruction and the host, before a
 * byte is written, so a caller that ignores the result produces a block with
 * the load in it twice rather than one that silently drops it. It declines an
 * integer operand (a different conversion), a width other than 4 or 8, a form
 * that pops (a load that retires a slot is not the shape this fast path
 * clears), and every host whose X86pX87Reg is not the architectural pair --
 * there being no significand field to store on one of those.
 */
int x86p_wasm_x87_load_inline(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

#endif /* X86PORT_JIT_WASM_X87_LOAD_H */
