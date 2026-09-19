#include "jit_storage.h"

#include "code_memory.h"

#include <stdio.h>
#include <stdlib.h>

struct X86pJitStorage {
  JcCodeRegion code;
  size_t used;
  size_t capacity;
};

const char *x86p_jit_storage_mechanism(void) {
  return jc_code_mechanism();
}

size_t x86p_jit_storage_min_capacity(void) {
  return X86P_JIT_MIN_BLOCK_BYTES;
}

X86pJitStorage *x86p_jit_storage_create(size_t capacity, size_t max_blocks, char *reason, unsigned reason_len) {
  X86pJitStorage *storage;
  /* Machine-code blocks share one region and are reclaimed in bulk, so the
     block count is bounded by the bytes and there is no second resource to
     size. Named here rather than silently ignored. */
  (void)max_blocks;
  storage = calloc(1u, sizeof *storage);
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
  storage->capacity = capacity;
  return storage;
}

void x86p_jit_storage_destroy(X86pJitStorage *storage) {
  if (storage) {
    jc_code_region_destroy(&storage->code);
    free(storage);
  }
}

X86pJitStorageRoom x86p_jit_storage_room(const X86pJitStorage *storage) {
  /* One region, one limit: bytes. There is no module slot and no engine object
     to run out of here. */
  return storage->code.size - storage->used >= X86P_JIT_MIN_BLOCK_BYTES ? kX86pJitStorageRoom
                                                                        : kX86pJitStorageOutOfBytes;
}

int x86p_jit_storage_has_room(const X86pJitStorage *storage) {
  return x86p_jit_storage_room(storage) == kX86pJitStorageRoom;
}

size_t x86p_jit_storage_used(const X86pJitStorage *storage) {
  return storage->used;
}

size_t x86p_jit_storage_capacity(const X86pJitStorage *storage) {
  return storage->capacity;
}

unsigned x86p_jit_storage_block_records(const X86pJitStorage *storage) {
  /* Blocks here are bounded by the bytes, not by a record of their own, and
     saying 0 keeps a report from printing a cap this storage does not have. */
  (void)storage;
  return 0u;
}

unsigned x86p_jit_storage_evict(X86pJitStorage *storage, X86pJitStorageDropFn drop, void *user) {
  /* Machine code is written into one arena at a bump pointer, so there is no
     block in it whose bytes can be handed back on their own. Zero is the
     answer, not a stub: the caller rewinds the whole arena instead. */
  (void)storage;
  (void)drop;
  (void)user;
  return 0u;
}

unsigned x86p_jit_storage_compactions(const X86pJitStorage *storage) {
  /* Machine code is written into one arena and needs no per-block engine
     object, so there is nothing here to make fewer of. Zero is the true
     answer, not a stub: a caller reporting it beside a wasm run's figure is
     comparing two different resources. */
  (void)storage;
  return 0u;
}

unsigned x86p_jit_storage_compaction_refusals(const X86pJitStorage *storage) {
  (void)storage;
  return 0u;
}

unsigned x86p_jit_storage_compaction_pending(const X86pJitStorage *storage) {
  (void)storage;
  return 0u;
}

int x86p_jit_storage_compaction_stopped(const X86pJitStorage *storage) {
  (void)storage;
  return 0;
}

void x86p_jit_storage_reset(X86pJitStorage *storage) {
  storage->used = 0u;
}

void x86p_jit_storage_invalidate(X86pJitStorage *storage, uint32_t lo, uint32_t hi) {
  /* Native code is reclaimed in bulk when its region fills. */
  (void)storage;
  (void)lo;
  (void)hi;
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
