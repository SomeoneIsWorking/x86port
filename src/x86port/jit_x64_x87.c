/*
 * jit_x64_x87.c -- native x87 emission for the x86-64 JIT backend.
 *
 * See jit_x64_x87.h for the scope. Every emitted sequence widens its operands
 * inline and then calls the same x86p_x87_* helper the interpreter calls, so
 * the reverse flag, the ZE/IE/SF status bits, the divide-by-zero infinity, the
 * result tag and TOP stay that one module's business -- jit.verify compares the
 * full x87 state on every block end and would fail on the first divergence.
 */
#include "jit_x64_x87.h"

#include "cpu.h"
#include "emit_x64.h"
#include "x87.h"

#include <stddef.h>
#include <stdint.h>

/* The X86pX87 sub-struct, and where it lives in X86pCpu. */
static int32_t x87_off(void) {
  return (int32_t)offsetof(X86pCpu, x87);
}

/* ---- predicates -------------------------------------------------------- */

/*
 * FLD. A load is the one x87 instruction with no rounding and no
 * reverse-operand trap: widening a 32- or 64-bit float to the register's 80
 * bits is exact for every value, NaNs and denormals included, so `fld
 * dword`/`fld qword` on the host produces bit-for-bit what x86p_x87_from_f32/f64
 * produce. FLD m80 stays on the helper (its own byte-assembly path), as does
 * FILD (integer source, x87_mem_int).
 */
int x87_load_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o;
  if (insn->x87 != (uint8_t)kX86pX87InsnLoad || insn->x87_mem_int || insn->operands != 1) {
    return 0;
  }
  o = &insn->operand[0];
  if (o->kind == kX86pOperandSt) {
    return o->reg >= 0 && o->reg < X86P_X87_REGS;
  }
  return o->kind == kX86pOperandMem && (o->size == 4 || o->size == 8) && !o->addr16;
}

/*
 * FADD / FSUB / FMUL / FDIV, their R (operand-swap) and P (pop) variants, in
 * the three operand shapes arith_operands (x87_exec.c) recognises: `Fop m32/m64`
 * accumulating into ST(0), `Fop ST(0), ST(i)` written short as one operand, and
 * `FopP ST(i), ST(0)` with two. The integer-source forms (FIADD ...,
 * x87_mem_int) keep their own path.
 */
int x87_arith_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  if (insn->x87 != (uint8_t)kX86pX87InsnArith || insn->x87_mem_int) {
    return 0;
  }
  if ((unsigned)insn->x87_op >= (unsigned)kX86pX87OpCount) {
    return 0;
  }
  if (insn->operands == 2 && o0->kind == kX86pOperandSt && insn->operand[1].kind == kX86pOperandSt) {
    return o0->reg >= 0 && o0->reg < X86P_X87_REGS && insn->operand[1].reg >= 0 && insn->operand[1].reg < X86P_X87_REGS;
  }
  if (insn->operands == 1 && o0->kind == kX86pOperandSt) {
    return o0->reg >= 0 && o0->reg < X86P_X87_REGS;
  }
  return insn->operands == 1 && o0->kind == kX86pOperandMem && (o0->size == 4 || o0->size == 8) && !o0->addr16;
}

/*
 * FST / FSTP to a stack position (not memory). Both slots are 80-bit, so there
 * is no narrowing and no control-word question.
 */
int x87_store_reg_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  return insn->x87 == (uint8_t)kX86pX87InsnStore && insn->operands == 1 && o0->kind == kX86pOperandSt && o0->reg >= 0 &&
         o0->reg < X86P_X87_REGS;
}

/*
 * FST / FSTP to a 32- or 64-bit memory float. x86p_x87_to_f32/f64 own the
 * narrowing (now RC-correct), so the emitted code calls them -- and calls
 * x86p_x87_get first, because the interpreter tests ST(0) for emptiness BEFORE
 * it touches memory: an empty ST(0) is a whole no-op there, no store and no
 * fault even if the address is bad. So the get and the narrowing run first, and
 * only then the bounds check.
 */
int x87_store_mem_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  return insn->x87 == (uint8_t)kX86pX87InsnStore && insn->operands == 1 && o0->kind == kX86pOperandMem &&
         (o0->size == 4 || o0->size == 8) && !o0->addr16;
}

