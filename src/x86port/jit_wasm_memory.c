#include "jit_wasm_memory.h"

#include "diagnostic.h"

int x86p_wasm_mem_ok(const X86pMem *mem, uint32_t address, uint32_t width, unsigned access) {
  return x86p_mem_accessible(mem, address, width, access);
}

uint32_t x86p_wasm_mem_load(const X86pMem *mem, uint32_t address, uint32_t width) {
  uint32_t value;
  if (!x86p_mem_read(mem, address, (int)width, &value)) {
    x86p_diagnostic_fatalf("wasm.memory", "checked guest read changed mapping at %08x width %u", address, width);
  }
  return value;
}

void x86p_wasm_mem_store(const X86pMem *mem, uint32_t address, uint32_t width, uint32_t value) {
  if (!x86p_mem_write(mem, address, (int)width, value)) {
    x86p_diagnostic_fatalf("wasm.memory", "checked guest write changed mapping at %08x width %u", address, width);
  }
}
