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

/* ---- shared emission helpers ----------------------------------------- */

/* First argument to every x86p_x87_* helper. */
static void x87_lea_self(X86pEmit *e) {
  x86p_emit_lea64(e, X86P_JIT_HOST_ARG0, CPU_REG, x87_off());
}

/* mov rax, imm64(fn); call rax. */
static void x87_call(X86pEmit *e, const void *fn) {
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)fn);
  x86p_emit_call_r64(e, kX64Rax);
}

/* Keep long double out of the generated-code ABI. System V passes an 80-bit
 * long double by value in memory while Win64 passes non-register-sized values
 * indirectly. These pointer-valued adapters give both emitters one ordinary
 * register-argument contract and dereference only inside compiler-generated C. */
static int jit_x87_push(X86pX87 *x87, const long double *value) {
  return x86p_x87_push(x87, *value);
}

static int jit_x87_set(X86pX87 *x87, uint32_t index, const long double *value) {
  return x86p_x87_set(x87, (int)index, *value);
}

static int jit_x87_arith(X86pX87 *x87, uint32_t operation_destination_reverse, const long double *source) {
  X86pX87Op operation = (X86pX87Op)(operation_destination_reverse & 0xFFu);
  int destination = (int)((operation_destination_reverse >> 8) & 0xFFu);
  int reverse = (int)((operation_destination_reverse >> 16) & 1u);
  return x86p_x87_arith(x87, operation, destination, *source, reverse);
}

static int jit_x87_compare(X86pX87 *x87, const long double *value) {
  return x86p_x87_compare(x87, *value);
}

static uint32_t jit_x87_to_f32(const X86pX87 *x87, const long double *value) {
  return x86p_x87_to_f32(x87, *value);
}

static uint64_t jit_x87_to_f64(const X86pX87 *x87, const long double *value) {
  return x86p_x87_to_f64(x87, *value);
}

static int32_t x87_scratch_off(void) {
  return X86P_JIT_HOST_CALL_FRAME_BYTES;
}

static void x87_lea_scratch(X86pEmit *e, X86pHostReg destination) {
  x86p_emit_lea64(e, destination, kX64Rsp, x87_scratch_off());
}

void emit_x87_constant(BlockCtx *c, const X86pInsn *insn) {
  x87_lea_self(c->e);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG1, insn->x87);
  x87_call(c->e, (const void *)&x86p_x87_push_constant);
}

/* FNSTSW AX. x86p_x87_status is the one owner that replaces any stale TOP
   bits in the stored status field with the live stack pointer. A 16-bit store
   into the guest EAX slot preserves its upper half exactly like
   x86p_reg_write(..., width=2), and none of this materialises or changes the
   separate integer EFLAGS model. */
void emit_x87_status_ax(BlockCtx *c) {
  x87_lea_self(c->e);
  x87_call(c->e, (const void *)&x86p_x87_status);
  x86p_emit_store16_reg(c->e, CPU_REG, (int32_t)offsetof(X86pCpu, reg[kX86pEax]), kX64Rax);
}

/* FNCLEX changes no integer flags or registers. The shared semantic owner
   clears precisely the architectural exception/busy mask in the guest x87
   status field and leaves every other x87 field untouched. */
void emit_x87_clear_exceptions(BlockCtx *c) {
  x87_lea_self(c->e);
  x87_call(c->e, (const void *)&x86p_x87_clear_exceptions);
}

/* fld dword/qword [r11] (D9 /0 or DD /0); fstp tbyte into the ABI-owned scratch
   slot. The widen-and-spill leaves the host x87 stack balanced and an 80-bit
   copy of the memory float for the pointer-valued helper adapter. */
