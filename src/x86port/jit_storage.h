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
/* Return one reclaimable block range on hosts that can release translations
   independently. A zero means the storage requires a whole-cache rewind. */
int x86p_jit_storage_victim(X86pJitStorage *storage, uint32_t *lo, uint32_t *hi);
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
