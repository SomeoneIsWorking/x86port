#include "jit_x64_internal.h"
#include "x87.h"
#include <stddef.h>

int x87_register_is_emittable(const X86pInsn *insn) {
  switch (insn->x87) {
  case kX86pX87InsnCompare:
  case kX86pX87InsnExchange:
    return insn->operands == 0 || (insn->operands == 1 && insn->operand[0].kind == kX86pOperandSt &&
                                   insn->operand[0].reg >= 0 && insn->operand[0].reg < 8);
  case kX86pX87InsnChangeSign:
  case kX86pX87InsnAbs:
  case kX86pX87InsnTest:
    return insn->operands == 0;
  default:
    return 0;
  }
}
void emit_x87_register(BlockCtx *c, const X86pInsn *insn) {
  const unsigned index = insn->operands ? (unsigned)insn->operand[0].reg : 1;
  const void *fn = insn->x87 == kX86pX87InsnCompare    ? (const void *)&x86p_x87_compare_register
                   : insn->x87 == kX86pX87InsnExchange ? (const void *)&x86p_x87_exchange
                   : insn->x87 == kX86pX87InsnTest     ? (const void *)&x86p_x87_test
                                                       : (const void *)&x86p_x87_sign;
  const unsigned arg =
      insn->x87 == kX86pX87InsnChangeSign || insn->x87 == kX86pX87InsnAbs ? insn->x87 == kX86pX87InsnAbs : index;
  x86p_emit_lea64(c->e, kX64Rdi, CPU_REG, (int32_t)offsetof(X86pCpu, x87));
  x86p_emit_mov_r32_imm32(c->e, kX64Rsi, arg);
  x86p_emit_mov_r32_imm32(c->e, kX64Rdx, insn->x87_pops);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)fn);
  x86p_emit_call_r64(c->e, kX64Rax);
}
