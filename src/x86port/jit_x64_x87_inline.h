/*
 * jit_x64_x87_inline.h -- the x87 register file, accessed from emitted code.
 *
 * jit_x64_x87.c lowers every x87 instruction to a call into the x86p_x87_*
 * owners. On the Dead Zone gameplay route that call, not the arithmetic, is the
 * cost: the x87 helper family was 57% of the frame, and one guest FMUL paid a
 * call, two ten-byte loads, a ten-byte store, a tag classification through a
 * second store, and a return.
 *
 * This unit emits the ORDINARY case of the hottest forms inline -- FLD, the
 * four arithmetic operations and their R/P variants, and FST/FSTP to a
 * register or to 32/64-bit memory -- and names every case it does not own as a
 * jump to the caller's existing helper sequence:
 *
 *   - a stack fault: an empty source, an empty destination, a full push slot;
 *   - a guest control word that differs from the host's, which the helpers
 *     answer with an FLDCW sandwich;
 *   - a divisor of zero, whose ZE status the helper owns;
 *   - an armed op census, which counts inside the helper.
 *
 * Inside those guards the helper's own work on an x87 host IS one host
 * instruction on the same ten-byte values, so the answer, the tag, TOP and the
 * status word are the ones the helper would have written. The block
 * differentials in tests/test_jit_x64.c and
 * tests/test_jit_x64_x87.c compare the whole x87 state against the
 * interpreter, including the fallback cases.
 */
#ifndef X86PORT_JIT_X64_X87_INLINE_H
#define X86PORT_JIT_X64_X87_INLINE_H

#include "jit_x64_internal.h"

/* The most guards one inline sequence emits. */
#define X87_INLINE_MAX_SLOW 8

/*
 * An emitted fast path: every guard's jump to the slow path, and the jump the
 * completed fast path takes over it. `emitted` is 0 when this host or this
 * operand shape has no inline form; the caller then emits only its helper
 * sequence and binds nothing.
 */
typedef struct X87Inline {
  int emitted;
  X86pEmitSite slow[X87_INLINE_MAX_SLOW];
  unsigned nslow;
  X86pEmitSite done;
} X87Inline;

/* The host x87 control word, read now; 0 where this unit emits nothing. The
   x86-64 backend's x86p_jit_host_state(). */
uint32_t x87_inline_host_control(void);

/* Bind the guards to the caller's helper sequence, which follows. */
void x87_inline_begin_slow(X86pEmit *e, X87Inline *fast);
/* Bind the fast path's completion jump after that sequence. */
void x87_inline_end(X86pEmit *e, X87Inline *fast);

/* FLD m32/m64, FILD m16/m32/m64 and FLD ST(i). A memory operand must already
   be prepared in HOSTPTR_REG. */
void x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FADD/FSUB/FMUL/FDIV (+R, +P) with a register or a prepared memory source. */
void x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FST/FSTP ST(i). */
void x87_inline_store_reg(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/*
 * FST/FSTP m32/m64. Prepares the memory operand itself, AFTER the empty-ST(0)
 * guard, because an empty ST(0) must not fault on a bad address.
 */
void x87_inline_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, X87Inline *fast);

#endif /* X86PORT_JIT_X64_X87_INLINE_H */
