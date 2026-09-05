/*
 * jit_arm64_x87.c -- native x87 emission for the AArch64 JIT backend.
 *
 * See jit_arm64_x87.h for the scope. jit_x87_predicates.c owns the common
 * admission checks and refuses value-bearing lowering when host long double
 * cannot represent x87 extended state exactly. The value emitters below are
 * therefore unavailable on macOS binary64 and Linux/Android binary128 hosts
 * until the semantic state owner becomes host-independent f80.
 * Every emitted sequence calls the same x86p_x87_* helper the interpreter
 * calls, so the reverse flag, the ZE/IE/SF status bits, the divide-by-zero
 * infinity, the result tag and TOP stay that one module's business -- see
 * x87.h.
 *
 * WHY THIS FILE HAS NO EQUIVALENT OF x64'S x87_widen_mem_to_scratch. The x64
 * backend widens a 32/64-bit guest float to 80 bits with the HOST's own x87
 * unit (a real `fld`), because x86-64's `long double` is that exact 80-bit
 * extended format -- free and exact. AArch64 has no x87 unit, and on the
 * target this backend actually builds for (Apple Silicon / Darwin), the
 * compiler's own `long double` is not a 128-bit quad either -- it IS
 * `double`, 8 bytes (verified: `sizeof(long double) == 8` on this host,
 * unlike Linux AArch64's 128-bit quad that an earlier comment in
 * emit_arm64.h assumed). So there is no widening trick to reproduce: the bit
 * pattern already needed converting to `long double` in C for the
 * interpreter to share this module at all, and x87.h already exposes that
 * conversion as a plain function -- x86p_x87_from_f32 / x86p_x87_from_f64.
 * This file calls it and receives the result already sitting in D0, exactly
 * where the next helper call's `long double` argument belongs under AAPCS64
 * -- no memory round-trip, no widen-and-spill, no scratch slot at all for
 * that path.
 *
 * A scratch slot IS still needed wherever a helper hands a `long double`
 * BACK by pointer (x86p_x87_get, x86p_x87_pop): AAPCS64 has no way to return
 * a struct-sized value in a register pair the caller can address as `[sp]`
 * without one. The slot is 16, not 8, bytes so `x86p_a64_emit_load_q` can
 * read it back into V0 as a single aligned load -- the upper 64 bits it also
 * reads are never-written stack garbage that AAPCS64 requires the callee to
 * ignore when it only reads D0 (the low 64 bits of V0), so this is safe
 * despite `long double` here being half that width. `sub sp,#16` / `add
 * sp,#16` keeps SP 16-byte aligned, which every AArch64 load/store through it
 * requires.
 *
 * ORDERING VERSUS emit_mem_prepare_w: identical constraint to jit_x64_x87.c.
 * A bounds check must never run while a scratch slot is open, because its
 * fault stub assumes SP is at the post-prologue depth. Every call site below
 * either has no memory operand while the slot is open, or closes the slot
 * first (emit_x87_store_mem).
 */
#include "jit_arm64_x87.h"

#include "cpu.h"
#include "emit_arm64.h"
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

