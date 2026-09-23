/*
 * jit_x64_mem.c -- guest memory operands in emitted code: the address the guest
 * computes, the bounds check against the block's mapping, the host pointer,
 * and the fault sites that check leaves for the block's stubs.
 */
#include "jit_x64_gpr.h"
#include "jit_x64_internal.h"

#include "cond.h"

#include <stddef.h>
#include <stdint.h>

/*
 * THE MAPPING IS BAKED IN AS CONSTANTS, and that is a contract, not an
 * oversight.
 *
 * A block embeds the host base, guest low address, and size of the X86pMem it
 * was translated against, so an access is a bounds check and an add rather than
 * a call. The cost is that a block is only valid for the
 * mapping it was translated against: if the guest memory is remapped, moved, or
 * resized, every block must be discarded. The block cache's flush is that
 * mechanism. A caller that remaps without flushing gets a block reading freed
 * host memory, so this is stated here and in the header rather than left to be
 * discovered.
 */

/*
 * Emit: EA_REG = base + index*scale + disp, as the guest computes it.
 *
 * 32-bit throughout, so the wrap at 4 GB is the guest's wrap. Widening any part
 * of this to 64 bits would make an address that the guest wraps address
 * something real instead.
 */
/*
 * base + index*scale + disp, WITHOUT the segment base.
 *
 * Separate from emit_effective_address because LEA computes exactly this and
 * no more: it produces the OFFSET, not the linear address, so a LEA that added
 * the FS base would hand the guest a pointer it never asked for. Two named
 * functions rather than a flag, because a flag at a call site is a thing to
 * get the wrong way round.
 */
void emit_address_parts(BlockCtx *c, const X86pOperand *o) {
  X86pEmit *e = c->e;
  int have_base = (o->base >= 0);
  if (have_base) {
    gpr_load(c, EA_REG, o->base, 4);
  } else {
    x86p_emit_mov_r32_imm32(e, EA_REG, 0u);
  }
  if (o->index >= 0) {
    unsigned shift = 0u;
    switch (o->scale) {
    case 2:
      shift = 1u;
      break;
    case 4:
      shift = 2u;
      break;
    case 8:
      shift = 3u;
      break;
    default:
      shift = 0u;
      break;
    }
    gpr_load(c, ADDR_TMP, o->index, 4);
    if (shift) {
      x86p_emit_shift_r32_imm8(e, kX64Shl, ADDR_TMP, (uint8_t)shift);
    }
    x86p_emit_alu_r32_r32(e, kX64Add, EA_REG, ADDR_TMP);
  }
  if (o->disp != 0) {
    x86p_emit_alu_r32_imm32(e, kX64Add, EA_REG, (uint32_t)o->disp);
  }
}

/*
 * The linear address an ACCESS uses: the offset plus the segment base.
 *
 * Only FS and GS have one -- see cpu.h on why the flat model is a contract --
 * and which segment an operand uses is resolved by the decoder, so this costs
 * nothing at all for the other four rather than a load and an add on every
 * memory access in the program.
 */
void emit_effective_address(BlockCtx *c, const X86pOperand *o) {
  X86pEmit *e = c->e;
  emit_address_parts(c, o);
  if (o->seg == (uint8_t)kX86pSegFs) {
    x86p_emit_alu_r32_mem(e, kX64Add, EA_REG, CPU_REG, (int32_t)offsetof(X86pCpu, fs_base));
  } else if (o->seg == (uint8_t)kX86pSegGs) {
    x86p_emit_alu_r32_mem(e, kX64Add, EA_REG, CPU_REG, (int32_t)offsetof(X86pCpu, gs_base));
  }
}

/* The guest address is the host address: no offset to subtract, no base to
   add. */
static int plan_is_identity(const MemPlan *plan) {
  return plan->lo == 0u && plan->host == 0u;
}

/* The register holding the offset into the mapping, EA - lo. With lo 0 that
   is the address itself, zero-extended by the 32-bit write that formed it,
   so no copy is made. */
static X86pHostReg plan_offset_reg(const MemPlan *plan) {
  return plan->lo == 0u ? EA_REG : ADDR_TMP;
}

