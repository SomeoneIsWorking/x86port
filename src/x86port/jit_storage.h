#ifndef X86PORT_JIT_STORAGE_H
#define X86PORT_JIT_STORAGE_H

#include "jit_x64.h"

/* Published block lifetime, independent of whether publication protects code
 * pages or instantiates a WebAssembly module. The engine must remove cache
 * references before resetting or invalidating this owner. */
typedef struct X86pJitStorage X86pJitStorage;

const char *x86p_jit_storage_mechanism(void);
size_t x86p_jit_storage_min_capacity(void);
/*
 * `capacity` bounds the BYTES of live translated code; `max_blocks` bounds how
 * many translations may be live at once. On a machine-code host the second is
 * implied by the first, because blocks share one region and are reclaimed in
 * bulk. On the WebAssembly host they are separate resources -- each block is
 * its own module with its own fixed cost in the engine -- and leaving the
 * block cap implicit is what let a caller's cache be eight times larger than
 * the storage behind it.
 */
X86pJitStorage *x86p_jit_storage_create(size_t capacity, size_t max_blocks, char *reason, unsigned reason_len);
void x86p_jit_storage_destroy(X86pJitStorage *storage);
int x86p_jit_storage_has_room(const X86pJitStorage *storage);
size_t x86p_jit_storage_used(const X86pJitStorage *storage);
/* The two limits this storage was created with, so a report of what it HOLDS
   has the denominators that say which one an eviction hit. */
size_t x86p_jit_storage_capacity(const X86pJitStorage *storage);
unsigned x86p_jit_storage_block_records(const X86pJitStorage *storage);
/* Told for each block an eviction drops, before its exec address can be
   handed out again, so the caller can forget its own record of that block. */
typedef void (*X86pJitStorageDropFn)(void *user, uint32_t lo, uint32_t hi);
/*
 * Free space for another block, in whatever unit this storage actually frees.
 *
 * That unit is NOT always one block: a storage that puts several blocks in one
 * engine module frees nothing until the last of them goes, so dropping one
 * block at a time and giving up when the used figure did not move rewinds the
 * whole cache over and over -- measured in a browser run as 94 full flushes
 * and a working set that collapsed to two blocks. The storage therefore owns
 * the choice of what to drop, and reports how many blocks that cost.
 *
 * Returns 0 when it has nothing left to drop, which means the caller has to
 * rewind the whole cache instead.
 */
unsigned x86p_jit_storage_evict(X86pJitStorage *storage, X86pJitStorageDropFn drop, void *user);
/*
 * How many batches of singly-published blocks this storage has rebuilt as one
 * module, and how many it left alone.
 *
 * Published because the whole point is a number that cannot be seen any other
 * way: a run whose compactions stay at zero is holding one engine module per
 * translated block, which is the shape that hits a browser's module limit, and
 * it looks identical from the outside to a run that is compacting perfectly.
 */
unsigned x86p_jit_storage_compactions(const X86pJitStorage *storage);
unsigned x86p_jit_storage_compaction_refusals(const X86pJitStorage *storage);
/*
 * How many published blocks are waiting for a batch to fill, and whether this
 * storage has stopped compacting altogether.
 *
 * Both exist because a compaction count that stops rising says only that; it
 * does not say whether the batches stopped filling or the storage gave up, and
 * those are different defects.
 */
unsigned x86p_jit_storage_compaction_pending(const X86pJitStorage *storage);
int x86p_jit_storage_compaction_stopped(const X86pJitStorage *storage);

void x86p_jit_storage_reset(X86pJitStorage *storage);
void x86p_jit_storage_invalidate(X86pJitStorage *storage, uint32_t lo, uint32_t hi);
X86pJitStatus x86p_jit_storage_translate(X86pJitStorage *storage,
                                         const X86pMem *mem,
                                         uint32_t eip,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitBlock *block,
                                         char *reason,
                                         unsigned reason_len);

#endif