/* Read the raw bits at a bounds-checked guest float operand into W0/X0. */
static void x87_load_bits(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w) {
  emit_mem_prepare_w(c, o, insn_eip, w);
  if (w == 4) {
    x86p_a64_emit_load32(c->e, kA64X0, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load64(c->e, kA64X0, HOSTPTR_REG, 0);
  }
}

/* Bounds-check a guest float destination and store the raw bits already
   sitting in `src`. */
static void x87_store_bits(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w, X86pA64Reg src) {
  emit_mem_prepare_w(c, o, insn_eip, w);
  if (w == 4) {
    x86p_a64_emit_store32(c->e, HOSTPTR_REG, 0, src);
  } else {
    x86p_a64_emit_store64(c->e, HOSTPTR_REG, 0, src);
  }
}

/* x86p_x87_from_f32(w0) / x86p_x87_from_f64(x0) -- the exact bit-pattern to
   `long double` conversion x87.h already owns; see the file comment for why
   this replaces x64's hardware `fld` widen. Leaves the result in D0. */
static void x87_from_bits(X86pA64Emit *e, int w) {
  x87_call(e, (w == 4) ? (const void *)&x86p_x87_from_f32 : (const void *)&x86p_x87_from_f64);
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
 * FLD -- push a float onto the x87 stack. See x87.h and jit_x64_x87.c for why
 * x86p_x87_push owns overflow/tag/TOP, and why FLD ST(i) reads with
 * x86p_x87_get before pushing (a push would renumber the register being
 * read) and pushes nothing when that register was empty. */
void emit_x87_load(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o = &insn->operand[0];

  if (o->kind == kX86pOperandMem) {
    const int w = o->size; /* 4 or 8 -- can_emit gate */
    x87_load_bits(c, o, insn_eip, w);
    x87_from_bits(e, w); /* D0 = the widened value */
    x87_lea_self(e);     /* X0 = &cpu->x87; D0 survives (GP-only in between) */
    x87_call(e, (const void *)&x86p_x87_push);
    return;
  }

  /* FLD ST(i): x86p_x87_get(&cpu->x87, i, &slot); push only when it succeeded. */
  x86p_a64_emit_sub_sp_imm(e, 16u);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)o->reg);
  x86p_a64_emit_lea64(e, kA64X2, kA64Sp, 0);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
  {
    X86pA64EmitSite skip = x86p_a64_emit_bcc(e, kA64CondEq); /* source register was empty */
    x86p_a64_emit_load_q(e, 0u, kA64Sp, 0);                  /* D0 = the slot's value */
    x87_lea_self(e);
    x87_call(e, (const void *)&x86p_x87_push);
    x86p_a64_emit_bind(e, skip);
  }
  x86p_a64_emit_add_sp_imm(e, 16u);
}

/*
 * FADD / FSUB / FMUL / FDIV (+R, +P). The source is converted to `long
 * double` in D0 -- from memory bits via x86p_x87_from_f32/f64, or from a
 * stack register via x86p_x87_get -- and x86p_x87_arith runs the real op
 * under the guest control word. A named source register that is empty is a
 * stack fault arith_operands turns into a whole no-op there too: this jumps
 * straight past the arith call and the pops.
 */
void emit_x87_arith(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  const int two_op = (insn->operands == 2);
  const int dst = two_op ? o0->reg : 0; /* mem and short reg form accumulate into ST(0) */
  X86pA64EmitSite skip;
  int have_skip = 0;
  int i;

  if (o0->kind == kX86pOperandMem) {
    x87_load_bits(c, o0, insn_eip, o0->size);
    x87_from_bits(e, o0->size); /* D0 = src */
  } else {
    x86p_a64_emit_sub_sp_imm(e, 16u);
    x87_lea_self(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)(two_op ? insn->operand[1].reg : o0->reg));
    x86p_a64_emit_lea64(e, kA64X2, kA64Sp, 0);
    x87_call(e, (const void *)&x86p_x87_get);
    x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
    skip = x86p_a64_emit_bcc(e, kA64CondEq); /* empty source register */
    have_skip = 1;
    x86p_a64_emit_load_q(e, 0u, kA64Sp, 0); /* D0 = src */
  }

  /* x86p_x87_arith(f, op, dst, src=D0, reverse): GP args and the one FP arg
     are allocated from independent register files under AAPCS64, so setting
     X0..X3 here does not disturb D0. */
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)insn->x87_op);
  x86p_a64_emit_mov_w_imm32(e, kA64X2, (uint32_t)dst);
  x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)insn->x87_reverse);
  x87_call(e, (const void *)&x86p_x87_arith);

  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }

  if (have_skip) {
    x86p_a64_emit_bind(e, skip);
    x86p_a64_emit_add_sp_imm(e, 16u); /* balances the sub in the register-source branch, on BOTH paths */
  }
}

