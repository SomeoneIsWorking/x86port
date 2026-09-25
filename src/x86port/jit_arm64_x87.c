/*
 * jit_arm64_x87.c -- native x87 emission for the AArch64 JIT backend.
 *
 * Loads, stores, arithmetic, compares and copies call jit_x87_helpers.h, which
 * stays in the register file's storage type from guest memory to guest
 * memory. The `long double` forms in x87.h are software binary128 on Linux and
 * Android AArch64, and calling through them widened and re-encoded every value
 * on every instruction: on an Adreno 722 phone that round trip was about 40%
 * of the game thread. Constants, the status word, FNCLEX and the x87_fn table
 * need no value crossing and keep their own owners.
 *
 * Operand bits reach the helpers as two 32-bit halves (lo in X1, hi in X2),
 * the same ABI the WebAssembly backend calls them with.
 *
 * ORDERING VERSUS emit_mem_prepare_w: identical constraint to jit_x64_x87.c.
 * A bounds check must never run while a scratch slot is open, because its
 * fault stub assumes SP is at the post-prologue depth. emit_x87_store_mem
 * closes its slot before the check.
 */
#include "jit_arm64_x87.h"
#include "jit_arm64_x87_inline.h"

#include "cpu.h"
#include "emit_arm64.h"
#include "jit_x87_helpers.h"
#include "x87.h"

#include <stddef.h>
#include <stdint.h>

/* The X86pX87 sub-struct, and where it lives in X86pCpu. */
static int32_t x87_off(void) {
  return (int32_t)offsetof(X86pCpu, x87);
}

/* ---- shared emission helpers ---------------------------------------------- */

/* x0 = &cpu->x87 -- the first argument to every x86p_x87_* helper. */
static void x87_lea_self(X86pA64Emit *e) {
  x86p_a64_emit_lea64(e, kA64X0, CPU_REG, x87_off());
}

/* mov x9, imm64(fn); blr x9 -- X9 is CALL_TARGET in jit_arm64.c's convention
   (jit_arm64_internal.h: encoder-internal scratch, never a role register,
   never live across more than this one call). This file has its own copy
   because it is a separate translation unit, exactly as jit_x64_x87.c keeps
   its own local x87_call rather than sharing jit_x64.c's. */
static void x87_call(X86pA64Emit *e, const void *fn) {
  x86p_a64_emit_mov_x_imm64(e, kA64X9, (uint64_t)(uintptr_t)fn);
  x86p_a64_emit_blr(e, kA64X9);
}

/* Read the raw bits at a bounds-checked guest float operand into
   X87_BITS_REG, where both an inline fast path and the helper arguments read
   them. */
static void x87_load_bits(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w) {
  emit_mem_prepare_w(c, o, insn_eip, w);
  if (w == 2) {
    x86p_a64_emit_load16_zx(c->e, X87_BITS_REG, HOSTPTR_REG, 0);
  } else if (w == 4) {
    x86p_a64_emit_load32(c->e, X87_BITS_REG, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load64(c->e, X87_BITS_REG, HOSTPTR_REG, 0);
  }
}

/* Bounds-check a guest float destination and store the raw bits already
   sitting in `src`. */
static void x87_store_bits(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w, X86pA64Reg src) {
  emit_mem_prepare_w(c, o, insn_eip, w);
  if (w == 2) {
    x86p_a64_emit_store16_reg(c->e, HOSTPTR_REG, 0, src);
  } else if (w == 4) {
    x86p_a64_emit_store32(c->e, HOSTPTR_REG, 0, src);
  } else {
    x86p_a64_emit_store64(c->e, HOSTPTR_REG, 0, src);
  }
}

/* The raw operand bits the preceding x87_load_bits left in X87_BITS_REG, as
   the helpers' (f, lo, hi) prefix: X1 = low half, X2 = high half, X0 = f. */
static void x87_bits_args(X86pA64Emit *e) {
  x86p_a64_emit_mov_x_x(e, kA64X1, X87_BITS_REG);
  x86p_a64_emit_lsr_x_imm(e, kA64X2, X87_BITS_REG, 32u);
  x87_lea_self(e);
}

void emit_x87_constant(BlockCtx *c, const X86pInsn *insn) {
  X86pA64Emit *e = c->e;
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)insn->x87);
  x87_call(e, (const void *)&x86p_x87_push_constant);
}

/* FNSTSW AX. x86p_x87_status is the one owner that replaces any stale TOP
   bits in the stored status field with the live stack pointer. A 16-bit
   store into the guest EAX slot preserves its upper half exactly like
   x86p_reg_write(..., width=2), and none of this touches the separate
   integer EFLAGS model. */
