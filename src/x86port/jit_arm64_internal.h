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
#include "jit_chain.h"
#include "jit_x64.h"

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

/*
 * The host address of the block's mapping (MemPlan.host), set once by the
 * prologue and held for the whole dispatcher entry. Callee-saved, so every
 * helper and leaf call keeps it; a chained transfer enters past the prologue
 * into a block translated against the same mapping, so the value is already
 * right. A guest access then forms its host address with one ADD instead of
 * materialising the 64-bit base with up to four MOVZ/MOVK every time.
 * X23 is saved with it only to keep the frame a pair; nothing uses it.
 */
#define MEM_BASE_REG kA64X22

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
  uint32_t guard_above; /* X86pMem.guard_above */
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
  unsigned conds;
  unsigned cond_helper_calls;
  unsigned cond_inline;
  unsigned cond_unknown_kind;
  MemPlan plan;
  X86pA64EmitSite faults[MAX_INSNS * 2];
  unsigned nfaults;
  X86pA64EmitSite divide_faults[MAX_INSNS];
  unsigned ndivide_faults;
  /* Exit slots for chained transfers (jit_chain.h), or NULL: every exit then
     returns to the dispatcher. */
  X86pJitChain *chain;
  unsigned chain_exits;
  unsigned chain_exits_unslotted;
  /* Exits that missed their slot and jump to the block's one probe. */
  X86pA64EmitSite chain_probes[4];
  unsigned nchain_probes;
  /* Branches to the block's one return from a chained exit, with W4 naming
     the pending slot (jit_arm64_branch.c): each exit's miss and transfer
     checks, and the probe's. */
  X86pA64EmitSite chain_leaves[16];
  unsigned nchain_leaves;
  /* Leaves (X86pJitLeafFn), only with `chain`: a leaf returns into the block
     through a chained exit. */
  X86pJitLeafResolveFn leaf;
  void *leaf_user;
  X86pJitLeafSites *leaf_sites;
  unsigned leaf_calls;
  unsigned leaf_site_count;
  /* The block's leaf site's refill, which runs only on a miss and so lives in
     the tail (jit_arm64_branch.c): the miss that enters it and the body's
     offsets it returns to. A CALL ends its block, so there is at most one. */
  struct X86pJitLeafSite *site_refill;
  X86pA64EmitSite site_miss;
  size_t site_call_leaf;
  size_t site_reload;
  size_t site_ordinary;
  /* The x87 inline paths' two shared routines (jit_arm64_x87_inline.c): the
     BL sites that call each, bound when the tail emits it. An arithmetic or
     register compare narrows two registers, so a block makes at most two
     narrowing calls per instruction and one widening call. */
  X86pA64EmitSite x87_narrow_calls[MAX_INSNS * 2];
  unsigned x87_narrow_count;
  X86pA64EmitSite x87_widen_calls[MAX_INSNS];
  unsigned x87_widen_count;
  /* Tail bytes reserved beyond X86P_JIT_EPILOGUE_BYTES, for those routines. */
  size_t tail_reserve;
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
/* Block entry and exits (jit_arm64_branch.c). A transfer enters a block past
   its prologue, so the prologue is emitted there too. */
void emit_prologue(X86pA64Emit *e, uint64_t mem_base);
/* Leave the frame the prologue opened and return X0 to the dispatcher. */
void emit_frame_return(X86pA64Emit *e);
void emit_epilogue(X86pA64Emit *e, uint32_t next_eip, X86pJitExit exit);
void emit_epilogue_from(X86pA64Emit *e, X86pA64Reg eip_reg, X86pJitExit exit);
/* The exit to a guest EIP, chained through a slot when the block has them. */
void emit_exit(BlockCtx *c, uint32_t next_eip);
void emit_exit_from(BlockCtx *c, X86pA64Reg eip_reg);
/* The block's last exit: chained when it is a plain block end. */
void emit_block_end(BlockCtx *c, uint32_t next_eip, X86pJitExit exit);
/* NZCV holds `cc` true: to `taken`; otherwise to `not_taken`. Two exits
   rather than one to a selected address, so each keeps its own slot. */
void emit_exits_on(BlockCtx *c, X86pA64Cond cc, uint32_t taken, uint32_t not_taken);
/* A CALL's end, its return address already pushed: to the target, or its
   leaf completes the CALL in place. The indirect form takes its target in
   TARGET_REG and asks a leaf site (jit_leaf_sites.h). */
void emit_call_exit(BlockCtx *c, uint32_t return_eip, uint32_t target);
void emit_call_indirect_exit(BlockCtx *c, uint32_t return_eip);
/* After every exit and fault stub: the chain probe the exits jump to. */
void emit_tail_routines(BlockCtx *c);
void emit_loop(BlockCtx *c, const X86pInsn *insn, uint32_t target, uint32_t next);

/* Jcc and SETcc (jit_arm64_cond.c): the condition read off the host flags
   when the block knows who wrote them (`last_kind`, `last_w`), else x86p_cond. */
void emit_jcc(BlockCtx *c, uint8_t cond, uint32_t target, uint32_t fallthrough, int last_kind, int last_w);
void emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int last_kind, int last_w);

void emit_alu_helper(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);

void emit_cpu_transfer(BlockCtx *c, uint8_t op);

int simd_bits_is_emittable(const X86pInsn *insn);
void emit_simd_bits(BlockCtx *c, const X86pInsn *insn, uint32_t pc);

int x87_register_is_emittable(const X86pInsn *insn);
void emit_x87_register(BlockCtx *c, const X86pInsn *insn);

void emit_mul32(BlockCtx *c, const X86pInsn *insn, uint32_t pc);

int double_shift_is_emittable(const X86pInsn *insn);
void emit_double_shift(BlockCtx *c, const X86pInsn *insn, uint32_t pc);

#endif /* X86PORT_JIT_ARM64_INTERNAL_H */
