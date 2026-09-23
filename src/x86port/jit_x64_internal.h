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

#include "bit_ops.h"
#include "cpu.h"
#include "decode.h"
#include "emit_x64.h"
#include "jit_chain.h"
#include "jit_x64.h"
#include "jit_x64_abi.h"

#include <stddef.h>
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

/* The x87 TOP and occupancy cache across a block's x87 forms
   (jit_x64_x87_inline.h). Callee-saved, so helper calls keep them, and saved
   by the block's frame (jit_x64_abi.h). */
#define X87_TOP_REG kX64R14
#define X87_FULL_REG kX64R15

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
  const X86pMem *mem;
  /* x86p_jit_host_state() for this translation; see jit_x64.h. */
  uint32_t host_state;
  /* The engine's chain slots, or NULL: exits then only return. */
  X86pJitChain *chain;
  unsigned chain_exits;
  unsigned chain_exits_unslotted;
  /* The guest address this block translates, and the exits that jump to its
     front-array probe (jit_chain.h, THE PROBE) when they miss their slot. */
  uint32_t entry_eip;
  X86pEmitSite chain_probes[4];
  unsigned nchain_probes;
  /* How many guest x87 registers the host x87 stack mirrors here; see
     jit_x64_x87_inline.h. */
  unsigned x87_depth;
  /* Calls into the block's mirror loader, each with the depth it loads; the
     loader is emitted once, after the exits, when there is any. */
  X86pEmitSite x87_loads[MAX_INSNS * 2];
  unsigned nx87_loads;
  /* Bit k: host ST(k) holds a value the register file does not have yet. */
  unsigned x87_dirty;
  /* Whether R14 and R15 hold TOP and the occupancy (THE CACHE), whether the
     previous instruction was an x87 form, and whether its inline form ran;
     and the calls into the block's routine that reads them back. */
  int x87_regs_live;
  int x87_open;
  int x87_kept;
  X86pEmitSite x87_syncs[MAX_INSNS * 2];
  unsigned nx87_syncs;
  /* Whether the mirror was ever non-empty, so a fault or slow path may find
     values on the host stack to store. */
  int x87_used;
  /* Calls into the block's two write-back routines, emitted with the loader:
     one stores and pops host ST(0), the other every value the host holds. */
  X86pEmitSite x87_store_pops[MAX_INSNS * 4];
  unsigned nx87_store_pops;
  X86pEmitSite x87_spills[MAX_INSNS + 1];
  unsigned nx87_spills;
  X86pEmitSite x87_write_backs[MAX_INSNS * 2 + 1];
  unsigned nx87_write_backs;
  /* The guest register cache (jit_x64_gpr.h): the cache slot + 1 each guest
     register was given, or 0; how many slots are given out; the registers
     whose host copy equals memory; and the emitter's calls + bound sites when
     that set was last known good. */
  uint8_t gpr_slot[8];
  unsigned gpr_slots;
  unsigned gpr_live;
  unsigned gpr_epoch;
  unsigned flag_helper_calls;
  unsigned conds;
  unsigned cond_unknown_kind;
  unsigned cond_helper_calls;
  unsigned cond_inline;
  MemPlan plan;
  X86pEmitSite faults[MAX_INSNS * 2];
  unsigned nfaults;
  X86pEmitSite divide_faults[MAX_INSNS];
  unsigned ndivide_faults;
  /* A Jcc's inline condition whose recorded-kind guard failed is completed by
     x86p_cond out of line, after the block's exits, and jumps back. A Jcc ends
     its block, so there is at most one, and X86P_JIT_EPILOGUE_BYTES holds it. */
  int has_cond_slow;
  X86pEmitSite cond_slow_guard;
  size_t cond_slow_resume;
  uint8_t cond_slow_cond;
} BlockCtx;

/* Emit the out-of-line x86p_cond path recorded in `c`, if any; after the
   exits. */
void x86p_x64_emit_cond_slow_path(BlockCtx *c);