int x87_constant_is_emittable(const X86pInsn *insn) {
  if (insn->operands != 0) {
    return 0;
  }
  switch ((X86pX87Insn)insn->x87) {
  case kX86pX87InsnConstZero:
  case kX86pX87InsnConstOne:
  case kX86pX87InsnConstPi:
  case kX86pX87InsnConstLog2E:
  case kX86pX87InsnConstLog2T:
  case kX86pX87InsnConstLn2:
  case kX86pX87InsnConstLog102:
    return 1;
  default:
    return 0;
  }
}

/* ---- shared emission helpers ----------------------------------------- */

/* lea rdi, [&cpu->x87] -- the first argument to every x86p_x87_* helper. */
static void x87_lea_self(X86pEmit *e) {
  x86p_emit_lea64(e, kX64Rdi, CPU_REG, x87_off());
}

/* mov rax, imm64(fn); call rax. */
static void x87_call(X86pEmit *e, const void *fn) {
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)fn);
  x86p_emit_call_r64(e, kX64Rax);
}

void emit_x87_constant(BlockCtx *c, const X86pInsn *insn) {
  x87_lea_self(c->e);
  x86p_emit_mov_r32_imm32(c->e, kX64Rsi, insn->x87);
  x87_call(c->e, (const void *)&x86p_x87_push_constant);
}

/* fld dword/qword [r11] (D9 /0 or DD /0); fstp tbyte [rsp] (DB /7). The
   widen-and-spill leaves the host x87 stack balanced and an 80-bit copy of the
   memory float at [rsp], which is where System V wants an outgoing long double
   argument and where the interpreter's read_float lands the same bits. */
static void x87_widen_mem_to_scratch(X86pEmit *e, int w) {
  x86p_emit_x87_m(e, (w == 4) ? 0xD9u : 0xDDu, 0u, HOSTPTR_REG, 0);
  x86p_emit_x87_m(e, 0xDBu, 7u, kX64Rsp, 0);
}

/* ---- emission -------------------------------------------------------- */

/*
 * FLD -- push a float onto the x87 stack.
 *
 * The value is widened to 80 bits by the host `fld` (exact for every 32- and
 * 64-bit float) and handed to x86p_x87_push, which owns the overflow flags, the
 * tag and TOP exactly as it does for the interpreter. The FLD ST(i) form reads
 * the source with x86p_x87_get first and pushes nothing when that register is
 * empty -- the interpreter's own order, because the push would renumber the
 * position being read.
 *
 * A 16-byte scratch slot is opened on the stack for the long double: System V
 * passes an 80-bit value to a function in memory, and x86p_x87_get writes its
 * result there. `sub rsp, 16` keeps the 16-byte alignment a CALL needs. The
 * memory form's bounds check runs BEFORE the sub, so the fault stub -- which
 * pops RBX and returns assuming the post-prologue RSP -- is never reached with
 * the slot still open.
 */
void emit_x87_load(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pEmit *e = c->e;
  const X86pOperand *o = &insn->operand[0];

  if (o->kind == kX86pOperandMem) {
    const int w = o->size; /* 4 or 8 -- can_emit gate */
    emit_mem_prepare_w(c, o, insn_eip, w);
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
    x87_widen_mem_to_scratch(e, w);
    x87_lea_self(e);
    x87_call(e, (const void *)&x86p_x87_push);
    x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
    return;
  }

  /* FLD ST(i): x86p_x87_get(&cpu->x87, i, &slot); push only when it succeeded. */
  x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, kX64Rsi, (uint32_t)o->reg);
  x86p_emit_lea64(e, kX64Rdx, kX64Rsp, 0);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  {
    X86pEmitSite skip = x86p_emit_jcc_rel32(e, 0x4u); /* jz: source register was empty */
    x87_lea_self(e);
    x87_call(e, (const void *)&x86p_x87_push);
    x86p_emit_bind(e, skip);
  }
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
}

/*
 * FADD / FSUB / FMUL / FDIV (+R, +P). The source operand is widened to 80 bits
 * into a 16-byte stack slot -- from memory with a host `fld`, or from a stack
 * register with x86p_x87_get -- and x86p_x87_arith runs the real op under the
 * guest control word. System V passes the long double `src` in memory, so the
 * slot IS the outgoing argument; `reverse` follows in ECX.
 *
 * A named source register that is empty is a stack fault that arith_operands
 * turns into a whole no-op -- no arithmetic, no pop -- so that path jumps
 * straight to the stack cleanup.
 */
