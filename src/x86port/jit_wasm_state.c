/*
 * jit_wasm_state.c -- guest state and guest memory, as emitted WebAssembly
 * reaches them. See jit_wasm_state.h for the contract.
 */
#include "jit_wasm_state.h"

#include "cpu.h"

#include <stddef.h>
#include <string.h>

/*
 * ALIGNMENT HINTS ARE ALWAYS ZERO.
 *
 * The hint is log2 of the alignment the code PROMISES, and a hint larger than
 * the access width is a validation error rather than a slow path. Guest
 * accesses are freely unaligned -- x86 permits it and this guest does it -- so
 * a hint derived from the operand would have to be proven, and the proof would
 * have to hold for every base register value at run time. Zero promises
 * nothing, is valid for every access, and leaves the engine to discover the
 * alignment it actually sees.
 */
#define ALIGN_NONE 0u

static uint32_t reg_offset(int index) {
  return (uint32_t)(offsetof(X86pCpu, reg) + (size_t)index * sizeof(uint32_t));
}

/* Where a guest register operand of width `w` lives, as a byte offset. At
   width 1 the index names a byte register, and indices 4..7 are the SECOND
   byte of EAX..EBX rather than four further registers; x86p_byte_reg owns
   that. */
static uint32_t reg_offset_w(int index, int w) {
  if (w == 1) {
    int shift = 0;
    int r = x86p_byte_reg(index, &shift);
    return reg_offset(r) + (uint32_t)(shift / 8);
  }
  return reg_offset(index);
}

static uint32_t flag_offset(size_t field) {
  return (uint32_t)(offsetof(X86pCpu, flags) + field);
}

void x86p_wasm_state_init(X86pWasmState *s, X86pWasmEmit *e, const X86pWasmPlan *plan) {
  if (!s) {
    return;
  }
  memset(s, 0, sizeof *s);
  s->e = e;
  if (plan) {
    s->plan = *plan;
  }
}

void x86p_wasm_state_cpu(X86pWasmState *s) {
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalCpu);
}

void x86p_wasm_state_flags_addr(X86pWasmState *s) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_i32_const(s->e, (int32_t)flag_offset(0));
  x86p_wasm_i32_op(s->e, kWasmI32Add);
}

void x86p_wasm_state_x87_addr(X86pWasmState *s) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_i32_const(s->e, (int32_t)offsetof(X86pCpu, x87));
  x86p_wasm_i32_op(s->e, kWasmI32Add);
}

/* Load of width `w` from a cpu-relative field. Zero-extending, because the
   value is about to be used at that width and stale high bits would reach the
   flag tuple. */
static void load_w(X86pWasmState *s, uint32_t offset, int w) {
  if (w == 1) {
    x86p_wasm_i32_load8_u(s->e, ALIGN_NONE, offset);
  } else if (w == 2) {
    x86p_wasm_i32_load16_u(s->e, ALIGN_NONE, offset);
  } else {
    x86p_wasm_i32_load(s->e, ALIGN_NONE, offset);
  }
}

static void store_w(X86pWasmState *s, uint32_t offset, int w) {
  if (w == 1) {
    x86p_wasm_i32_store8(s->e, ALIGN_NONE, offset);
  } else if (w == 2) {
    x86p_wasm_i32_store16(s->e, ALIGN_NONE, offset);
  } else {
    x86p_wasm_i32_store(s->e, ALIGN_NONE, offset);
  }
}

void x86p_wasm_state_load_reg(X86pWasmState *s, int index, int w) {
  x86p_wasm_state_cpu(s);
  load_w(s, reg_offset_w(index, w), w);
}

void x86p_wasm_state_store_reg(X86pWasmState *s, int index, int w, X86pWasmLocal value) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)value);
  store_w(s, reg_offset_w(index, w), w);
}

static uint32_t xmm_offset(unsigned index, unsigned lane) {
  return (uint32_t)offsetof(X86pCpu, xmm) + index * 16u + lane * 4u;
}
void x86p_wasm_state_load_xmm_lane(X86pWasmState *s, unsigned index, unsigned lane) {
  x86p_wasm_state_cpu(s);
  load_w(s, xmm_offset(index, lane), 4);
}
void x86p_wasm_state_store_xmm_lane(X86pWasmState *s, unsigned index, unsigned lane, X86pWasmLocal value) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)value);
  store_w(s, xmm_offset(index, lane), 4);
}
void x86p_wasm_state_load_mxcsr(X86pWasmState *s) {
  x86p_wasm_state_cpu(s);
  load_w(s, (uint32_t)offsetof(X86pCpu, mxcsr), 4);
}
void x86p_wasm_state_store_mxcsr(X86pWasmState *s, X86pWasmLocal value) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)value);
  store_w(s, (uint32_t)offsetof(X86pCpu, mxcsr), 4);
}

