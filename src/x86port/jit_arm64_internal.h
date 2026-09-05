/*
 * jit_arm64_internal.h -- the contract between jit_arm64.c and its per-family
 * integer and x87 emission units.
 *
 * The AArch64 counterpart of jit_x64_internal.h: same shape, same contract,
 * different host registers and a different emitter type. NOT a public header.
 */
#ifndef X86PORT_JIT_ARM64_INTERNAL_H
#define X86PORT_JIT_ARM64_INTERNAL_H

#include "cpu.h"
#include "decode.h"
#include "emit_arm64.h"

#include <stddef.h>
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

/* Shared CPU-layout and host-instruction primitives for emission families. */
static inline int32_t reg_off(int index) {
  return (int32_t)(offsetof(X86pCpu, reg) + (size_t)index * sizeof(uint32_t));
}

static inline int32_t flags_off(void) {
  return (int32_t)offsetof(X86pCpu, flags);
}

static inline int32_t flag_off(size_t field) {
  return (int32_t)(offsetof(X86pCpu, flags) + field);
}

#define FLAG_A flag_off(offsetof(X86pFlags, a))
#define FLAG_B flag_off(offsetof(X86pFlags, b))
#define FLAG_R flag_off(offsetof(X86pFlags, r))
#define FLAG_KIND flag_off(offsetof(X86pFlags, kind))
#define FLAG_W flag_off(offsetof(X86pFlags, w))
#define FLAG_CARRY_IN flag_off(offsetof(X86pFlags, carry_in))

/* ---- calling a host helper ------------------------------------------------
 * The AAPCS64 counterpart of x64's repeated "mov r64,imm64; call r64"
 * sequence: the target address is materialised into X9 -- the encoder's own
 * second scratch, never a role register and never live across more than this
 * one call -- and BLR'd immediately. Arguments must already be in X0..X7. */
static inline void emit_call(X86pA64Emit *e, void *fn) {
  x86p_a64_emit_mov_x_imm64(e, kA64X9, (uint64_t)(uintptr_t)fn);
  x86p_a64_emit_blr(e, kA64X9);
}

/* Where a guest register operand of width `w` lives, as a byte offset --
   identical to jit_x64.c's reg_off_w, arch-independent. */
static inline int32_t reg_off_w(int index, int w) {
  if (w == 1) {
    int shift = 0;
    int r = x86p_byte_reg(index, &shift);
    return reg_off(r) + shift / 8;
  }
  return reg_off(index);
}

static inline void emit_load_w(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp, int w) {
  if (w == 1) {
    x86p_a64_emit_load8_zx(e, dst, base, disp);
  } else if (w == 2) {
    x86p_a64_emit_load16_zx(e, dst, base, disp);
  } else {
    x86p_a64_emit_load32(e, dst, base, disp);
  }
}

static inline void emit_store_w(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src, int w) {
  if (w == 1) {
    x86p_a64_emit_store8_reg(e, base, disp, src);
  } else if (w == 2) {
    x86p_a64_emit_store16_reg(e, base, disp, src);
  } else {
    x86p_a64_emit_store32(e, base, disp, src);
  }
}

#endif /* X86PORT_JIT_ARM64_INTERNAL_H */