void emit_x87_arith(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pEmit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  const int two_op = (insn->operands == 2);
  const int dst = two_op ? o0->reg : 0; /* mem and short reg form accumulate into ST(0) */
  X86pEmitSite skip;
  int have_skip = 0;
  int i;

  if (o0->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, o0, insn_eip, o0->size);
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
    x87_widen_mem_to_scratch(e, o0->size);
  } else {
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, kX64Rsi, (uint32_t)(two_op ? insn->operand[1].reg : o0->reg));
    x86p_emit_lea64(e, kX64Rdx, kX64Rsp, 0);
    x87_call(e, (const void *)&x86p_x87_get);
    x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
    skip = x86p_emit_jcc_rel32(e, 0x4u); /* jz: empty source register */
    have_skip = 1;
  }

  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, kX64Rsi, (uint32_t)insn->x87_op);
  x86p_emit_mov_r32_imm32(e, kX64Rdx, (uint32_t)dst);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, (uint32_t)insn->x87_reverse);
  x87_call(e, (const void *)&x86p_x87_arith); /* src at [rsp] */

  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, kX64Rsi, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }

  if (have_skip) {
    x86p_emit_bind(e, skip);
  }
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
}

/*
 * FST ST(i) / FSTP ST(i): read ST(0), copy it into ST(i), pop when the P form.
 * Both slots are 80-bit so nothing rounds. An empty ST(0) is a stack fault the
 * interpreter turns into a no-op.
 */
void emit_x87_store_reg(BlockCtx *c, const X86pInsn *insn) {
  X86pEmit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  X86pEmitSite skip;
  int i;

  x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, kX64Rsi, 0u);
  x86p_emit_lea64(e, kX64Rdx, kX64Rsp, 0);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  skip = x86p_emit_jcc_rel32(e, 0x4u); /* jz: ST(0) empty */
  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, kX64Rsi, (uint32_t)o0->reg);
  x87_call(e, (const void *)&x86p_x87_set); /* value at [rsp] */
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, kX64Rsi, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }
  x86p_emit_bind(e, skip);
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
}

/*
 * FST m32/m64 / FSTP m32/m64.
 *
 * ORDER MATTERS. The interpreter reads ST(0) and, only if it is not empty,
 * narrows and writes memory. An empty ST(0) never touches memory, so it never
 * faults on a bad address. This emits the same order: x86p_x87_get into a stack
 * slot, then -- if it succeeded -- x86p_x87_to_f32/f64 (which now round by the
 * guest control word), then the bounds check, then the store, then the pop.
 *
 * The scratch slot is released BEFORE emit_mem_prepare_w so its fault stub sees
 * the post-prologue RSP; the narrowed bits survive that in R9, which the
 * address path (RAX/RDI/R10/R11) does not touch.
 */
void emit_x87_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pEmit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  const int w = o0->size; /* 4 or 8 -- gate */
  X86pEmitSite empty;
  X86pEmitSite done;
  int i;

  x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, kX64Rsi, 0u);
  x86p_emit_lea64(e, kX64Rdx, kX64Rsp, 0);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  empty = x86p_emit_jcc_rel32(e, 0x4u); /* jz: ST(0) empty -> no store, no pop, no fault */

  x87_lea_self(e);
  x87_call(e, (w == 4) ? (const void *)&x86p_x87_to_f32 : (const void *)&x86p_x87_to_f64); /* value at [rsp] */
  x86p_emit_mov_r64_r64(e, kX64R9, kX64Rax);
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);

  emit_mem_prepare_w(c, o0, insn_eip, w);
  if (w == 4) {
    x86p_emit_store32(e, HOSTPTR_REG, 0, kX64R9);
  } else {
    x86p_emit_store64(e, HOSTPTR_REG, 0, kX64R9);
  }
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, kX64Rsi, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }
  done = x86p_emit_jmp_rel32(e);

  x86p_emit_bind(e, empty);
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
  x86p_emit_bind(e, done);
}