void x86p_wasm_state_address_parts(X86pWasmState *s, const X86pOperand *o) {
  int pushed = 0;
  if (o->base >= 0) {
    x86p_wasm_state_load_reg(s, o->base, 4);
    pushed = 1;
  }
  if (o->index >= 0) {
    unsigned shift = (o->scale == 8) ? 3u : (o->scale == 4) ? 2u : (o->scale == 2) ? 1u : 0u;
    x86p_wasm_state_load_reg(s, o->index, 4);
    if (shift != 0u) {
      x86p_wasm_i32_const(s->e, (int32_t)shift);
      x86p_wasm_i32_op(s->e, kWasmI32Shl);
    }
    if (pushed) {
      x86p_wasm_i32_op(s->e, kWasmI32Add);
    }
    pushed = 1;
  }
  if (!pushed) {
    /* `[disp32]` has neither part. A zero here rather than folding the
       displacement in below, so the two cases share one path and the
       displacement is added the same way in both. */
    x86p_wasm_i32_const(s->e, 0);
  }
  if (o->disp != 0) {
    x86p_wasm_i32_const(s->e, o->disp);
    x86p_wasm_i32_op(s->e, kWasmI32Add);
  }
}

void x86p_wasm_state_address(X86pWasmState *s, const X86pOperand *o) {
  x86p_wasm_state_address_parts(s, o);
  /*
   * Only FS and GS have a base to add -- cpu.h states why the flat model is a
   * contract and not an approximation -- and which segment an operand uses was
   * resolved by the decoder, so the other four cost nothing at all here.
   */
  if (o->seg == (uint8_t)kX86pSegFs) {
    x86p_wasm_state_cpu(s);
    x86p_wasm_i32_load(s->e, ALIGN_NONE, (uint32_t)offsetof(X86pCpu, fs_base));
    x86p_wasm_i32_op(s->e, kWasmI32Add);
  } else if (o->seg == (uint8_t)kX86pSegGs) {
    x86p_wasm_state_cpu(s);
    x86p_wasm_i32_load(s->e, ALIGN_NONE, (uint32_t)offsetof(X86pCpu, gs_base));
    x86p_wasm_i32_op(s->e, kWasmI32Add);
  }
}

/*
 * Exact page permissions, inline, for a host with no VM to enforce them.
 *
 * kX86pWasmLocalAddr holds the OFFSET from `lo` by the time this runs, and the
 * bounds check above has already proved offset <= size - w, so neither page
 * index can leave the table and `offset + w - 1` cannot overflow.
 *
 * An access of width w can straddle two pages, so both are read. They are
 * ANDed rather than branched on separately: the question is whether EVERY byte
 * is permitted, one branch answers it, and a second branch would cost more
 * than the second load. The table's base rides in the load's own offset
 * immediate, so the whole check is a shift and a load per page.
 */
static void x86p_wasm_state_guard_perms(X86pWasmState *s, uint32_t insn_eip, int w, unsigned access) {
  if (!s->plan.perms) {
    return;
  }
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_i32_const(s->e, (int32_t)s->plan.page_shift);
  x86p_wasm_i32_op(s->e, kWasmI32ShrU);
  x86p_wasm_i32_load8_u(s->e, 0u, s->plan.perms);
  if (w > 1) {
    x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
    x86p_wasm_i32_const(s->e, w - 1);
    x86p_wasm_i32_op(s->e, kWasmI32Add);
    x86p_wasm_i32_const(s->e, (int32_t)s->plan.page_shift);
    x86p_wasm_i32_op(s->e, kWasmI32ShrU);
    x86p_wasm_i32_load8_u(s->e, 0u, s->plan.perms);
    x86p_wasm_i32_op(s->e, kWasmI32And);
  }
  x86p_wasm_i32_const(s->e, (int32_t)access);
  x86p_wasm_i32_op(s->e, kWasmI32And);
  x86p_wasm_i32_const(s->e, (int32_t)access);
  x86p_wasm_i32_op(s->e, kWasmI32Ne);
  x86p_wasm_if(s->e, kWasmVoid);
  x86p_wasm_state_exit_imm(s, insn_eip, kX86pJitExitMemoryFault);
  x86p_wasm_end(s->e);
}

