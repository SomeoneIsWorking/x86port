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
