/*
 * jit_arm64_internal.h -- the contract between jit_arm64.c and its per-family
 * emission units (currently jit_arm64_x87.c).
 *
 * The AArch64 counterpart of jit_x64_internal.h: same shape, same contract,
 * different host registers and a different emitter type. NOT a public header.
 */
#ifndef X86PORT_JIT_ARM64_INTERNAL_H
#define X86PORT_JIT_ARM64_INTERNAL_H

#include "cpu.h"
#include "decode.h"
#include "emit_arm64.h"

#include <stdint.h>

/*
 * The host register holding the X86pCpu pointer for the life of a block. X19
 * is callee-saved, so the BLR helper calls this backend emits cannot clobber
 * it -- the AArch64 counterpart of CPU_REG=RBX on the x64 side.
 */
#define CPU_REG kA64X19

/*
 * Scratch roles in the emitted memory sequence, kept OUTSIDE x0..x7 so they
 * survive across the AAPCS64 argument setup for every helper call: x0..x7 are
 * reloaded fresh immediately before each BLR, exactly as the x64 backend
 * reloads RDI/RSI/RDX/RCX/R8 last. A collision between a role register and an
 * argument register would be silent -- the call would receive a stale
 * address instead of the one just computed.
 */
#define EA_REG kA64X10      /* the guest effective address, 32-bit */
#define HOSTPTR_REG kA64X11 /* the host address it maps to */
#define FAULTPC_REG kA64X12 /* guest EIP to report if the access faults */
/*
 * ADDR_TMP and CARRY_REG must be DIFFERENT registers, and that is a
 * correctness constraint rather than a preference -- see jit_x64_internal.h's
 * comment on the same pair. The carry-in bit is computed before the address,
 * from the OLD flag state, but must not be stored until the bounds check has
 * passed, so it stays live in CARRY_REG across the whole address computation.
 */
#define ADDR_TMP kA64X13  /* index/offset scratch during address forming */
#define CARRY_REG kA64X14 /* the pending carry-in bit, until safe to store */

/* The indirect branch/call target, read before anything else touches memory
   (see emit_read_branch_target in jit_arm64.c). Its own role register for the
   same reason: CALL [ESP+4] must read the stack as it stands before the
   return address push disturbs it. */
#define TARGET_REG kA64X15

/* x8 and x9 are the ENCODER's own internal scratch (see emit_arm64.c's
   A64_SCRATCH and its store8_imm/store16_imm/store32_imm/cmp_w_imm
   fallbacks) -- never used here to carry a value across more than one
   emit_arm64.h call, and never assigned a role above. */

/* Guest instructions per block. */
#define MAX_INSNS 64

/*
 * THE MAPPING IS BAKED IN AS CONSTANTS -- see jit_x64_internal.h's MemPlan for
 * the full rationale, which applies identically here.
 */
typedef struct MemPlan {
  uint64_t host;
  uint32_t lo;
  uint32_t size;
} MemPlan;

/*
 * Per-block emission state. The fault sites are collected rather than bound
 * as they are made, because they all jump to ONE stub emitted after the
 * normal epilogue.
 */
typedef struct BlockCtx {
  X86pA64Emit *e;
  const X86pMem *mem;
  unsigned flag_helper_calls;
  MemPlan plan;
  X86pA64EmitSite faults[MAX_INSNS * 2];
  unsigned nfaults;
  X86pA64EmitSite divide_faults[MAX_INSNS];
  unsigned ndivide_faults;
} BlockCtx;

/*
 * Leave HOSTPTR_REG pointing at the guest operand of width `w`, or record a
 * fault site that jumps to the block's fault stub. The width is the ACCESS
 * width, not a constant: a two-byte access ending one byte past the mapping
 * must be refused.
 */
void emit_mem_prepare_w(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w);

#endif /* X86PORT_JIT_ARM64_INTERNAL_H */
