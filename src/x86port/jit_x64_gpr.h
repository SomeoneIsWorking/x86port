/*
 * jit_x64_gpr.h -- the guest register cache: guest GPRs held in host registers
 * across a block's instructions.
 *
 * Every guest register access was a load or a store of X86pCpu.reg, so a value
 * one instruction wrote reached the next through memory: a store, then a load
 * that waits for store-to-load forwarding. On the Dead Zone route the
 * instructions right after those loads were a sixth of the samples inside
 * translated code.
 *
 * THE CACHE is WRITE-THROUGH. A block gives up to GPR_CACHE_SLOTS guest
 * registers a callee-saved host register each, the first ones it uses at full
 * width. Every store still writes X86pCpu.reg, and a full-width store also
 * writes the host register, so memory is always complete: an exit, a fault, a
 * helper and the next block find the guest state where they always did, and
 * nothing is ever written back. A load reads the host register instead of
 * memory while the cache knows the two agree.
 *
 * WHEN THEY AGREE. BlockCtx.gpr_live has a bit per guest register whose host
 * register equals memory. It is only trusted between two points of the emitted
 * code with nothing between them that could change memory behind the cache or
 * arrive from elsewhere: the live set is dropped whenever the emitter has
 * emitted a call (a helper may write X86pCpu.reg) or bound a jump (another path
 * arrives there, with its own state) since the set was built, and explicitly at
 * a position a later jump will return to (gpr_forget). A narrow store drops
 * that register's bit rather than merging into the host register.
 *
 * A dropped register is loaded from memory again at its next use, which is
 * what every access did before, so forgetting too much costs only speed.
 * Trusting too much reads a stale value: the checked build
 * (X86P_JIT_GPR_CHECK) compares every live host register with memory at every
 * instruction boundary and traps on a difference, and the block differentials
 * run under it.
 */
#ifndef X86PORT_JIT_X64_GPR_H
#define X86PORT_JIT_X64_GPR_H

#include "jit_x64_internal.h"

#ifndef X86P_JIT_GPR_CHECK
#define X86P_JIT_GPR_CHECK 0
#endif

/* Load guest register operand `reg` at width `w` into `dst`, zero-extended --
   emit_load_w's contract, with reg_off_w's naming of a byte register. */
void gpr_load(BlockCtx *c, X86pHostReg dst, int reg, int w);

/* Store `src` (or `imm`) to guest register operand `reg` at width `w`. */
void gpr_store(BlockCtx *c, int reg, X86pHostReg src, int w);
void gpr_store_imm(BlockCtx *c, int reg, uint32_t imm, int w);

/* A position a later jump returns to: nothing is known there. */
void gpr_forget(BlockCtx *c);

/* Between two guest instructions: in the checked build, trap unless every live
   host register equals memory. Emits nothing otherwise. */
void gpr_check(BlockCtx *c);

#endif /* X86PORT_JIT_X64_GPR_H */
