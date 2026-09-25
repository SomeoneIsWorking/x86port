/*
 * jit_arm64_x87_inline.h -- the ordinary x87 cases, emitted as AArch64
 * instructions instead of helper calls.
 *
 * On an AArch64 phone about a third of the game thread was the x87 helpers
 * jit_arm64_x87.c calls: a call per instruction, and inside it an ext80 to
 * binary64 conversion, the operation, and the conversion back. This emits the
 * ordinary case of each common form in line:
 *
 *   - FADD/FSUB/FMUL/FDIV (+R, +P) on registers and on m32/m64, in the
 *     consumer's double-arithmetic mode (x87_double_arith.h) -- the mode the
 *     helper would answer on binary64 anyway, so the answer is the same;
 *   - FLD m32/m64 and FST/FSTP m32/m64 (exact widening, round-to-nearest
 *     narrowing, as x87_ext80_widen.h and x87_ext80_narrow.h define them);
 *   - FCOM/FCOMP m32/m64 and FCOM/FCOMP/FCOMPP/FUCOM* ST(i) when both values
 *     are exact binary64 numbers;
 *   - FLD ST(i), FST/FSTP ST(i), FXCH, FCHS and FABS as slot copies and sign
 *     bit edits.
 *
 * THE REGISTER FILE STAYS EXT80. Nothing is cached in host registers across
 * guest instructions: each form reads its operands from X86pX87, computes in
 * d0/d1, and writes the result back before the instruction ends. That keeps
 * FSAVE/FRSTOR, FLD/FSTP m80, MMX's aliasing of the significand and every
 * helper exactly as they were, and there is nothing to spill at a call, an
 * exit or a fault. What it drops is the call and the C conversions around the
 * operation.
 *
 * EVERY GUARD RUNS BEFORE ANY STATE CHANGES. A form that meets anything
 * outside its ordinary case -- an empty or full stack, a NaN, an infinity, a
 * subnormal, a result that is not a binary64 normal, a directed rounding mode,
 * single precision, an armed op census -- branches to the helper sequence the
 * caller emits after it, which then runs from the untouched state. So a
 * refusal changes speed and never an answer, and tests/test_jit_x64_x87.c
 * holds each form to the interpreter on both sides of each guard.
 *
 * The two conversions every form needs (ext80 slot to d0, d0 to ext80 slot)
 * are routines the block emits once in its tail and the forms reach with BL.
 */
#ifndef X86PORT_JIT_ARM64_X87_INLINE_H
#define X86PORT_JIT_ARM64_X87_INLINE_H

#include "decode.h"
#include "jit_arm64_internal.h"

/*
 * The largest the two tail routines can be. A block reserves this much tail
 * the first time one of its forms calls a routine, and only when the buffer
 * still has room for it beside the instruction and epilogue bounds; the
 * translator's look-ahead assumes it is always reserved, which can only end a
 * predicted block earlier. Enforced by the translator's tail bound.
 */
#define X86P_A64_X87_ROUTINE_BYTES 192u

/* Where a form sends its refusals, and where its fast path rejoins. */
typedef struct X87Slow {
  X86pA64EmitSite refusals[16];
  unsigned refusal_count;
  X86pA64EmitSite done;
  int fast; /* a fast path was emitted and ends in `done` */
} X87Slow;

/*
 * Each emits the fast path of its form and returns 1, or emits nothing and
 * returns 0 when the form has none (an integer operand, a host without ext80
 * storage, no tail room). After a 1 the caller emits x87_slow_begin, its
 * helper sequence, and x87_slow_end.
 *
 * The memory forms other than the store expect the operand's raw bits in
 * X87_BITS_REG, already loaded from the prepared address; the helper sequence
 * reads them from there too. The store leaves the bits to write in CARRY_REG
 * and its `done` is where the caller's store of CARRY_REG begins.
 */
#define X87_BITS_REG kA64X6
int emit_x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Slow *s);
int emit_x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Slow *s);
int emit_x87_inline_store(BlockCtx *c, const X86pInsn *insn, X87Slow *s);
int emit_x87_inline_compare_mem(BlockCtx *c, const X86pInsn *insn, X87Slow *s);
/* FLD ST(i) and FST/FSTP ST(i). */
int emit_x87_inline_copy(BlockCtx *c, const X86pInsn *insn, X87Slow *s);
/* FCOM* ST(i), FXCH, FCHS and FABS. */
int emit_x87_inline_register(BlockCtx *c, const X86pInsn *insn, X87Slow *s);

void x87_slow_begin(BlockCtx *c, X87Slow *s);
void x87_slow_end(BlockCtx *c, X87Slow *s);

/* Pop `pops` occupied registers: tag Empty, TOP + 1. The caller has proved
   they are occupied; reads TOP from memory, so any sequence may precede it. */
void emit_x87_pops(BlockCtx *c, unsigned pops);

/* The tail routines the block's forms called, if any. */
void emit_x87_routines(BlockCtx *c);

#endif /* X86PORT_JIT_ARM64_X87_INLINE_H */
