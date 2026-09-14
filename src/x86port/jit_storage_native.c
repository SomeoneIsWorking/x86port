#include "jit_storage.h"

#include "code_memory.h"

#include <stdio.h>
#include <stdlib.h>

struct X86pJitStorage {
  JcCodeRegion code;
  size_t used;
};

const char *x86p_jit_storage_mechanism(void) {
  return jc_code_mechanism();
}

size_t x86p_jit_storage_min_capacity(void) {
  return X86P_JIT_MIN_BLOCK_BYTES;
}

X86pJitStorage *x86p_jit_storage_create(size_t capacity, char *reason, unsigned reason_len) {
  X86pJitStorage *storage = calloc(1u, sizeof *storage);
  if (!storage) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory creating JIT storage");
    }
    return NULL;
  }
  if (jc_code_region_create(capacity, &storage->code, reason, reason_len) != kJcCodeOk) {
    free(storage);
    return NULL;
  }
  return storage;
}

void x86p_jit_storage_destroy(X86pJitStorage *storage) {
  if (storage) {
    jc_code_region_destroy(&storage->code);
    free(storage);
  }
}

int x86p_jit_storage_has_room(const X86pJitStorage *storage) {
  return storage->code.size - storage->used >= X86P_JIT_MIN_BLOCK_BYTES;
}

size_t x86p_jit_storage_used(const X86pJitStorage *storage) {
  return storage->used;
}

int x86p_jit_storage_victim(X86pJitStorage *storage, uint32_t *lo, uint32_t *hi) {
  (void)storage;
  (void)lo;
  (void)hi;
  return 0;
}

void x86p_jit_storage_reset(X86pJitStorage *storage) {
  storage->used = 0u;
}

int x86p_jit_storage_unit_range(X86pJitStorage *storage, uint32_t address, uint32_t *lo, uint32_t *hi) {
  (void)storage;
  (void)address;
  (void)lo;
  (void)hi;
  /* One block per unit here, so the caller's own range is already exact. */
  return 0;
}

int x86p_jit_storage_invalidate_unit(
    X86pJitStorage *storage, uint32_t lo, uint32_t hi, uint32_t *lo_out, uint32_t *hi_out) {
  (void)storage;
  (void)lo;
  (void)hi;
  (void)lo_out;
  (void)hi_out;
  return 0;
}

void x86p_jit_storage_invalidate(X86pJitStorage *storage, uint32_t lo, uint32_t hi) {
  /* Native code is reclaimed in bulk when its region fills. */
  (void)storage;
  (void)lo;
  (void)hi;
}

X86pJitStatus x86p_jit_storage_translate_chain(X86pJitStorage *storage,
                                               const X86pMem *mem,
                                               uint32_t eip,
                                               X86pJitBoundaryFn boundary,
                                               void *boundary_user,
                                               X86pJitBlock *blocks,
                                               unsigned max_blocks,
                                               unsigned *count,
                                               char *reason,
                                               unsigned reason_len) {
  X86pJitStatus status;
  (void)max_blocks;
  if (!blocks || !count || max_blocks == 0u) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "no room for a block");
    }
    return kX86pJitOutOfSpace;
  }
  *count = 0u;
  /* Machine code is published per block and the price does not depend on how
     many blocks a run holds, so there is nothing here to batch. */
  status = x86p_jit_storage_translate(storage, mem, eip, boundary, boundary_user, blocks, reason, reason_len);
  if (status == kX86pJitOk) {
    *count = 1u;
  }
  return status;
}

X86pJitStatus x86p_jit_storage_translate(X86pJitStorage *storage,
                                         const X86pMem *mem,
                                         uint32_t eip,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitBlock *block,
                                         char *reason,
                                         unsigned reason_len) {
  X86pJitStatus status;
  JcCodeStatus published;
  if (jc_code_begin_write(&storage->code) != kJcCodeOk) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "code memory (%s) refused a write window", jc_code_mechanism());
    }
    return kX86pJitOutOfSpace;
  }
  status = x86p_jit_translate_bounded(mem,
                                      eip,
                                      storage->code.write + storage->used,
                                      storage->code.size - storage->used,
                                      boundary,
                                      boundary_user,
                                      block,
                                      reason,
                                      reason_len);
  /* Close the write window on failure too: no partially translated block may
   * leave executable storage writable. */
  published = jc_code_publish_range(&storage->code, storage->used, status == kX86pJitOk ? block->host_bytes : 0u);
  if (published != kJcCodeOk) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "code memory (%s) refused publication", jc_code_mechanism());
    }
    return kX86pJitOutOfSpace;
  }
  if (status == kX86pJitOk) {
    block->entry = storage->code.exec + storage->used;
    storage->used += block->host_bytes;
  }
  return status;
}
