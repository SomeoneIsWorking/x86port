#ifndef X86PORT_JIT_WASM_SIMD_H
#define X86PORT_JIT_WASM_SIMD_H

#include "cpu.h"
#include "decode.h"

struct X86pWasmLower;

/* Source lanes are captured by emitted code before this arithmetic ABI is
 * entered. No instruction object, guest address, or decoder crosses it. */
uint32_t x86p_wasm_simd_arithmetic(X86pCpu *cpu,
                                   uint32_t destination,
                                   uint32_t operation,
                                   uint32_t b0,
                                   uint32_t b1,
                                   uint32_t b2,
                                   uint32_t b3,
                                   uint32_t immediate);
int x86p_wasm_simd_accepts(const X86pInsn *insn);
void x86p_wasm_simd_lower(struct X86pWasmLower *lower, const X86pInsn *insn, uint32_t pc);

#endif
