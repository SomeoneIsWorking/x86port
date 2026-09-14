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

/*
 * Translate a RUN of blocks into ONE module, and say how long that module is.
 *
 * Instantiation is charged per module and not per byte -- measured, ~0.52 ms for
 * a 5.6 KB module against 0.0047 ms for a synthetic one of 4 KB, so what is
 * being paid for is the module's shape (its imports and its table entry), not
 * the code inside it. A consumer that publishes one body per module therefore
 * pays that fixed price for every ~5-instruction block it translates, which is
 * what a translation-heavy phase spends its time on.
 *
 * This is the same translation contract as x86p_jit_translate_bounded with two
 * differences: `blocks` receives up to `max_blocks` of them, and `entry` stays
 * NULL for every one -- x86p_wasm_arena_publish instantiates the module, and
 * x86p_wasm_body_name(i) is the export each block's entry comes from.
 *
 * The run is the STRAIGHT LINE the translator itself would cut, so batching
 * changes no boundary: the chain starts at `eip` and continues at each block's
 * own end while the boundary policy keeps producing blocks and the translator
 * keeps accepting them. A refusal -- or a block the boundary policy ends -- ends
 * the run, and the blocks before it are still a valid module.
 *
 * The first pass lowers each candidate on its own to learn where it ends, so a
 * run costs two emissions and one instantiation. That is the price of not
 * needing to know the count in advance: the module's function and export
 * sections are written when it opens, and a module that promised bodies it
 * never wrote is rejected.
 */
X86pJitStatus x86p_jit_translate_chain(const X86pMem *mem,
                                       uint32_t eip,
                                       void *code,
                                       size_t code_cap,
                                       X86pJitBoundaryFn boundary,
                                       void *boundary_user,
                                       X86pJitBlock *blocks,
                                       unsigned max_blocks,
                                       unsigned *count,
                                       size_t *module_bytes,
                                       char *reason,
                                       unsigned reason_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_H */
