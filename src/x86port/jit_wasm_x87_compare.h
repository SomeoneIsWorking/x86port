/*
 * jit_wasm_x87_compare.h -- FCOM/FCOMP m32 and m64, without leaving the module.
 *
 * WHY. After the load and store forms were emitted inline, the comparison was
 * the largest x87 form still crossing out of its translated module every time:
 * 28.5 million FCOMs of a float operand in 90 seconds of the Dead Zone, each a
 * call into x86p_jit_x87_compare_mem_bits, which widened the operand to the
 * host's binary128 long double and ordered the two in softfloat.
 *
 * WHAT IS EMITTED. Two values that are each a normal or a zero, with the
 * canonical encoding ext80 gives them, are ordered by their magnitudes as the
 * pair (exponent, significand) compared lexicographically, then by their
 * signs -- with the two zeros equal whatever their signs. That is integer work
 * and nothing else, and it is exact: the order it computes is the order of the
 * values, not of a rounding of them.
 *
 * The status word is what x86p_x87_compare leaves for an ordered pair: C0, C1,
 * C2 and C3 cleared, then C0 for "less" or C3 for "equal". An FCOMP retires
 * ST(0) afterwards.
 *
 * WHAT IT DECLINES, to exactly the helper that answered before:
 *   - an integer operand (FICOM), which is a different conversion;
 *   - a width other than 4 or 8, and a form that pops more than once;
 *   - at run time: an empty ST(0), which is a stack fault; and on either side
 *     an infinity, a NaN (the UNORDERED result and its invalid-operation
 *     flag), a subnormal, and an ext80 unnormal.
 */
#ifndef X86PORT_JIT_WASM_X87_COMPARE_H
#define X86PORT_JIT_WASM_X87_COMPARE_H

#include "decode.h"

#include <stdint.h>

struct X86pWasmLower;

/*
 * Emit `insn`, a memory-operand x87 comparison at `pc`, as the inline ordering
 * plus a cold call. Returns 1 when it emitted the whole instruction and 0 when
 * it emitted NOTHING and the caller must lower it the ordinary way; the
 * refusal is decided from the instruction and the host before a byte is
 * written.
 */
int x86p_wasm_x87_compare_inline(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

#endif /* X86PORT_JIT_WASM_X87_COMPARE_H */
