#ifndef X86PORT_JIT_WASM_BITOPS_H
#define X86PORT_JIT_WASM_BITOPS_H
#include "cpu.h"
#include "decode.h"
struct X86pWasmLower;
uint32_t
x86p_wasm_double_shift(X86pCpu *cpu, uint32_t dst, uint32_t src, uint32_t count, uint32_t width, uint32_t left);
uint32_t x86p_wasm_bit(X86pCpu *cpu, uint32_t value, uint32_t index, uint32_t operation);
int x86p_wasm_bcd(X86pCpu *cpu, uint32_t operation, uint32_t immediate);
int x86p_wasm_shift_accepts(const X86pInsn *insn);
void x86p_wasm_shift_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_bit_accepts(const X86pInsn *insn);
void x86p_wasm_bit_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
int x86p_wasm_bcd_accepts(const X86pInsn *insn);
void x86p_wasm_bcd_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
#endif