/*
 * FCOM / FCOMP m32/m64. x86p_x87_compare remains the sole owner of C0/C2/C3,
 * NaN and empty-ST(0) status semantics; FCOMP then pops through the same
 * stack owner as the interpreter. Neither changes integer EFLAGS.
 */
void emit_x87_compare_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  int i;

  x87_load_bits(c, o0, insn_eip, o0->size);
  x87_from_bits(e, o0->size); /* D0 = other */
  x87_lea_self(e);
  x87_call(e, (const void *)&x86p_x87_compare);
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }
}

/*
 * FST ST(i) / FSTP ST(i): read ST(0), copy it into ST(i), pop when the P
 * form. Both slots are the same width so nothing rounds. An empty ST(0) is a
 * stack fault the interpreter turns into a no-op.
 */
void emit_x87_store_reg(BlockCtx *c, const X86pInsn *insn) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  X86pA64EmitSite skip;
  int i;

  x86p_a64_emit_sub_sp_imm(e, 16u);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
  x86p_a64_emit_lea64(e, kA64X2, kA64Sp, 0);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
  skip = x86p_a64_emit_bcc(e, kA64CondEq); /* ST(0) empty */

  x86p_a64_emit_load_q(e, 0u, kA64Sp, 0); /* D0 = value */
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, (uint32_t)o0->reg);
  x87_call(e, (const void *)&x86p_x87_set);
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }
  x86p_a64_emit_bind(e, skip);
  x86p_a64_emit_add_sp_imm(e, 16u);
}

/*
 * FST m32/m64 / FSTP m32/m64.
 *
 * ORDER MATTERS, identically to jit_x64_x87.c: the interpreter reads ST(0)
 * and, only if it is not empty, narrows and writes memory, so an empty
 * ST(0) never faults on a bad address. This emits x86p_x87_get, then --
 * if it succeeded -- x86p_x87_to_f32/f64 (which rounds by the guest control
 * word), THEN closes the scratch slot, THEN the bounds check, so the shared
 * fault stub always observes the post-prologue SP. The narrowed bits survive
 * the slot's closing and the address computation in CARRY_REG: x87 emission
 * never touches it (that role only matters to integer ALU carry-in, and x87
 * never runs alongside one), so it is the one role register the address
 * path (EA_REG/HOSTPTR_REG/ADDR_TMP/FAULTPC_REG) cannot disturb, unlike X8/X9
 * which the encoder's own large-immediate fallbacks may use internally
 * during that same call.
 */
void emit_x87_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64Emit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  const int w = o0->size; /* 4 or 8 -- gate */
  X86pA64EmitSite empty;
  X86pA64EmitSite done;
  int i;

  x86p_a64_emit_sub_sp_imm(e, 16u);
  x87_lea_self(e);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
  x86p_a64_emit_lea64(e, kA64X2, kA64Sp, 0);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
  empty = x86p_a64_emit_bcc(e, kA64CondEq); /* ST(0) empty -> no store, no pop, no fault */

  x86p_a64_emit_load_q(e, 0u, kA64Sp, 0); /* D0 = value */
  x87_lea_self(e);
  x87_call(e, (w == 4) ? (const void *)&x86p_x87_to_f32 : (const void *)&x86p_x87_to_f64);
  x86p_a64_emit_mov_x_x(e, CARRY_REG, kA64X0); /* narrowed bits (all 64), parked past sp restore */
  x86p_a64_emit_add_sp_imm(e, 16u);

  x87_store_bits(c, o0, insn_eip, w, CARRY_REG);
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_a64_emit_mov_w_imm32(e, kA64X1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }
  done = x86p_a64_emit_b(e);

  x86p_a64_emit_bind(e, empty);
  x86p_a64_emit_add_sp_imm(e, 16u);
  x86p_a64_emit_bind(e, done);
}
