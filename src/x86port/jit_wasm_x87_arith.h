/*
 * jit_wasm_x87_arith.h -- FADD, FSUB, FMUL and FDIV in binary64, without
 * leaving the module.
 *
 * WHY. A consumer that selected binary64 arithmetic (x87_double_arith.h) still
 * paid a cross-module call per operation, and the helper behind it: in the
 * browser the x87 arithmetic helpers were about 7% of the guest worker after
 * the blocks chained, against one f64 instruction for the answer.
 *
 * WHAT IS EMITTED. The same answer x86p_ext80_double_arith gives, for the set
 * it answers, computed in the block; everything else reaches the helper the
 * instruction called before, which is still the authority:
 *   - the unit's `double_arith` is set and no census is armed (a census counts
 *     every operation, and only the helper counts);
 *   - the control word is one x86p_x87_double_control_applies accepts;
 *   - the registers read are not empty;
 *   - each ext80 operand is a zero, or a normal whose unbiased exponent is in
 *     [-959, 1022]. Rounding one to binary64 is then f64.convert_i64_u of its
 *     significand -- round to nearest even, as the narrowing does -- times a
 *     power of two that is itself a normal binary64, which is exact because the
 *     product stays normal. The C form accepts a few more exponents; those go
 *     to it;
 *   - a memory operand is a binary32 or binary64 zero or normal;
 *   - the result is a zero or a binary64 normal.
 * Nothing is written until all of that has been decided, so a refusal reaches
 * the helper with the machine as the instruction found it.
 *
 * tests/test_wasm_x87.c runs both arms against the interpreter, which answers
 * through the C form.
 */
#ifndef X86PORT_JIT_WASM_X87_ARITH_H
#define X86PORT_JIT_WASM_X87_ARITH_H

#include "decode.h"

#include <stdint.h>

struct X86pWasmLower;

/*
 * For an x87 arithmetic `insn` at `pc`: emit the operand read and the decision,
 * and open the arm the helper runs in. Returns 1 having done so, and 0 having
 * emitted nothing for a form this does not compute (an integer or 80-bit
 * operand, or a host whose X86pX87Reg is not the architectural pair).
 *
 * After a 1 the caller emits the helper call, reading a memory operand's bits
 * from kX86pWasmLocal64Bits rather than loading them again, then calls
 * x86p_wasm_x87_arith_end.
 */
int x86p_wasm_x87_arith_begin(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

/* Close the helper's arm with the binary64 one: the result, its tag and the
   instruction's pops. */
void x86p_wasm_x87_arith_end(struct X86pWasmLower *l, const X86pInsn *insn);

#endif /* X86PORT_JIT_WASM_X87_ARITH_H */