/*
 * Bounds-check EA_REG and leave the host address in HOSTPTR_REG.
 *
 * ONE unsigned compare covers both ends: (addr - lo) as unsigned is huge when
 * addr is below lo, so `ja` catches underflow and overflow together. Writing
 * two signed comparisons instead is the classic way to let a negative offset
 * through.
 *
 * The check is against size - w, so an access that STARTS inside the mapping
 * and runs off the end is refused rather than truncated -- the same rule
 * x86p_mem_read enforces.
 *
 * An identity mapping (lo 0, host 0) compares EA_REG itself: the offset is
 * the address, and emit_host_pointer reads it from there.
 *
 * Returns the site to bind to the fault stub.
 */
X86pEmitSite emit_bounds_check(X86pEmit *e, const MemPlan *plan, int w) {
  const X86pHostReg offset = plan_offset_reg(plan);
  if (offset == ADDR_TMP) {
    x86p_emit_mov_r32_r32(e, ADDR_TMP, EA_REG);
    x86p_emit_alu_r32_imm32(e, kX64Sub, ADDR_TMP, plan->lo);
  }
  /*
   * A mapping narrower than the access has NO in-bounds address, so the check
   * becomes unconditional rather than arithmetic. Subtracting w from a smaller
   * size underflows to about four billion and would admit every address --
   * and w reaches 16 for an SSE access, so refusing to translate against a
   * mapping under one dword did not prevent it, it only moved it.
   */
  if (plan->size < (uint32_t)w) {
    return x86p_emit_jmp_rel32(e);
  }
  x86p_emit_alu_r32_imm32(e, kX64Cmp, offset, plan->size - (uint32_t)w);
  return x86p_emit_jcc_rel32(e, (unsigned)kX86pCondA);
}

/* HOSTPTR_REG = host + (EA - lo). plan_offset_reg already holds the offset,
   and writing a 32-bit register zero-extends, so the 64-bit add gets a clean
   offset. An identity mapping's offset IS the guest address, and the 32-bit
   move zero-extends it into the pointer without a base to add. */
void emit_host_pointer(X86pEmit *e, const MemPlan *plan) {
  if (plan_is_identity(plan)) {
    x86p_emit_mov_r32_r32(e, HOSTPTR_REG, EA_REG);
    return;
  }
  x86p_emit_mov_r64_imm64(e, HOSTPTR_REG, plan->host);
  x86p_emit_alu_r64_r64(e, kX64Add, HOSTPTR_REG, plan_offset_reg(plan));
}

void note_fault(BlockCtx *c, X86pEmitSite site, uint32_t eip) {
  if (c->nfaults < sizeof c->faults / sizeof c->faults[0]) {
    if (c->nfaults == 0u || c->fault_eips[c->nfaults - 1u] != eip) {
      c->fault_tail_bytes += FAULT_TRAMPOLINE_BYTES;
    }
    c->fault_eips[c->nfaults] = eip;
    c->faults[c->nfaults++] = site;
    return;
  }
  /* More fault sites than the block can hold. The site is real and now cannot
     be bound, so the buffer is poisoned and the block discarded -- never left
     with a jump to an arbitrary offset. */
  c->e->overflow = 1;
}

void note_divide_fault(BlockCtx *c, X86pEmitSite site) {
  if (c->ndivide_faults < sizeof c->divide_faults / sizeof c->divide_faults[0]) {
    c->divide_faults[c->ndivide_faults++] = site;
    return;
  }
  c->e->overflow = 1;
}

/* Leave HOSTPTR_REG pointing at the guest operand, or jump to the fault stub. */
/* The width is the ACCESS width, not a constant: a two-byte access ending one
   byte past the mapping must be refused, and a check hard-coded to 4 would
   refuse a legal one-byte access at the last address instead. */
void emit_mem_prepare_w(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w) {
  emit_effective_address(c, o);
  note_fault(c, emit_bounds_check(c->e, &c->plan, w), insn_eip);
  emit_host_pointer(c->e, &c->plan);
}
