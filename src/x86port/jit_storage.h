#ifndef X86PORT_JIT_STORAGE_H
#define X86PORT_JIT_STORAGE_H

#include "jit_x64.h"

/* Published block lifetime, independent of whether publication protects code
 * pages or instantiates a WebAssembly module. The engine must remove cache
 * references before resetting or invalidating this owner. */
typedef struct X86pJitStorage X86pJitStorage;

const char *x86p_jit_storage_mechanism(void);
size_t x86p_jit_storage_min_capacity(void);
X86pJitStorage *x86p_jit_storage_create(size_t capacity, char *reason, unsigned reason_len);
void x86p_jit_storage_destroy(X86pJitStorage *storage);
int x86p_jit_storage_has_room(const X86pJitStorage *storage);
size_t x86p_jit_storage_used(const X86pJitStorage *storage);
/* Return one reclaimable block range on hosts that can release translations
   independently. A zero means the storage requires a whole-cache rewind. */
int x86p_jit_storage_victim(X86pJitStorage *storage, uint32_t *lo, uint32_t *hi);
void x86p_jit_storage_reset(X86pJitStorage *storage);
void x86p_jit_storage_invalidate(X86pJitStorage *storage, uint32_t lo, uint32_t hi);
/*
 * Translate the block at `eip`, and the run of blocks that follows it on the
 * host's own terms, and make every one of them enterable. `*count` says how
 * many were written to `blocks` (at least one on success, never more than
 * `max_blocks`).
 *
 * Batching is a WASM-HOST AMORTIZATION and nothing else: publishing there costs
 * a fixed ~0.5 ms per module whatever its size, so a host that pays per module
 * pays it once for a run instead of once per block. The machine-code host has
 * nothing to amortize and writes exactly one block, which is why this returns
 * the chain rather than promising a length.
 */
/*
 * The guest range of the published unit covering `address`, if there is one.
 *
 * A unit is one module, which may hold several blocks, so a consumer that drops
 * its own record of one address has to drop every address that shares the
 * module's lifetime. Returns 0 when no unit covers the address, in which case
 * the caller's own range is already the whole story.
 */
int x86p_jit_storage_unit_range(X86pJitStorage *storage, uint32_t address, uint32_t *lo, uint32_t *hi);

/*
 * Invalidate everything that shares a published unit with [lo, hi).
 *
 * `lo_out`/`hi_out` receive the range the storage actually released, which may
 * be WIDER than the range asked for: a module holding several blocks is released
 * as one, and the caller must drop its own records for every address in it
 * rather than only for the address that was written. Returns 0 when the storage
 * released nothing individually (it reclaims in bulk), in which case the
 * caller's range stands.
 */
int x86p_jit_storage_invalidate_unit(
    X86pJitStorage *storage, uint32_t lo, uint32_t hi, uint32_t *lo_out, uint32_t *hi_out);

X86pJitStatus x86p_jit_storage_translate_chain(X86pJitStorage *storage,
                                               const X86pMem *mem,
                                               uint32_t eip,
                                               X86pJitBoundaryFn boundary,
                                               void *boundary_user,
                                               X86pJitBlock *blocks,
                                               unsigned max_blocks,
                                               unsigned *count,
                                               char *reason,
                                               unsigned reason_len);

X86pJitStatus x86p_jit_storage_translate(X86pJitStorage *storage,
                                         const X86pMem *mem,
                                         uint32_t eip,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitBlock *block,
                                         char *reason,
                                         unsigned reason_len);

#endif
