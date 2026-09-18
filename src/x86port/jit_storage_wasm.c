#include "jit_storage.h"

#include "jit_wasm.h"
#include "jit_wasm_host.h"
#include "jit_wasm_lower.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct X86pWasmStoredBlock {
  uint32_t guest;
  uint32_t guest_len;
  size_t bytes;
} X86pWasmStoredBlock;

struct X86pJitStorage {
  X86pWasmArena arena;
  X86pWasmStoredBlock *blocks; /* `capacity_blocks` entries, owned */
  unsigned capacity_blocks;
  /*
   * ONE block is lowered at a time, so the scratch buffer holds ONE worst-case
   * module -- it used to be allocated at the whole byte budget, which tied the
   * amount of live translated code a consumer may hold to the size of a buffer
   * that is overwritten on every translation. Raising the budget then cost
   * real memory per guest thread for nothing.
   */
  unsigned char *buffer;
  size_t buffer_bytes;
  size_t byte_budget; /* how many bytes of LIVE module may be held at once */
  size_t used;
  unsigned next_victim;
};

const char *x86p_jit_storage_mechanism(void) {
  return "WebAssembly modules with owned indirect-table entries";
}

size_t x86p_jit_storage_min_capacity(void) {
  return X86P_WASM_MIN_MODULE_BYTES;
}

X86pJitStorage *x86p_jit_storage_create(size_t capacity, size_t max_blocks, char *reason, unsigned reason_len) {
  X86pJitStorage *storage;
  X86pWasmHost host;
  if (max_blocks == 0u || max_blocks > UINT_MAX) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "a live-block cap of %zu is not a usable number of modules", max_blocks);
    }
    return NULL;
  }
  storage = calloc(1u, sizeof *storage);
  if (!storage) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory creating WebAssembly storage");
    }
    return NULL;
  }
  storage->blocks = calloc(max_blocks, sizeof *storage->blocks);
  storage->buffer_bytes = X86P_WASM_MAX_MODULE_BYTES;
  storage->buffer = malloc(storage->buffer_bytes);
  if (!storage->blocks || !storage->buffer) {
    if (reason && reason_len) {
      snprintf(reason,
               reason_len,
               "out of memory: %zu block record(s) plus a %zu-byte module buffer",
               max_blocks,
               storage->buffer_bytes);
    }
    free(storage->blocks);
    free(storage->buffer);
    free(storage);
    return NULL;
  }
  if (!x86p_wasm_host_create(&host, reason, reason_len)) {
    free(storage->blocks);
    free(storage->buffer);
    free(storage);
    return NULL;
  }
  if (!x86p_wasm_arena_init(&storage->arena, &host, (unsigned)max_blocks)) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory allocating %zu module slot(s)", max_blocks);
    }
    x86p_wasm_host_destroy(&host);
    free(storage->blocks);
    free(storage->buffer);
    free(storage);
    return NULL;
  }
  storage->capacity_blocks = (unsigned)max_blocks;
  storage->byte_budget = capacity;
  return storage;
}

void x86p_jit_storage_reset(X86pJitStorage *storage) {
  x86p_wasm_arena_release_all(&storage->arena);
  memset(storage->blocks, 0, (size_t)storage->capacity_blocks * sizeof *storage->blocks);
  storage->used = 0u;
  storage->next_victim = 0u;
}

void x86p_jit_storage_destroy(X86pJitStorage *storage) {
  if (storage) {
    X86pWasmHost host = storage->arena.host;
    x86p_wasm_arena_dispose(&storage->arena);
    x86p_wasm_host_destroy(&host);
    free(storage->blocks);
    free(storage->buffer);
    free(storage);
  }
}

int x86p_jit_storage_has_room(const X86pJitStorage *storage) {
  return storage->byte_budget - storage->used >= X86P_WASM_MIN_MODULE_BYTES &&
         x86p_wasm_arena_live(&storage->arena) < storage->capacity_blocks;
}

size_t x86p_jit_storage_used(const X86pJitStorage *storage) {
  return storage->used;
}

int x86p_jit_storage_victim(X86pJitStorage *storage, uint32_t *lo, uint32_t *hi) {
  unsigned scanned;
  for (scanned = 0u; scanned < storage->capacity_blocks; ++scanned) {
    unsigned token = storage->next_victim;
    const X86pWasmStoredBlock *block = &storage->blocks[token];
    uint64_t end = (uint64_t)block->guest + block->guest_len;
    storage->next_victim = (token + 1u) % storage->capacity_blocks;
    if (!block->bytes) {
      continue;
    }
    if (end > UINT32_MAX) {
      return 0;
    }
    *lo = block->guest;
    *hi = (uint32_t)end;
    return 1;
  }
  return 0;
}

void x86p_jit_storage_invalidate(X86pJitStorage *storage, uint32_t lo, uint32_t hi) {
  unsigned i;
  if (lo >= hi) {
    return;
  }
  for (i = 0; i < storage->capacity_blocks; ++i) {
    X86pWasmStoredBlock *block = &storage->blocks[i];
    if (block->bytes && block->guest < hi && (uint64_t)block->guest + block->guest_len > lo) {
      x86p_wasm_arena_release(&storage->arena, (int)i);
      storage->used -= block->bytes;
      memset(block, 0, sizeof *block);
    }
  }
}

X86pJitStatus x86p_jit_storage_translate(X86pJitStorage *storage,
                                         const X86pMem *mem,
                                         uint32_t eip,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitBlock *block,
                                         char *reason,
                                         unsigned reason_len) {
  size_t remaining = storage->byte_budget - storage->used;
  size_t room = remaining < storage->buffer_bytes ? remaining : storage->buffer_bytes;
  X86pJitStatus status =
      x86p_jit_translate_bounded(mem, eip, storage->buffer, room, boundary, boundary_user, block, reason, reason_len);
  int token;
  if (status != kX86pJitOk) {
    return status;
  }
  token = x86p_jit_wasm_publish(&storage->arena, block, storage->buffer, block->host_bytes, reason, reason_len);
  if (token < 0) {
    const char *host_error = x86p_wasm_host_error(&storage->arena.host);
    if (host_error[0] && reason && reason_len) {
      snprintf(reason, reason_len, "WebAssembly publication: %s", host_error);
    }
    return kX86pJitOutOfSpace;
  }
  storage->blocks[token] = (X86pWasmStoredBlock){block->guest_eip, block->guest_len, block->host_bytes};
  storage->used += block->host_bytes;
  return kX86pJitOk;
}
