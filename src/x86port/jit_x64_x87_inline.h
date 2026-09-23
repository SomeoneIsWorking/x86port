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
 * WRITE-BACK IS LAZY. A result stays on the host stack, marked dirty in
 * BlockCtx.x87_dirty, and reaches the register file only when something needs
 * it there. TOP, the tags and the status word are still written at every
 * instruction; only the ten-byte values wait. A value is stored when:
 *
 *   - (never when it is popped: a popped value is dropped, since an empty
 *     register's value is unspecified -- see X86pX87 in x87.h);
 *   - a full mirror lets go of it to make room for a push;
 *   - the mirror is flushed (x87_cache_flush) before an instruction that calls
 *     out or ends the block, so a call or an exit always finds the host stack
 *     empty and the register file complete. The mirror's depth and dirty set
 *     are known there, so up to two stores are inline and more are one call
 *     to the block's write_back routine with that state in ECX;
 *   - a guard takes its slow path: the same, with the state the guards left
 *     with -- unless a sequence's guards left with different ones, when the
 *     slow path cannot tell which jumped;
 *   - an access faults, from a site the shared stub cannot tell either. Those
 *     two call the block's spill routine, which stores whatever the host stack
 *     holds and empties it. The host stack holds the mirror and nothing else,
 *     so that needs no record of the depth at the site.
 *
 * Before this, every result was also written through with an FLD ST(0) and a
 * ten-byte FSTP, and those two were a fifth of the samples inside translated
 * code on the Dead Zone route. Storing a clean value again is exact -- FLD
 * and FSTP of a ten-byte value change no bits -- so a path that merges a clean
 * register with a dirty one keeps it dirty.
 *
 * A mirrored register may be EMPTY in the guest -- a slow path's reload
 * copies whatever bits an empty slot holds -- so every tag guard stays: the
 * mirror supplies values, never the answer to whether a register is occupied.
 *
 * THE CACHE answers it instead of memory. R14 holds TOP and R15 which
 * registers are occupied, relative to it, across the block's x87 forms, so a
 * tag guard is one TEST of a register. Each guard had loaded TOP -- which the
 * previous form had just stored -- computed an index and loaded the tag
 * behind it, and that chain was about a fifth of the samples inside
 * translated code on the Dead Zone route. Memory stays authoritative: every
 * form still writes TOP and the tags, so an exit, a fault or a helper finds
 * them current, and the cache is only read back from them: at the first form
 * after anything that calls out, and after every slow path's helper.
 */
#ifndef X86PORT_JIT_X64_X87_INLINE_H
#define X86PORT_JIT_X64_X87_INLINE_H

#include "jit_x64_internal.h"

/* X87Inline.spill for guards that leave with different mirrors: the slow path
   then spills whatever the host stack holds. */
#define X87_SPILL_UNKNOWN (~0u)

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
  /* The mirror depth the fast path ends with, which the slow path rebuilds,
     and its dirty set. */
  unsigned depth;
  unsigned dirty;
  /* The mirror state every guard jumps with, as the write-back routine takes
     it; 0 for an empty mirror, X87_SPILL_UNKNOWN when the guards differ. */
  unsigned spill;
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

/* Before each instruction: the TOP and occupancy cache is stale after an x87
   instruction whose inline form did not run, and the mirror is flushed and the
   cache dropped before one that does not keep the mirror (it calls out or ends
   the block). */
void x87_cache_before(BlockCtx *c, const X86pInsn *insn, int keeps_mirror);

/* Store the mirror's dirty values and pop it: the host stack is empty after
   this, statically. */
void x87_cache_flush(BlockCtx *c);
/* Store and empty whatever the host stack holds, at a point whose mirror
   depth is not known statically: the shared memory fault stub. */
void x87_cache_spill(BlockCtx *c);
/* The block's mirror loader and write-back routines, which the mirror's
   reloads, stores and spills call: after the exits and the fault stubs, and
   only those that were called. */
void x87_cache_emit_routines(BlockCtx *c);

/* FLD m32/m64, FILD m16/m32/m64 and FLD ST(i). A memory operand must already
   be prepared in HOSTPTR_REG. */
void x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FADD/FSUB/FMUL/FDIV (+R, +P) with a register or a prepared memory source. */
void x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FXCH, FCHS, FABS, and FCOM/FCOMP/FCOMPP/FUCOM* against a register. */
void x87_inline_register(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

/* FSQRT; the other function forms take the helper alone. */
void x87_inline_fn(BlockCtx *c, const X86pInsn *insn, X87Inline *fast);

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
