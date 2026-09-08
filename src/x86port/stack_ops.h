#ifndef X86PORT_STACK_OPS_H
#define X86PORT_STACK_OPS_H
#include "cpu.h"
/* Multi-access stack operations preserve every completed access when a later
 * access faults. Fault addresses are optional diagnostic outputs. */
int x86p_stack_pushad(X86pCpu *cpu, const X86pMem *mem, uint32_t *fault);
int x86p_stack_popad(X86pCpu *cpu, const X86pMem *mem, uint32_t *fault);
int x86p_stack_enter(X86pCpu *cpu, const X86pMem *mem, uint32_t allocation, uint32_t level, uint32_t *fault);
#endif
