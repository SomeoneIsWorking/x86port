/* The scalar helper ABI shared by module encoding and the actual host. */
#ifndef X86PORT_JIT_WASM_IMPORTS_H
#define X86PORT_JIT_WASM_IMPORTS_H
#include "jit_wasm_module.h"
/* i32-return signatures with 1..8 parameters precede void signatures 1..4. */
uint32_t x86p_wasm_import_type(X86pWasmImport which);
#endif
