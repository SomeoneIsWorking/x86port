#include "jit_x87_predicates.h"
#include "cpu.h"
#include "x87.h"

int x87_values_are_emittable(void) {
#if defined(__APPLE__) && defined(__aarch64__)
  /* STOPGAP: host-independent f80 storage/arithmetic is the proper fidelity fix.
   * The user explicitly approved retaining the playable Mac binary64 path;
   * this admission must not be reported as exact x87 representation. */
  return 1;
#else
  return x86p_x87_precision_is_exact();
#endif
}

int x87_register_is_emittable(const X86pInsn *insn) {
  if (!x87_values_are_emittable()) {
    return 0;
  }
  switch (insn->x87) {
  case kX86pX87InsnCompare:
  case kX86pX87InsnExchange:
    return insn->operands == 0 || (insn->operands == 1 && insn->operand[0].kind == kX86pOperandSt &&
                                   insn->operand[0].reg >= 0 && insn->operand[0].reg < X86P_X87_REGS);
  case kX86pX87InsnChangeSign:
  case kX86pX87InsnAbs:
  case kX86pX87InsnTest:
    return insn->operands == 0;
  default:
    return 0;
  }
}

int x87_fn_is_emittable(const X86pInsn *insn) {
  return x87_values_are_emittable() && insn->x87 == kX86pX87InsnFn && insn->operands == 0 &&
         insn->x87_fn < kX86pX87FnCount && insn->x87_fn != kX86pX87FnXtract;
}

int x87_control_is_emittable(const X86pInsn *insn) {
  return (insn->x87 == kX86pX87InsnLoadControl || insn->x87 == kX86pX87InsnStoreControl) && insn->operands == 1 &&
         insn->operand[0].kind == kX86pOperandMem && insn->operand[0].size == 2 && !insn->operand[0].addr16;
}

/* Shape and value admission are shared by every host emitter. Status/control
 * forms do not require value admission; exactness remains separate metadata. */

/*
 * FLD accepts register copies and binary32/binary64 memory operands only
 * under the admitted value representation. FILD also accepts 16/32/64-bit
 * integer memory. Widening belongs to each backend; FLD m80 remains refused.
 */
int x87_load_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o;
  if (x87_values_are_emittable() && insn->x87 == kX86pX87InsnLoadInt) {
    o = &insn->operand[0];
    return insn->operands == 1 && o->kind == kX86pOperandMem && !o->addr16 &&
           (o->size == 2 || o->size == 4 || o->size == 8);
  }
  if (!x87_values_are_emittable() || insn->x87 != (uint8_t)kX86pX87InsnLoad || insn->x87_mem_int ||
      insn->operands != 1) {
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
  if (!x87_values_are_emittable() || insn->x87 != (uint8_t)kX86pX87InsnArith) {
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
  return insn->operands == 1 && o0->kind == kX86pOperandMem &&
         (insn->x87_mem_int ? (o0->size == 2 || o0->size == 4) : (o0->size == 4 || o0->size == 8)) && !o0->addr16;
}

/*
 * FCOM / FCOMP against memory float or integer operands. Register and implicit
 * forms use x87_register_is_emittable and their own stack-behavior emitter.
 */
int x87_compare_mem_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  return x87_values_are_emittable() && insn->x87 == (uint8_t)kX86pX87InsnCompare && insn->operands == 1 &&
         o0->kind == kX86pOperandMem &&
         (insn->x87_mem_int ? (o0->size == 2 || o0->size == 4) : (o0->size == 4 || o0->size == 8)) && !o0->addr16;
}

/*
 * FST / FSTP to a stack position (not memory). Both slots share the same host
 * representation, so this transfer adds no narrowing.
 */
int x87_store_reg_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  return x87_values_are_emittable() && insn->x87 == (uint8_t)kX86pX87InsnStore && insn->operands == 1 &&
         o0->kind == kX86pOperandSt && o0->reg >= 0 && o0->reg < X86P_X87_REGS;
}

/*
 * FST / FSTP to binary32/binary64 memory uses the shared narrowing semantics.
 * Emitters preserve the source-stack check before accessing guest memory;
 * that ordering is separate from admitting the decoded operand shape here.
 */
int x87_store_mem_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  return x87_values_are_emittable() && (insn->x87 == kX86pX87InsnStore || insn->x87 == kX86pX87InsnStoreInt) &&
         insn->operands == 1 && o0->kind == kX86pOperandMem &&
         (o0->size == 4 || o0->size == 8 || (insn->x87 == kX86pX87InsnStoreInt && o0->size == 2)) && !o0->addr16;
}

int x87_constant_is_emittable(const X86pInsn *insn) {
  if (!x87_values_are_emittable() || insn->operands != 0) {
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

/* FNSTSW AX is the only register form. The decoder exposes the architecturally
   fixed AX as an explicit 16-bit operand; the memory form remains outside this
   predicate. */
int x87_status_ax_is_emittable(const X86pInsn *insn) {
  const X86pOperand *o0 = &insn->operand[0];
  return insn->x87 == (uint8_t)kX86pX87InsnStoreStatus && insn->operands == 1 && o0->kind == kX86pOperandReg &&
         o0->reg == kX86pEax && o0->size == 2;
}

/* FNCLEX is exactly the zero-operand DB E2 instruction. Keeping the shape in
   the predicate prevents another x87 instruction, or a malformed decoded
   instruction carrying operands, from reaching its status-only emitter. */
int x87_clear_exceptions_is_emittable(const X86pInsn *insn) {
  return insn->x87 == (uint8_t)kX86pX87InsnClearExc && insn->operands == 0;
}