/*
 * Leave HOSTPTR_REG pointing at the guest operand of width `w`, or record a
 * fault site that jumps to the block's fault stub. The width is the ACCESS
 * width, not a constant: a two-byte access ending one byte past the mapping
 * must be refused.
 */
void emit_mem_prepare_w(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w);

/* The pieces of that, for the forms that compose them differently
   (jit_x64_mem.c): LEA's offset without a segment base, the linear address an
   access uses, the bounds check whose returned site the caller binds, and the
   host pointer for an address that passed it. */
void emit_address_parts(BlockCtx *c, const X86pOperand *o);
void emit_effective_address(BlockCtx *c, const X86pOperand *o);
X86pEmitSite emit_bounds_check(X86pEmit *e, const MemPlan *plan, uint32_t insn_eip, int w);
void emit_host_pointer(X86pEmit *e, const MemPlan *plan);
/* Record a site that jumps to the block's memory-fault or divide-error stub. */
void note_fault(BlockCtx *c, X86pEmitSite site);
void note_divide_fault(BlockCtx *c, X86pEmitSite site);

/* Guest CPU layout, shared by every x64 emission family. These were statics in
   jit_x64.c until jit_x64_cond.c needed the same two answers; jit_arm64_internal.h
   states the identical, arch-independent forms. */
static inline int32_t reg_off(int index) {
  return (int32_t)(offsetof(X86pCpu, reg) + (size_t)index * sizeof(uint32_t));
}

static inline int32_t flags_off(void) {
  return (int32_t)offsetof(X86pCpu, flags);
}

/*
 * The recorded flag kind and operand width, as ONE 16-bit field: the inline
 * condition guard reads them with one load, so they are written with one
 * store. Two byte stores cannot forward to that wider load, and on the Dead
 * Zone route waiting for them to retire was 7% of translated-code samples.
 */
_Static_assert(offsetof(X86pFlags, w) == offsetof(X86pFlags, kind) + 1u,
               "kind and width are stored and read as one 16-bit value");

/* The lazy tuple's fields. */
#define FLAG_A (flags_off() + (int32_t)offsetof(X86pFlags, a))
#define FLAG_B (flags_off() + (int32_t)offsetof(X86pFlags, b))
#define FLAG_R (flags_off() + (int32_t)offsetof(X86pFlags, r))
#define FLAG_CARRY_IN (flags_off() + (int32_t)offsetof(X86pFlags, carry_in))

static inline int32_t flag_kind_off(void) {
  return flags_off() + (int32_t)offsetof(X86pFlags, kind);
}

static inline uint16_t flag_kind_word(unsigned kind, unsigned w) {
  return (uint16_t)(kind | (w << 8));
}

/*
 * Where a guest register operand of width `w` lives, as a byte offset.
 *
 * The host is little-endian and the guest slot is a dword, so the low byte of a
 * register is the slot's first byte and a HIGH byte register (AH, CH, DH, BH)
 * is its second. That means narrow writes need no read-modify-write at all: a
 * one-byte store to the right offset preserves the other 24 bits by
 * construction, which is exactly the rule x86p_reg_write states.
 *
 * x86p_byte_reg owns which register an index names -- indices 4..7 are the
 * SECOND byte of EAX..EBX, not four different registers -- so this does not
 * restate it.
 */
static inline int32_t reg_off_w(int index, int w) {
  if (w == 1) {
    int shift = 0;
    int r = x86p_byte_reg(index, &shift);
    return reg_off(r) + shift / 8;
  }
  return reg_off(index);
}

/* Load a guest value of width `w` into `dst`, zero-extended. The upper bits are
   cleared rather than left alone because the value is about to be stored back
   at that width, and stale high bits would be written into the neighbouring
   part of a register the guest still owns. */
static inline void emit_load_w(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp, int w) {
  if (w == 1) {
    x86p_emit_load8_zx(e, dst, base, disp);
  } else if (w == 2) {
    x86p_emit_load16_zx(e, dst, base, disp);
  } else {
    x86p_emit_load32(e, dst, base, disp);
  }
}

