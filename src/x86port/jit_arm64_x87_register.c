#include "jit_arm64_internal.h"
#include "x87.h"
#include <stddef.h>

void emit_x87_register(BlockCtx *c, const X86pInsn *insn) {
  const unsigned index = insn->operands ? (unsigned)insn->operand[0].reg : 1;
  const void *fn = insn->x87 == kX86pX87InsnCompare    ? (const void *)&x86p_x87_compare_register
                   : insn->x87 == kX86pX87InsnExchange ? (const void *)&x86p_x87_exchange
                   : insn->x87 == kX86pX87InsnTest     ? (const void *)&x86p_x87_test
                                                       : (const void *)&x86p_x87_sign;
  const unsigned arg =
      insn->x87 == kX86pX87InsnChangeSign || insn->x87 == kX86pX87InsnAbs ? insn->x87 == kX86pX87InsnAbs : index;
  x86p_a64_emit_lea64(c->e, kA64X0, CPU_REG, (int32_t)offsetof(X86pCpu, x87));
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, arg);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, insn->x87_pops);
  x86p_a64_emit_mov_x_imm64(c->e, kA64X9, (uint64_t)(uintptr_t)fn);
  x86p_a64_emit_blr(c->e, kA64X9);
}
