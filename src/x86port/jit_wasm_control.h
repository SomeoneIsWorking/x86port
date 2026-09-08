#ifndef X86PORT_JIT_WASM_CONTROL_H
#define X86PORT_JIT_WASM_CONTROL_H
#include "cpu.h"
#include "decode.h"
#include "stack_ops.h"
struct X86pWasmLower;
int x86p_wasm_trap(X86pCpu *cpu, uint32_t vector, uint32_t conditional);
int x86p_wasm_cmov_accepts(const X86pInsn *insn);
void x86p_wasm_cmov_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_flags_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_enter_accepts(const X86pInsn *insn);
void x86p_wasm_stack_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_trap_accepts(const X86pInsn *insn);
void x86p_wasm_trap_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_privilege_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_cpu_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
#endif