void emit_x87_status_ax(BlockCtx *c) {
  X86pA64Emit *e = c->e;
  x87_lea_self(e);
  x87_call(e, (const void *)&x86p_x87_status);
  x86p_a64_emit_store16_reg(e, CPU_REG, (int32_t)offsetof(X86pCpu, reg[kX86pEax]), kA64X0);
}

/* FNCLEX changes no integer flags or registers. The shared semantic owner
   clears precisely the architectural exception/busy mask in the guest x87
   status field and leaves every other x87 field untouched. */
void emit_x87_clear_exceptions(BlockCtx *c) {
  x87_lea_self(c->e);
  x87_call(c->e, (const void *)&x86p_x87_clear_exceptions);
}

/* ---- emission --------------------------------------------------------------
 * FLD -- push a float onto the x87 stack. The helpers own overflow, tags and
 * TOP. FLD ST(i) reads the register before pushing (a push would renumber
 * it) and pushes nothing when that register was empty. */
void emit_x87_load(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o = &insn->operand[0];
  X87Slow slow = {0};

  if (o->kind == kX86pOperandMem) {
    const int w = o->size; /* 2, 4 or 8 -- can_emit gate */
    x87_load_bits(c, o, insn_eip, w);
    emit_x87_inline_load(c, insn, &slow);
    x87_slow_begin(c, &slow);
    x87_bits_args(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)w);
    x86p_a64_emit_mov_w_imm32(e, kA64X4, insn->x87 == kX86pX87InsnLoadInt ? 1u : 0u);
    x86p_a64_emit_mov_w_imm32(e, kA64X5, 0u);
    x87_call(e, (const void *)&x86p_jit_x87_load_bits);
    x87_slow_end(c, &slow);
    return;
  }

  emit_x87_inline_copy(c, insn, &slow);
  x87_slow_begin(c, &slow);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)o->reg);
  x86p_a64_emit_mov_w_imm32(e, kA64X2, 0u);
  x86p_a64_emit_mov_w_imm32(e, kA64X3, 1u); /* push */
  x86p_a64_emit_mov_w_imm32(e, kA64X4, 0u);
  x87_call(e, (const void *)&x86p_jit_x87_copy);
  x87_slow_end(c, &slow);
}

/*
 * FADD / FSUB / FMUL / FDIV (+R, +P). A memory source accumulates into ST(0);
 * a register form names both. A named source register that is empty is a
 * stack fault the helper turns into a whole no-op, pops included.
 */
void emit_x87_arith(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  const int two_op = (insn->operands == 2);
  X87Slow slow = {0};

  if (o0->kind == kX86pOperandMem) {
    x87_load_bits(c, o0, insn_eip, o0->size);
    emit_x87_inline_arith(c, insn, &slow);
    x87_slow_begin(c, &slow);
    x87_bits_args(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)o0->size);
    x86p_a64_emit_mov_w_imm32(e, kA64X4, (uint32_t)insn->x87_mem_int);
    x86p_a64_emit_mov_w_imm32(e, kA64X5, (uint32_t)insn->x87_op);
    x86p_a64_emit_mov_w_imm32(e, kA64X6, (uint32_t)insn->x87_reverse);
    x86p_a64_emit_mov_w_imm32(e, kA64X7, (uint32_t)insn->x87_pops);
    x87_call(e, (const void *)&x86p_jit_x87_arith_mem_bits);
    x87_slow_end(c, &slow);
    return;
  }
  emit_x87_inline_arith(c, insn, &slow);
  x87_slow_begin(c, &slow);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)(two_op ? o0->reg : 0));
  x86p_a64_emit_mov_w_imm32(e, kA64X2, (uint32_t)(two_op ? insn->operand[1].reg : o0->reg));
  x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)insn->x87_op);
  x86p_a64_emit_mov_w_imm32(e, kA64X4, (uint32_t)insn->x87_reverse);
  x86p_a64_emit_mov_w_imm32(e, kA64X5, (uint32_t)insn->x87_pops);
  x87_call(e, (const void *)&x86p_jit_x87_arith_reg);
  x87_slow_end(c, &slow);
}

/* FCOM / FCOMP m32/m64. x86p_x87_compare, behind the helper, remains the sole
   owner of C0/C2/C3 and the NaN and empty-ST(0) status. Neither changes
   integer EFLAGS. */
