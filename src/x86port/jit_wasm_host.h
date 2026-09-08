#ifndef X86PORT_JIT_WASM_HOST_H
#define X86PORT_JIT_WASM_HOST_H

#include "jit_wasm_arena.h"

/* An instance-local Emscripten host. It binds emitted module imports to this
 * program's real helper functions and memory, and owns indirect-table slots.
 * Browser callers must run on a worker; Node is also supported for conformance. */
int x86p_wasm_host_create(X86pWasmHost *host, char *reason, unsigned reason_len);
void x86p_wasm_host_destroy(X86pWasmHost *host);
const char *x86p_wasm_host_error(const X86pWasmHost *host);

#endif
