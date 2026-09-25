/*
 * multiply.h -- MUL and IMUL applied to the register file, once for every
 * backend.
 *
 * alu.h owns the arithmetic and the flags; this owns where the product goes.
 * The x86-64, ARM64 and WebAssembly backends all call out for multiplies, and
 * each used to carry its own copy of "which halves land in which registers" --
 * five of them, with only the 32-bit form of IMUL r, r/m[, imm] accepted
 * natively because the native copies wrote `cpu->reg[destination]` whole.
 * A 16-bit `imul cx, cx, 5` must leave the top of ECX alone, and writing
 * through x86p_reg_write at the operand's width is what does that.
 *
 * Every entry point takes uint32_t arguments so the same functions serve as a
 * native call target and a WebAssembly import.
 */
#ifndef X86PORT_MULTIPLY_H
#define X86PORT_MULTIPLY_H

#include "cpu.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The widening multiply of `left` by `right` at `width` bytes, flags included.
   With `implicit` set this is the one-operand form: the product lands in
   AX (width 1) or DX:AX / EDX:EAX. Returns the low half either way. */
uint32_t
x86p_multiply(X86pCpu *cpu, uint32_t left, uint32_t right, uint32_t width, uint32_t signed_multiply, uint32_t implicit);

/* One-operand MUL/IMUL: the accumulator times `operand`. */
void x86p_multiply_accumulator(X86pCpu *cpu, uint32_t operand, uint32_t signed_multiply, uint32_t width);

/* IMUL r, r/m and IMUL r, r/m, imm: the low half of `left * right` into
   register `destination` at `width` bytes, every other bit preserved. */
void x86p_imul_to_register(X86pCpu *cpu, uint32_t left, uint32_t right, uint32_t destination, uint32_t width);

#ifdef __cplusplus
}
#endif

#endif