void emit_x87_compare_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  X87Slow slow = {0};

  x87_load_bits(c, o0, insn_eip, o0->size);
  emit_x87_inline_compare_mem(c, insn, &slow);
  x87_slow_begin(c, &slow);
  x87_bits_args(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)o0->size);
  x86p_a64_emit_mov_w_imm32(e, kA64X4, (uint32_t)insn->x87_mem_int);
  x86p_a64_emit_mov_w_imm32(e, kA64X5, (uint32_t)insn->x87_pops);
  x87_call(e, (const void *)&x86p_jit_x87_compare_mem_bits);
  x87_slow_end(c, &slow);
}

/* FST ST(i) / FSTP ST(i): ST(0) into ST(i), then the pops. Both slots are the
   same width so nothing rounds; an empty ST(0) is a no-op. */
void emit_x87_store_reg(BlockCtx *c, const X86pInsn *insn) {
  X86pA64Emit *e = c->e;
  X87Slow slow = {0};
  emit_x87_inline_copy(c, insn, &slow);
  x87_slow_begin(c, &slow);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
  x86p_a64_emit_mov_w_imm32(e, kA64X2, (uint32_t)insn->operand[0].reg);
  x86p_a64_emit_mov_w_imm32(e, kA64X3, 0u);
  x86p_a64_emit_mov_w_imm32(e, kA64X4, (uint32_t)insn->x87_pops);
  x87_call(e, (const void *)&x86p_jit_x87_copy);
  x87_slow_end(c, &slow);
}

/*
 * FST/FSTP m32/m64 and FIST/FISTP m16/m32/m64.
 *
 * ORDER MATTERS, identically to the interpreter: ST(0) is converted -- which
 * raises the conversion's status flags -- and only if it was not empty is the
 * address checked and memory written, so an empty ST(0) never faults on a bad
 * address. The converted bytes land in a scratch slot and move to CARRY_REG
 * before the slot closes: x87 emission never touches that role, so it is the
 * one role register the address path (EA_REG/HOSTPTR_REG/ADDR_TMP/FAULTPC_REG)
 * cannot disturb, unlike X8/X9 which the encoder's large-immediate fallbacks
 * may use during that same call. Bytes past `w` in the slot are stale and
 * never stored.
 *
 * An inline fast path (jit_arm64_x87_inline.h) that answers leaves its bits in
 * CARRY_REG too and joins at the address check, so both share its one fault
 * site and the pops.
 */
void emit_x87_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  const int w = o0->size; /* 2, 4 or 8 -- gate */
  X86pA64EmitSite skip;
  X86pA64EmitSite done;
  X87Slow slow = {0};

  emit_x87_inline_store(c, insn, &slow);
  x87_slow_begin(c, &slow);
  x86p_a64_emit_sub_sp_imm(e, 16u);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)w);
  x86p_a64_emit_mov_w_imm32(e, kA64X2, insn->x87 == kX86pX87InsnStoreInt ? 1u : 0u);
  x86p_a64_emit_lea64(e, kA64X3, kA64Sp, 0);
  x87_call(e, (const void *)&x86p_jit_x87_store_bytes);
  x86p_a64_emit_cmp_w_imm(e, kA64X0, 1u);
  skip = x86p_a64_emit_bcc(e, kA64CondNe); /* ST(0) empty: no store, no pop, no fault */

  x86p_a64_emit_load64(e, CARRY_REG, kA64Sp, 0);
  x86p_a64_emit_add_sp_imm(e, 16u);
  x87_slow_end(c, &slow);
  x87_store_bits(c, o0, insn_eip, w, CARRY_REG);
  /* ST(0) was occupied on both paths to here. */
  emit_x87_pops(c, insn->x87_pops);
  done = x86p_a64_emit_b(e);

  x86p_a64_emit_bind(e, skip);
  x86p_a64_emit_add_sp_imm(e, 16u);
  x86p_a64_emit_bind(e, done);
}

void emit_x87_control(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const int32_t offset = (int32_t)(offsetof(X86pCpu, x87) + offsetof(X86pX87, control));
  emit_mem_prepare_w(c, &insn->operand[0], insn_eip, 2);
  if (insn->x87 == kX86pX87InsnLoadControl) {
    x86p_a64_emit_load16_zx(c->e, kA64X0, HOSTPTR_REG, 0);
    x86p_a64_emit_store16_reg(c->e, CPU_REG, offset, kA64X0);
  } else {
    x86p_a64_emit_load16_zx(c->e, kA64X0, CPU_REG, offset);
    x86p_a64_emit_store16_reg(c->e, HOSTPTR_REG, 0, kA64X0);
  }
}

void emit_x87_fn(BlockCtx *c, const X86pInsn *insn) {
  x87_lea_self(c->e);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, insn->x87_fn);
  x87_call(c->e, (const void *)&x86p_x87_apply_fn);
}
