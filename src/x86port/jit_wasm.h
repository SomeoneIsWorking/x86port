/*
 * jit_wasm.h -- the WebAssembly backend's one extra step.
 *
 * The rest of this backend is the x86p_jit_* contract declared in jit_x64.h
 * and behaves as the other two hosts do, with ONE difference that cannot be
 * hidden: on a wasm host, translated output is a module, and a module is not
 * an address. x86p_jit_translate therefore leaves X86pJitBlock::entry NULL and
 * the block is not enterable until the engine has instantiated it.
 *
 * That is the same division of labour the contract already states -- "this
 * function never makes it executable ... publishing is the caller's job" --
 * with a different publication step. Saying so in a header of its own rather
 * than letting entry stay NULL and be discovered at the first call.
 */
#ifndef X86PORT_JIT_WASM_H
#define X86PORT_JIT_WASM_H

#include "jit_wasm_arena.h"
#include "jit_x64.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Instantiate the module x86p_jit_translate wrote and make `block` enterable.
 *
 * `module` and `len` are the buffer that was translated into and
 * `block->host_bytes` -- the module's whole length, not the body's. On success
 * `block->entry` is set and the return value is the arena token the caller
 * must hand back to x86p_wasm_arena_release when it discards the block. On
 * failure the return is -1, `reason` says why, and `block->entry` is left
 * NULL.
 *
 * The arena is passed in rather than found: which engine a block is published
 * into is the consumer's decision, and a backend holding one in a static would
 * make two guests in one process share it.
 */
int x86p_jit_wasm_publish(
    X86pWasmArena *arena, X86pJitBlock *block, const void *module, size_t len, char *reason, unsigned reason_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_H */
