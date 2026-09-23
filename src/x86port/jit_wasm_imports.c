#include "jit_wasm_imports.h"
#include "alu.h"
#include "cond.h"
#include "flags.h"
#include "jit_wasm_bitops.h"
#include "jit_wasm_chain.h"
#include "jit_wasm_control.h"
#include "jit_wasm_integer.h"
#include "jit_wasm_memory.h"
#include "jit_wasm_simd.h"
#include "jit_wasm_x87.h"
#include "x87.h"
#include <stddef.h>

typedef struct X86pWasmImportDesc {
  const char *name;
  X86pWasmImportFn address;
  uint8_t parameters;
  uint8_t returns_value;
} X86pWasmImportDesc;

static const X86pWasmImportDesc kImports[kX86pWasmImportCount] = {
    [kX86pWasmImportAlu] = {"alu", (X86pWasmImportFn)x86p_alu, 5, 1},
    [kX86pWasmImportAluUnary] = {"alu_unary", (X86pWasmImportFn)x86p_alu_unary, 4, 1},
    [kX86pWasmImportCond] = {"cond", (X86pWasmImportFn)x86p_cond, 2, 1},
    [kX86pWasmImportFlagCf] = {"flag_cf", (X86pWasmImportFn)x86p_flag_cf, 1, 1},
    [kX86pWasmImportMemOk] = {"mem_ok", (X86pWasmImportFn)x86p_wasm_mem_ok, 4, 1},
    [kX86pWasmImportMemLoad] = {"mem_load", (X86pWasmImportFn)x86p_wasm_mem_load, 3, 1},
    [kX86pWasmImportMemStore] = {"mem_store", (X86pWasmImportFn)x86p_wasm_mem_store, 4, 0},
    [kX86pWasmImportMultiply] = {"multiply", (X86pWasmImportFn)x86p_wasm_multiply, 6, 1},
    [kX86pWasmImportDivide] = {"divide", (X86pWasmImportFn)x86p_wasm_divide, 4, 1},
    [kX86pWasmImportString] = {"string", (X86pWasmImportFn)x86p_wasm_string, 5, 1},
    [kX86pWasmImportLoop] = {"loop", (X86pWasmImportFn)x86p_cpu_loop, 3, 1},
    [kX86pWasmImportGetFlags] = {"get_flags", (X86pWasmImportFn)x86p_wasm_get_flags, 1, 1},
    [kX86pWasmImportSetFlags] = {"set_flags", (X86pWasmImportFn)x86p_wasm_set_flags, 2, 1},
    [kX86pWasmImportDoubleShift] = {"wasm_double_shift", (X86pWasmImportFn)x86p_wasm_double_shift, 6, 1},
    [kX86pWasmImportBit] = {"wasm_bit", (X86pWasmImportFn)x86p_wasm_bit, 4, 1},
    [kX86pWasmImportBcd] = {"wasm_bcd", (X86pWasmImportFn)x86p_wasm_bcd, 3, 1},
    [kX86pWasmImportPushad] = {"wasm_pushad", (X86pWasmImportFn)x86p_stack_pushad, 3, 1},
    [kX86pWasmImportPopad] = {"wasm_popad", (X86pWasmImportFn)x86p_stack_popad, 3, 1},
    [kX86pWasmImportEnter] = {"wasm_enter", (X86pWasmImportFn)x86p_stack_enter, 5, 1},
    [kX86pWasmImportTrap] = {"wasm_trap", (X86pWasmImportFn)x86p_wasm_trap, 3, 1},
    [kX86pWasmImportSahf] = {"cpu_sahf", (X86pWasmImportFn)x86p_cpu_sahf, 1, 0},
    [kX86pWasmImportLahf] = {"cpu_lahf", (X86pWasmImportFn)x86p_cpu_lahf, 1, 0},
    [kX86pWasmImportCpuid] = {"cpu_cpuid", (X86pWasmImportFn)x86p_cpu_cpuid, 1, 0},
    [kX86pWasmImportRdtsc] = {"cpu_rdtsc", (X86pWasmImportFn)x86p_cpu_rdtsc, 1, 0},
    [kX86pWasmImportX87LoadBits] = {"wasm_x87_load_bits", (X86pWasmImportFn)x86p_wasm_x87_load_bits, 6, 1},
    [kX86pWasmImportX87Store] = {"wasm_x87_store", (X86pWasmImportFn)x86p_wasm_x87_store, 6, 1},
    [kX86pWasmImportX87StoreAt] = {"wasm_x87_store_at", (X86pWasmImportFn)x86p_wasm_x87_store_at, 6, 1},
    [kX86pWasmImportX87ArithMemBits] = {"wasm_x87_arith_mem_bits",
                                        (X86pWasmImportFn)x86p_wasm_x87_arith_mem_bits,
                                        8,
                                        1},
    [kX86pWasmImportX87ArithReg] = {"wasm_x87_arith_reg", (X86pWasmImportFn)x86p_wasm_x87_arith_reg, 6, 1},
    [kX86pWasmImportX87CompareMemBits] = {"wasm_x87_compare_mem_bits",
                                          (X86pWasmImportFn)x86p_wasm_x87_compare_mem_bits,
                                          6,
                                          1},
    [kX86pWasmImportX87Copy] = {"wasm_x87_copy", (X86pWasmImportFn)x86p_wasm_x87_copy, 5, 1},
    [kX86pWasmImportX87Constant] = {"x87_push_constant", (X86pWasmImportFn)x86p_x87_push_constant, 2, 1},
    [kX86pWasmImportX87Status] = {"x87_status", (X86pWasmImportFn)x86p_x87_status, 1, 1},
    [kX86pWasmImportX87Clear] = {"x87_clear_exceptions", (X86pWasmImportFn)x86p_x87_clear_exceptions, 1, 0},
    [kX86pWasmImportX87Reset] = {"x87_reset", (X86pWasmImportFn)x86p_x87_reset, 1, 0},
    [kX86pWasmImportX87Fn] = {"x87_apply_fn", (X86pWasmImportFn)x86p_x87_apply_fn, 2, 1},
    [kX86pWasmImportX87Pop] = {"x87_pop", (X86pWasmImportFn)x86p_x87_pop, 2, 1},
    [kX86pWasmImportX87CompareRegister] = {"x87_compare_register", (X86pWasmImportFn)x86p_x87_compare_register, 3, 0},
    [kX86pWasmImportX87Exchange] = {"x87_exchange", (X86pWasmImportFn)x86p_x87_exchange, 2, 0},
    [kX86pWasmImportX87Sign] = {"x87_sign", (X86pWasmImportFn)x86p_x87_sign, 2, 0},
    [kX86pWasmImportX87Test] = {"x87_test", (X86pWasmImportFn)x86p_x87_test, 1, 0},
    [kX86pWasmImportX87CompareFlags] = {"x87_compare_flags", (X86pWasmImportFn)x86p_x87_compare_flags, 3, 1},
    [kX86pWasmImportX87Free] = {"x87_free", (X86pWasmImportFn)x86p_x87_free, 2, 0},
    [kX86pWasmImportX87Emms] = {"x87_emms", (X86pWasmImportFn)x86p_x87_emms, 1, 0},
    [kX86pWasmImportSimdArithmetic] = {"wasm_simd_arithmetic", (X86pWasmImportFn)x86p_wasm_simd_arithmetic, 8, 1},
    [kX86pWasmImportChainCall] = {"wasm_chain_call", (X86pWasmImportFn)x86p_wasm_chain_call, 2, 1},
};

const char *x86p_wasm_import_field(X86pWasmImport which) {
  return (unsigned)which < kX86pWasmImportCount ? kImports[which].name : "";
}
X86pWasmImportFn x86p_wasm_import_address(X86pWasmImport which) {
  return (unsigned)which < kX86pWasmImportCount ? kImports[which].address : NULL;
}
uint32_t x86p_wasm_import_type(X86pWasmImport which) {
  if ((unsigned)which >= kX86pWasmImportCount) {
    return 0;
  }
  const X86pWasmImportDesc *desc = &kImports[which];
  return (uint32_t)(desc->parameters - 1u) + (desc->returns_value ? 0u : 8u);
}