static inline void emit_store_w(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src, int w) {
  if (w == 1) {
    x86p_emit_store8_reg(e, base, disp, src);
  } else if (w == 2) {
    x86p_emit_store16_reg(e, base, disp, src);
  } else {
    x86p_emit_store32(e, base, disp, src);
  }
}

static inline void emit_store_imm_w(X86pEmit *e, X86pHostReg base, int32_t disp, uint32_t imm, int w) {
  if (w == 1) {
    x86p_emit_store8_imm(e, base, disp, (uint8_t)(imm & 0xFFu));
  } else if (w == 2) {
    x86p_emit_store16_imm(e, base, disp, (uint16_t)(imm & 0xFFFFu));
  } else {
    x86p_emit_store32_imm(e, base, disp, imm);
  }
}

void emit_epilogue(X86pEmit *e, uint32_t next_eip, X86pJitExit exit);
void emit_epilogue_from(X86pEmit *e, X86pHostReg eip_reg, X86pJitExit exit);
/* The routines a block's exits and x87 forms call, after its exits and fault
   stubs: the chain probe and the x87 cache's (jit_x64_x87_inline.h). */
void emit_tail_routines(BlockCtx *c);

/* The block's exits to a next guest EIP: chained through a slot of
   `c->chain` when the translation has one (jit_chain.h), otherwise the plain
   return. emit_block_end chains only kX86pJitExitBlockEnd. emit_exit_from is
   for a target computed at run time, and asks the front array when it misses
   its slot (jit_chain.h, THE PROBE). */
void emit_exit(BlockCtx *c, uint32_t next_eip);
void emit_exit_from(BlockCtx *c, X86pHostReg eip_reg);
void emit_block_end(BlockCtx *c, uint32_t next_eip, X86pJitExit exit);
/* EAX nonzero: to `taken`; zero: to `not_taken`. */
void emit_two_way_exit(BlockCtx *c, uint32_t taken, uint32_t not_taken);
void emit_loop(BlockCtx *c, const X86pInsn *insn, uint32_t target, uint32_t next);

/* The guest ALU operations emitted as host arithmetic plus the lazy tuple
   (jit_x64_alu.c). `last_kind` is the kind the previous flag writer in this
   block recorded, or -1; `flags_dead` skips the tuple a later instruction
   overwrites unread. */
int inline_alu_shape(uint8_t alu, X86pHostAlu *host, X86pFlagKind *kind, int *writes_dest);
void emit_alu_inline(BlockCtx *c,
                     const X86pInsn *insn,
                     X86pHostAlu host,
                     X86pFlagKind kind,
                     int writes_dest,
                     int last_kind,
                     int flags_dead,
                     uint32_t insn_eip);
/* INC, DEC, NEG and NOT: the kind recorded, or -1 for NOT. */
int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip);
void emit_alu_helper(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);

/* What emit_shift_inline says the flag state holds afterwards, besides a kind
   it recorded: a count known to be zero writes no flags, and a CL count is not
   known until the block runs. */
#define SHIFT_FLAGS_UNKNOWN (-1)
#define SHIFT_FLAGS_UNCHANGED (-2)

int is_inline_shift(uint8_t alu);
int emit_shift_inline(BlockCtx *c, const X86pInsn *insn, int flags_dead, uint32_t insn_eip);

void emit_cpu_transfer(BlockCtx *c, uint8_t op);

int simd_bits_is_emittable(const X86pInsn *insn);
void emit_simd_bits(BlockCtx *c, const X86pInsn *insn, uint32_t pc);

int x87_register_is_emittable(const X86pInsn *insn);
void emit_x87_register(BlockCtx *c, const X86pInsn *insn);

void emit_mul32(BlockCtx *c, const X86pInsn *insn, uint32_t pc);

int double_shift_is_emittable(const X86pInsn *insn);
void emit_double_shift(BlockCtx *c, const X86pInsn *insn, uint32_t pc);

#endif /* X86PORT_JIT_X64_INTERNAL_H */
