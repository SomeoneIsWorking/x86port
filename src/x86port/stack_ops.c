#include "stack_ops.h"

static int push(X86pCpu *cpu, const X86pMem *mem, uint32_t value, uint32_t *fault) {
  if (x86p_push32(cpu, mem, value)) {
    return 1;
  }
  if (fault) {
    *fault = cpu->reg[kX86pEsp] - 4u;
  }
  return 0;
}

int x86p_stack_pushad(X86pCpu *cpu, const X86pMem *mem, uint32_t *fault) {
  static const int order[] = {kX86pEax, kX86pEcx, kX86pEdx, kX86pEbx, kX86pEsp, kX86pEbp, kX86pEsi, kX86pEdi};
  const uint32_t original_esp = cpu->reg[kX86pEsp];
  unsigned i;
  for (i = 0; i < sizeof order / sizeof order[0]; ++i) {
    const uint32_t value = order[i] == kX86pEsp ? original_esp : cpu->reg[order[i]];
    if (!push(cpu, mem, value, fault)) {
      return 0;
    }
  }
  return 1;
}

int x86p_stack_popad(X86pCpu *cpu, const X86pMem *mem, uint32_t *fault) {
  static const int order[] = {kX86pEdi, kX86pEsi, kX86pEbp, kX86pEsp, kX86pEbx, kX86pEdx, kX86pEcx, kX86pEax};
  unsigned i;
  for (i = 0; i < sizeof order / sizeof order[0]; ++i) {
    uint32_t value;
    if (!x86p_pop32(cpu, mem, &value)) {
      if (fault) {
        *fault = cpu->reg[kX86pEsp];
      }
      return 0;
    }
    /* POPAD consumes the saved stack slot but never restores its value. */
    if (order[i] != kX86pEsp) {
      cpu->reg[order[i]] = value;
    }
  }
  return 1;
}

int x86p_stack_enter(X86pCpu *cpu, const X86pMem *mem, uint32_t allocation, uint32_t level, uint32_t *fault) {
  uint32_t frame;
  unsigned i;
  level &= 31u;
  if (!push(cpu, mem, cpu->reg[kX86pEbp], fault)) {
    return 0;
  }
  frame = cpu->reg[kX86pEsp];
  for (i = 1u; i < level; ++i) {
    uint32_t value;
    cpu->reg[kX86pEbp] -= 4u;
    if (!x86p_mem_read(mem, cpu->reg[kX86pEbp], 4, &value)) {
      if (fault) {
        *fault = cpu->reg[kX86pEbp];
      }
      return 0;
    }
    if (!push(cpu, mem, value, fault)) {
      return 0;
    }
  }
  if (level && !push(cpu, mem, frame, fault)) {
    return 0;
  }
  cpu->reg[kX86pEbp] = frame;
  cpu->reg[kX86pEsp] -= allocation & 0xffffu;
  return 1;
}
