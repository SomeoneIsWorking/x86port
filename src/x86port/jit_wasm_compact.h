/*
 * jit_wasm_compact.h -- making one module out of many published blocks.
 *
 * WHY THIS EXISTS. A block must be runnable the instant it is translated: the
 * engine translates on a cache miss and enters the result immediately, and
 * this backend has nothing to run a block with except a module. So blocks are
 * published one to a module, and on a wasm host every published module is a
 * permanent engine object. Measured in Firefox 156, instantiation is refused
 * at about 16,350 live modules -- a limit X-Men Legends II reaches four
 * seconds into its boot, with 49,000 of the arena's own slots still free.
 *
 * Eviction cannot answer that. Every module dropped is a block the run pays to
 * translate again, and the limit is reached by a working set the run genuinely
 * needs, not by waste.
 *
 * So the modules are made fewer instead. A batch of blocks that were published
 * singly is lowered AGAIN, this time into one module holding all of them, and
 * each block's existing table entry is pointed at the new module's matching
 * export. The entry's value never changes, so the block cache, the compiled
 * calls between blocks and anything else already holding that address keep
 * working; only the module behind it changes. The single modules are then
 * released. The engine compiles the batch once, in place of the many separate
 * compilations it would otherwise be asked for later.
 *
 * WHAT THIS FILE OWNS AND WHAT IT DOES NOT. It owns the operation: lower the
 * batch, publish it, move the entries, release what it replaced, and refuse as
 * a whole if any part of that cannot be done. It does not choose which blocks
 * to compact or when -- that is the storage's, which knows what is live.
 */
#ifndef X86PORT_JIT_WASM_COMPACT_H
#define X86PORT_JIT_WASM_COMPACT_H

#include "jit_wasm_arena.h"
#include "jit_wasm_chain.h"
#include "jit_wasm_leaf.h"
#include "jit_x64.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One block to be moved. `guest` and `guest_len` are what it was translated
 * from and covered; `token` is the module it is published in now; `entry` is
 * the address it is entered at, which must survive. `chain_first` and
 * `chain_exits` are the chain slots it was published with, which its links
 * name and which the rebuilt body reuses (jit_wasm_chain.h). `leaf_site` is
 * the leaf site its CALL was published with, which the rebuilt CALL reads
 * again (jit_wasm_leaf.h), or NULL.
 */
typedef struct X86pWasmCompactBlock {
  uint32_t guest;
  uint32_t guest_len;
  int token;
  unsigned chain_exits;
  void *entry;
  int64_t chain_first;
  struct X86pJitLeafSite *leaf_site;
} X86pWasmCompactBlock;

/*
 * What happened. `token` is the module the blocks now share, or -1 when
 * nothing moved; `bytes` is that module's length.
 */
typedef struct X86pWasmCompactResult {
  int token;
  size_t bytes;
  unsigned moved;
  /* Exits in the module that call a block of it directly (jit_wasm_chain.h). */
  unsigned direct_exits;
} X86pWasmCompactResult;

/*
 * Lower `count` blocks, at the guest addresses in `eips`, into ONE module in
 * `code`, and return the module's length in bytes -- or 0 with `reason` set.
 *
 * WHY THIS EXISTS. Publishing one module per block makes the engine hold one
 * permanent object per translated block, and a browser has a limit on those:
 * measured in Firefox 156, instantiation was refused at about 16,350 live
 * modules, which a real title reaches in seconds. The module format has always
 * allowed up to X86P_WASM_MAX_BODIES bodies; this is the call that uses them,
 * so the same guest coverage costs a fraction of the modules.
 *
 * `out` must have room for `count` blocks. Block i is entered through the
 * export x86p_wasm_module_export_name() gives for body i -- NOT body 0 -- and
 * its `entry` is left for the caller to fill from the arena, because only the
 * arena knows the module's token. `host_bytes` is zeroed for the same reason
 * it is set for a single block: the module's bytes belong to the module, and
 * charging its whole length to each of its blocks would count it `count`
 * times.
 *
 * `chains` is NULL, or `count` chain uses: block i's exits chain as
 * chains[i] says (jit_wasm_chain.h). `leaves` is NULL, or `count` leaf uses:
 * block i's CALL finds its leaf as leaves[i] says (jit_wasm_leaf.h).
 *
 * All or nothing: if any block cannot be lowered the module is abandoned and 0
 * is returned, because the sections have already promised every body.
 */
size_t x86p_jit_translate_batch(const X86pMem *mem,
                                const uint32_t *eips,
                                unsigned count,
                                void *code,
                                size_t code_cap,
                                X86pJitBoundaryFn boundary,
                                void *boundary_user,
                                const X86pWasmChainUse *chains,
                                const X86pWasmLeafUse *leaves,
                                X86pJitBlock *out,
                                char *reason,
                                unsigned reason_len);

/*
 * Rebuild `count` singly-published blocks as one module.
 *
 * Refuses, changing nothing, when the host cannot move an entry, when a block
 * no longer lowers to the same guest extent and chain slots it was published
 * with -- which
 * means the guest code changed under it and the block should be invalidated
 * rather than rebuilt -- or when the module cannot be published or an entry
 * cannot be moved. A refusal leaves every block exactly where it was, still
 * entered through the same address, so a caller may simply carry on.
 *
 * On success the caller owns `out->token`, must account for `out->bytes`, and
 * must stop treating the old tokens as live: they have been released here.
 * `env` is what the blocks were published in, or NULL: its boundary, the
 * slot table their exits claimed and the leaf resolver their CALLs asked.
 */
int x86p_wasm_compact(X86pWasmArena *arena,
                      const X86pMem *mem,
                      void *buffer,
                      size_t buffer_bytes,
                      const X86pJitTranslateEnv *env,
                      const X86pWasmCompactBlock *blocks,
                      unsigned count,
                      X86pWasmCompactResult *out,
                      char *reason,
                      unsigned reason_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_COMPACT_H */
