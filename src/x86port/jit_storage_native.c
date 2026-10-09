#include "jit_storage.h"

#include "code_memory.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Machine code is written at a bump pointer into chunks of code memory, made
 * as the working set reaches them, so memory follows the code actually
 * translated and `capacity` is only the ceiling. Blocks never move: emitted
 * code calls helpers and transfers between blocks through registers, so two
 * chunks may lie any distance apart.
 */
enum {
  kChunkMaxBytes = 32u << 20,
  kChunksPerCapacity = 16u,
  /* A chunk with less left than this is retired, so blocks are not cut short
     at its end; smaller chunks retire at an eighth. */
  kChunkMaxSlack = 64u << 10
};

struct X86pJitStorage {
  JcCodeRegion *chunks;
  size_t chunk_count;   /* made so far */
  size_t chunk_limit;   /* chunks `capacity` allows */
  size_t chunk_bytes;   /* each chunk's size */
  size_t chunk_slack;   /* left in a chunk below which the next one is used */
  size_t current;       /* the chunk being written */
  size_t used;          /* bytes written into the current chunk */
  size_t retired_bytes; /* bytes written into chunks before the current one */
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
  /* Machine-code blocks share chunks and are reclaimed in bulk, so the block
     count is bounded by the bytes and there is no second resource to size.
     Named here rather than silently ignored. */
  (void)max_blocks;
  storage = calloc(1u, sizeof *storage);
  if (!storage) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory creating JIT storage");
    }
    return NULL;
  }
  storage->chunk_bytes = capacity / kChunksPerCapacity;
  if (storage->chunk_bytes < X86P_JIT_MIN_BLOCK_BYTES) {
    storage->chunk_bytes = capacity < X86P_JIT_MIN_BLOCK_BYTES ? capacity : X86P_JIT_MIN_BLOCK_BYTES;
  }
  if (storage->chunk_bytes > kChunkMaxBytes) {
    storage->chunk_bytes = kChunkMaxBytes;
  }
  storage->chunk_slack = storage->chunk_bytes / 8u < kChunkMaxSlack ? storage->chunk_bytes / 8u : kChunkMaxSlack;
  if (storage->chunk_slack < X86P_JIT_MIN_BLOCK_BYTES) {
    storage->chunk_slack = X86P_JIT_MIN_BLOCK_BYTES;
  }
  storage->chunk_limit = storage->chunk_bytes ? (capacity + storage->chunk_bytes - 1u) / storage->chunk_bytes : 0u;
  storage->chunks = calloc(storage->chunk_limit ? storage->chunk_limit : 1u, sizeof *storage->chunks);
  if (!storage->chunks) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory creating JIT storage");
    }
    free(storage);
    return NULL;
  }
  if (jc_code_region_create(storage->chunk_bytes, &storage->chunks[0], reason, reason_len) != kJcCodeOk) {
    free(storage->chunks);
    free(storage);
    return NULL;
  }
  storage->chunk_count = 1u;
  storage->capacity = capacity;
  return storage;
}

void x86p_jit_storage_destroy(X86pJitStorage *storage) {
  size_t i;
  if (storage) {
    for (i = 0; i < storage->chunk_count; i++) {
      jc_code_region_destroy(&storage->chunks[i]);
    }
    free(storage->chunks);
    free(storage);
  }
}

static size_t chunk_left(const X86pJitStorage *storage) {
  return storage->chunks[storage->current].size - storage->used;
}

X86pJitStorageRoom x86p_jit_storage_room(const X86pJitStorage *storage) {
  /* One limit: bytes. There is no module slot and no engine object to run out
     of here. */
  return chunk_left(storage) >= X86P_JIT_MIN_BLOCK_BYTES || storage->current + 1u < storage->chunk_limit
             ? kX86pJitStorageRoom
             : kX86pJitStorageOutOfBytes;
}

int x86p_jit_storage_has_room(const X86pJitStorage *storage) {
  return x86p_jit_storage_room(storage) == kX86pJitStorageRoom;
}

size_t x86p_jit_storage_used(const X86pJitStorage *storage) {
  return storage->retired_bytes + storage->used;
}

size_t x86p_jit_storage_chunks(const X86pJitStorage *storage) {
  return storage->chunk_count;
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
  /* Machine code is written at a bump pointer, so there is no block whose
     bytes can be handed back on their own. Zero is the answer, not a stub: the
     caller rewinds the whole arena instead. */
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

uint64_t x86p_jit_storage_direct_exits(const X86pJitStorage *storage) {
  /* Machine code jumps straight to a linked block already. */
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

const char *x86p_jit_storage_compaction_refusal_reason(const X86pJitStorage *storage) {
  (void)storage;
  return "";
}

int x86p_jit_storage_compaction_stopped(const X86pJitStorage *storage) {
  (void)storage;
  return 0;
}

unsigned x86p_jit_storage_ceilings_learned(const X86pJitStorage *storage) {
  (void)storage;
  return 0u;
}

unsigned x86p_jit_storage_ceilings_retired(const X86pJitStorage *storage) {
  (void)storage;
  return 0u;
}

/* Chunks stay mapped and are written again from the first, so a reset never
   unmaps code a host call may still return into. */
void x86p_jit_storage_reset(X86pJitStorage *storage) {
  storage->current = 0u;
  storage->used = 0u;
  storage->retired_bytes = 0u;
}

/* Move to the next chunk when the current one is nearly full, making it the
   first time the working set reaches it. */
static X86pJitStatus ensure_chunk(X86pJitStorage *storage, char *reason, unsigned reason_len) {
  if (chunk_left(storage) >= storage->chunk_slack || storage->current + 1u >= storage->chunk_limit) {
    return kX86pJitOk;
  }
  if (storage->current + 1u == storage->chunk_count) {
    if (jc_code_region_create(storage->chunk_bytes, &storage->chunks[storage->chunk_count], reason, reason_len) !=
        kJcCodeOk) {
      return kX86pJitOutOfSpace;
    }
    storage->chunk_count++;
  }
  storage->retired_bytes += storage->used;
  storage->current++;
  storage->used = 0u;
  return kX86pJitOk;
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
                                         const X86pJitTranslateEnv *env,
                                         X86pJitBlock *block,
                                         char *reason,
                                         unsigned reason_len) {
  X86pJitStatus status;
  JcCodeStatus published;
  JcCodeRegion *code;
  status = ensure_chunk(storage, reason, reason_len);
  if (status != kX86pJitOk) {
    return status;
  }
  code = &storage->chunks[storage->current];
  if (jc_code_begin_write(code) != kJcCodeOk) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "code memory (%s) refused a write window", jc_code_mechanism());
    }
    return kX86pJitOutOfSpace;
  }
  status = x86p_jit_translate_bounded(
      mem, eip, code->write + storage->used, code->size - storage->used, env, block, reason, reason_len);
  /* Close the write window on failure too: no partially translated block may
   * leave executable storage writable. */
  published = jc_code_publish_range(code, storage->used, status == kX86pJitOk ? block->host_bytes : 0u);
  if (published != kJcCodeOk) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "code memory (%s) refused publication", jc_code_mechanism());
    }
    return kX86pJitOutOfSpace;
  }
  if (status == kX86pJitOk) {
    block->entry = code->exec + storage->used;
    storage->used += block->host_bytes;
  }
  return status;
}