static void x87_widen_mem_to_scratch(X86pEmit *e, int w) {
  x86p_emit_x87_m(e, (w == 4) ? 0xD9u : 0xDDu, 0u, HOSTPTR_REG, 0);
  x86p_emit_x87_m(e, 0xDBu, 7u, kX64Rsp, x87_scratch_off());
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
 * A 16-byte scratch slot is opened on the stack for the long double. On Win64
 * it sits above the active shadow/stack-argument area; on System V it begins at
 * RSP. `sub rsp, 16` keeps the 16-byte alignment a CALL needs. The memory
 * form's bounds check runs BEFORE the sub, so the fault stub -- which restores
 * the ABI frame and returns -- is never reached with the slot still open.
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
    x87_lea_scratch(e, X86P_JIT_HOST_ARG1);
    x87_call(e, (const void *)&jit_x87_push);
    x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
    return;
  }

  /* FLD ST(i): x86p_x87_get(&cpu->x87, i, &slot); push only when it succeeded. */
  x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, (uint32_t)o->reg);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG2);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  {
    X86pEmitSite skip = x86p_emit_jcc_rel32(e, 0x4u); /* jz: source register was empty */
    x87_lea_self(e);
    x87_lea_scratch(e, X86P_JIT_HOST_ARG1);
    x87_call(e, (const void *)&jit_x87_push);
    x86p_emit_bind(e, skip);
  }
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
}

/*
 * FADD / FSUB / FMUL / FDIV (+R, +P). The source operand is widened to 80 bits
 * into a 16-byte stack slot -- from memory with a host `fld`, or from a stack
 * register with x86p_x87_get -- and x86p_x87_arith runs the real op under the
 * guest control word. A pointer-valued adapter keeps the host's by-value long
 * double convention out of emitted code on both ABIs.
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
  uint32_t operation_destination_reverse;
  int have_skip = 0;
  int i;

  if (o0->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, o0, insn_eip, o0->size);
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
    x87_widen_mem_to_scratch(e, o0->size);
  } else {
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, (uint32_t)(two_op ? insn->operand[1].reg : o0->reg));
    x87_lea_scratch(e, X86P_JIT_HOST_ARG2);
    x87_call(e, (const void *)&x86p_x87_get);
    x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
    skip = x86p_emit_jcc_rel32(e, 0x4u); /* jz: empty source register */
    have_skip = 1;
  }

  x87_lea_self(e);
  operation_destination_reverse = (uint32_t)insn->x87_op | ((uint32_t)dst << 8) | ((uint32_t)insn->x87_reverse << 16);
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, operation_destination_reverse);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG2);
  x87_call(e, (const void *)&jit_x87_arith);

  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }

  if (have_skip) {
    x86p_emit_bind(e, skip);
  }
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
}

/*
 * FCOM / FCOMP m32/m64. The memory access is validated before opening the
 * outgoing long-double stack slot so the shared fault stub always observes
 * the post-prologue RSP. A host FLD widens the operand exactly and
 * x86p_x87_compare remains the sole owner of C0/C2/C3, NaN and empty-ST(0)
 * status semantics. FCOMP then pops through the same stack owner as the
 * interpreter. Neither comparison nor pop changes integer EFLAGS.
 */
void emit_x87_compare_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pEmit *e = c->e;
  const X86pOperand *o0 = &insn->operand[0];
  int i;

  emit_mem_prepare_w(c, o0, insn_eip, o0->size);
  x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
  x87_widen_mem_to_scratch(e, o0->size);
  x87_lea_self(e);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG1);
  x87_call(e, (const void *)&jit_x87_compare);
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
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
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, 0u);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG2);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  skip = x86p_emit_jcc_rel32(e, 0x4u); /* jz: ST(0) empty */
  x87_lea_self(e);
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, (uint32_t)o0->reg);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG2);
  x87_call(e, (const void *)&jit_x87_set);
  for (i = 0; i < (int)insn->x87_pops; i++) {
    x87_lea_self(e);
    x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, 0u);
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
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, 0u);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG2);
  x87_call(e, (const void *)&x86p_x87_get);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  empty = x86p_emit_jcc_rel32(e, 0x4u); /* jz: ST(0) empty -> no store, no pop, no fault */

  x87_lea_self(e);
  x87_lea_scratch(e, X86P_JIT_HOST_ARG1);
  x87_call(e, (w == 4) ? (const void *)&jit_x87_to_f32 : (const void *)&jit_x87_to_f64);
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
    x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG1, 0u);
    x87_call(e, (const void *)&x86p_x87_pop);
  }
  done = x86p_emit_jmp_rel32(e);

  x86p_emit_bind(e, empty);
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
  x86p_emit_bind(e, done);
}
