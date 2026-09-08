#ifndef X86PORT_JIT_WASM_INTEGER_H
#define X86PORT_JIT_WASM_INTEGER_H

#include "cpu.h"
#include "decode.h"

struct X86pWasmLower;

/* Narrow ABI adapters for the shared integer semantic owners. */
uint32_t x86p_wasm_multiply(
    X86pCpu *cpu, uint32_t left, uint32_t right, uint32_t width, uint32_t signed_multiply, uint32_t implicit);
int x86p_wasm_divide(X86pCpu *cpu, uint32_t divisor, uint32_t width, uint32_t signed_divide);
int x86p_wasm_string(X86pCpu *cpu, const X86pMem *mem, uint32_t operation, uint32_t repeat, uint32_t width);
uint32_t x86p_wasm_get_flags(X86pCpu *cpu);
uint32_t x86p_wasm_set_flags(X86pCpu *cpu, uint32_t value);

int x86p_wasm_multiply_accepts(const X86pInsn *insn);
void x86p_wasm_multiply_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_divide_accepts(const X86pInsn *insn);
void x86p_wasm_divide_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_string_accepts(const X86pInsn *insn);
void x86p_wasm_string_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_loop_accepts(const X86pInsn *insn);
void x86p_wasm_loop_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_pushfd_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
void x86p_wasm_popfd_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

#endif
