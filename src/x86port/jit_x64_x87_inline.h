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
 *
 * THE MIRROR. Consecutive inline forms keep their values on the host x87
 * stack: host ST(k) holds a copy of guest ST(k) for every k below
 * BlockCtx.x87_depth. Each sequence reads its register operands from there and
 * leaves its result there. Before the mirror, every operation stored its
 * result as ten bytes and the next one loaded it straight back; both are
 * microcoded, and that store-to-load chain was about a third of the samples
 * inside translated code on the Dead Zone route.
 *
 * The mirror is a cache of the register file and never its owner. Every
 * result is still stored to the guest register (write-through), with its tag
 * and TOP, so the guest state in memory is complete at every instruction
 * boundary. That is what keeps each way out cheap and exact:
 *
 *   - a guard's slow path discards the host stack before its helper call, as
 *     the ABI requires, and reloads the mirror from memory afterwards;
 *   - the memory fault stub discards it before returning (x87_cache_discard);
 *   - any instruction that is not an inline form, and the end of the block,
 *     first pops it (x87_cache_flush), so a call, a helper or an exit always
 *     finds the host stack empty.
 *
 * A mirrored register may be EMPTY in the guest -- a slow path's reload
 * copies whatever bits an empty slot holds -- so every tag guard stays: the
 * mirror supplies values, never the answer to whether a register is occupied.
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
  /* The mirror depth the fast path ends with, which the slow path rebuilds. */
  unsigned depth;
} X87Inline;

/* The host x87 control word, read now; 0 where this unit emits nothing. The
   x86-64 backend's x86p_jit_host_state(). */
uint32_t x87_inline_host_control(void);

/* Bind the guards to the caller's helper sequence, which follows, and empty
   the host stack for its call. */
void x87_inline_begin_slow(BlockCtx *c, X87Inline *fast);
/* Rebuild the mirror the fast path left after that sequence, then bind the
   fast path's completion jump. */
void x87_inline_end(BlockCtx *c, X87Inline *fast);

/* Pop the mirror: the host stack is empty after this, statically. */
void x87_cache_flush(BlockCtx *c);
/* Empty the host stack at a point whose mirror depth is not known statically:
   the shared memory fault stub. */
void x87_cache_discard(X86pEmit *e);
/* The block's mirror loader, which every mirror reload calls: after the
   exits, and only when a reload was emitted. */
void x87_cache_emit_loader(BlockCtx *c);

/* FLD m32/m64, FILD m16/m32/m64 and FLD ST(i). A memory operand must already
   be prepared in HOSTPTR_REG. */
void x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FADD/FSUB/FMUL/FDIV (+R, +P) with a register or a prepared memory source. */
void x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FXCH, FCHS, FABS, and FCOM/FCOMP/FCOMPP/FUCOM* against a register. */
void x87_inline_register(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FCOM/FCOMP m32/m64 and FICOM m16/m32 with the operand prepared in
   HOSTPTR_REG. */
void x87_inline_compare_mem(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FLDZ and FLD1, the two constants that do not depend on the rounding
   control. */
void x87_inline_constant(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FNSTSW AX, which calls nothing and so keeps the mirror. */
void x87_inline_status_ax(BlockCtx *c);

/* FST/FSTP ST(i). */
void x87_inline_store_reg(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/*
 * FST/FSTP m32/m64. Prepares the memory operand itself, AFTER the empty-ST(0)
 * guard, because an empty ST(0) must not fault on a bad address.
 */
void x87_inline_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, X87Inline *fast);

#endif /* X86PORT_JIT_X64_X87_INLINE_H */