void x86p_wasm_state_guard_addr(X86pWasmState *s, uint32_t insn_eip, int w, unsigned access) {
  if (s->plan.memory_context) {
    x86p_wasm_i32_const(s->e, (int32_t)s->plan.memory_context);
    x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
    x86p_wasm_i32_const(s->e, w);
    x86p_wasm_i32_const(s->e, (int32_t)access);
    x86p_wasm_call(s->e, (uint32_t)kX86pWasmImportMemOk);
    x86p_wasm_i32_op(s->e, kWasmI32Eqz);
    x86p_wasm_if(s->e, kWasmVoid);
    x86p_wasm_state_exit_imm(s, insn_eip, kX86pJitExitMemoryFault);
    x86p_wasm_end(s->e);
    return;
  }

  /*
   * A mapping narrower than the access has NO in-bounds address, so the
   * refusal is unconditional rather than arithmetic. Subtracting w from a
   * smaller size underflows to about four billion and would admit every
   * address.
   */
  if (s->plan.size < (uint32_t)w) {
    x86p_wasm_state_exit_imm(s, insn_eip, kX86pJitExitMemoryFault);
    return;
  }
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
  if (s->plan.lo != 0u) {
    x86p_wasm_i32_const(s->e, (int32_t)s->plan.lo);
    x86p_wasm_i32_op(s->e, kWasmI32Sub);
  }
  /* The offset from `lo` replaces the guest address in the local: everything
     after the check wants the offset, and recomputing it would read base
     registers the instruction may be about to modify. */
  x86p_wasm_local_tee(s->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_i32_const(s->e, (int32_t)(s->plan.size - (uint32_t)w));
  x86p_wasm_i32_op(s->e, kWasmI32GtU);
  x86p_wasm_if(s->e, kWasmVoid);
  x86p_wasm_state_exit_imm(s, insn_eip, kX86pJitExitMemoryFault);
  x86p_wasm_end(s->e);

  x86p_wasm_state_guard_perms(s, insn_eip, w, access);

  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_i32_const(s->e, (int32_t)s->plan.base);
  x86p_wasm_i32_op(s->e, kWasmI32Add);
  x86p_wasm_local_set(s->e, (uint32_t)kX86pWasmLocalAddr);
}

void x86p_wasm_state_guard(X86pWasmState *s, const X86pOperand *o, uint32_t insn_eip, int w, unsigned access) {
  x86p_wasm_state_address(s, o);
  x86p_wasm_local_set(s->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_state_guard_addr(s, insn_eip, w, access);
}

void x86p_wasm_state_load_mem(X86pWasmState *s, int w) {
  if (s->plan.memory_context) {
    x86p_wasm_i32_const(s->e, (int32_t)s->plan.memory_context);
    x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
    x86p_wasm_i32_const(s->e, w);
    x86p_wasm_call(s->e, (uint32_t)kX86pWasmImportMemLoad);
    return;
  }

  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
  load_w(s, 0u, w);
}

void x86p_wasm_state_store_mem(X86pWasmState *s, int w, X86pWasmLocal value) {
  if (s->plan.memory_context) {
    x86p_wasm_i32_const(s->e, (int32_t)s->plan.memory_context);
    x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
    x86p_wasm_i32_const(s->e, w);
    x86p_wasm_local_get(s->e, (uint32_t)value);
    x86p_wasm_call(s->e, (uint32_t)kX86pWasmImportMemStore);
    return;
  }

  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_local_get(s->e, (uint32_t)value);
  store_w(s, 0u, w);
}

void x86p_wasm_state_store_flags(X86pWasmState *s, X86pFlagKind kind, int w) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_i32_store(s->e, ALIGN_NONE, flag_offset(offsetof(X86pFlags, a)));
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalB);
  x86p_wasm_i32_store(s->e, ALIGN_NONE, flag_offset(offsetof(X86pFlags, b)));
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_i32_store(s->e, ALIGN_NONE, flag_offset(offsetof(X86pFlags, r)));
  x86p_wasm_state_cpu(s);
  x86p_wasm_i32_const(s->e, (int32_t)kind);
  x86p_wasm_i32_store8(s->e, ALIGN_NONE, flag_offset(offsetof(X86pFlags, kind)));
  x86p_wasm_state_cpu(s);
  x86p_wasm_i32_const(s->e, w);
  x86p_wasm_i32_store8(s->e, ALIGN_NONE, flag_offset(offsetof(X86pFlags, w)));
}

void x86p_wasm_state_store_carry(X86pWasmState *s) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)kX86pWasmLocalCarry);
  x86p_wasm_i32_store8(s->e, ALIGN_NONE, flag_offset(offsetof(X86pFlags, carry_in)));
}

void x86p_wasm_state_store_df(X86pWasmState *s, int value) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_i32_const(s->e, value ? 1 : 0);
  x86p_wasm_i32_store8(s->e, ALIGN_NONE, (uint32_t)offsetof(X86pCpu, df));
}

void x86p_wasm_state_exit_imm(X86pWasmState *s, uint32_t eip, X86pJitExit exit) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_i32_const(s->e, (int32_t)eip);
  x86p_wasm_i32_store(s->e, ALIGN_NONE, (uint32_t)offsetof(X86pCpu, eip));
  x86p_wasm_i32_const(s->e, (int32_t)exit);
  x86p_wasm_return(s->e);
}

void x86p_wasm_state_exit_local(X86pWasmState *s, X86pWasmLocal eip, X86pJitExit exit) {
  x86p_wasm_state_cpu(s);
  x86p_wasm_local_get(s->e, (uint32_t)eip);
  x86p_wasm_i32_store(s->e, ALIGN_NONE, (uint32_t)offsetof(X86pCpu, eip));
  x86p_wasm_i32_const(s->e, (int32_t)exit);
  x86p_wasm_return(s->e);
}
