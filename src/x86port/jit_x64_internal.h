/*
 * jit_x64_internal.h -- the contract between jit_x64.c and its per-family
 * emission units (currently jit_x64_x87.c).
 *
 * NOT a public header. It exposes exactly what a split-out emitter needs to
 * form a guest memory operand and thread block state: the host register roles,
 * the per-block MemPlan/BlockCtx, and emit_mem_prepare_w. Everything else about
 * a block stays private to jit_x64.c.
 */
#ifndef X86PORT_JIT_X64_INTERNAL_H
#define X86PORT_JIT_X64_INTERNAL_H

#include "cpu.h"
#include "decode.h"
#include "emit_x64.h"

#include <stdint.h>

/*
 * The host register holding the X86pCpu pointer for the life of a block. RBX is
 * callee-saved, so the helper calls the JIT emits cannot clobber it.
 */
#define CPU_REG kX64Rbx

/*
 * Scratch roles in the emitted memory sequence. Named rather than spelled at
 * each site, because a collision between the host pointer and an operand is
 * silent: the access simply reads the wrong address.
 */
#define EA_REG kX64Rax      /* the guest effective address, 32-bit */
#define HOSTPTR_REG kX64R11 /* the host address it maps to */
#define FAULTPC_REG kX64R10 /* guest EIP to report if the access faults */
/*
 * ADDR_TMP and CARRY_REG must be DIFFERENT registers, and that is a correctness
 * constraint rather than a preference. The carry-in bit is computed before the
 * address -- it reads the OLD flag state, which the operation is about to
 * overwrite -- but it must not be STORED until the bounds check has passed. So
 * the bit stays live in CARRY_REG across the whole address computation, and the
 * address machinery cannot use that register.
 */
#define ADDR_TMP kX64Rdi  /* index/offset scratch during address forming */
#define CARRY_REG kX64Rcx /* the pending carry-in bit, until it is safe to store */

/* Guest instructions per block. */
#define MAX_INSNS 64

/*
 * THE MAPPING IS BAKED IN AS CONSTANTS. A block embeds the host base, guest low
 * address, and size of the X86pMem it was translated against, so an access is a
 * bounds check and an add rather than a call. A block is only valid for the
 * mapping it was translated against; the block cache's flush is what discards
 * blocks when the guest memory is remapped, moved, or resized.
 */
typedef struct MemPlan {
  uint64_t host;
  uint32_t lo;
  uint32_t size;
} MemPlan;

/*
 * Per-block emission state. The fault sites are collected rather than bound as
 * they are made, because they all jump to ONE stub emitted after the normal
 * epilogue.
 */
typedef struct BlockCtx {
  X86pEmit *e;
  unsigned flag_helper_calls;
  MemPlan plan;
  X86pEmitSite faults[MAX_INSNS * 2];
  unsigned nfaults;
  /*
   * Sites where a helper reported a non-Ok status. They go to a DIFFERENT stub
   * from the bounds-check faults: that stub decides the exit code and the guest
   * EIP itself, whereas the helper has already set both.
   */
  X86pEmitSite helper_faults[MAX_INSNS];
  unsigned nhelper_faults;
  unsigned helper_calls;
  const X86pMem *mem;
} BlockCtx;

/*
 * Leave HOSTPTR_REG pointing at the guest operand of width `w`, or record a
 * fault site that jumps to the block's fault stub. The width is the ACCESS
 * width, not a constant: a two-byte access ending one byte past the mapping
 * must be refused.
 */
void emit_mem_prepare_w(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w);

#endif /* X86PORT_JIT_X64_INTERNAL_H */
