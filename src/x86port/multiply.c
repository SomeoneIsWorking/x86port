#include "multiply.h"

#include "alu.h"

uint32_t x86p_multiply(
    X86pCpu *cpu, uint32_t left, uint32_t right, uint32_t width, uint32_t signed_multiply, uint32_t implicit) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  if (signed_multiply) {
    x86p_alu_imul(left, right, (int)width, &low, &high, &cpu->flags);
  } else {
    x86p_alu_mul(left, right, (int)width, &low, &high, &cpu->flags);
  }
  if (implicit) {
    if (width == 1u) {
      x86p_reg_write(cpu, kX86pEax, 2, low | (high << 8));
    } else {
      x86p_reg_write(cpu, kX86pEax, (int)width, low);
      x86p_reg_write(cpu, kX86pEdx, (int)width, high);
    }
  }
  return low;
}

void x86p_multiply_accumulator(X86pCpu *cpu, uint32_t operand, uint32_t signed_multiply, uint32_t width) {
  x86p_multiply(cpu, x86p_reg_read(cpu, kX86pEax, (int)width), operand, width, signed_multiply, 1u);
}

void x86p_imul_to_register(X86pCpu *cpu, uint32_t left, uint32_t right, uint32_t destination, uint32_t width) {
  x86p_reg_write(cpu, (int)destination, (int)width, x86p_multiply(cpu, left, right, width, 1u, 0u));
}
