#ifndef X86PORT_JIT_WASM_MEMORY_H
#define X86PORT_JIT_WASM_MEMORY_H

#include "cpu.h"

int x86p_wasm_mem_ok(const X86pMem *mem, uint32_t address, uint32_t width);
uint32_t x86p_wasm_mem_load(const X86pMem *mem, uint32_t address, uint32_t width);
void x86p_wasm_mem_store(const X86pMem *mem, uint32_t address, uint32_t width, uint32_t value);

#endif
