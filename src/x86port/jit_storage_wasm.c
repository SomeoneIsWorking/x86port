#include "jit_storage.h"

#include "jit_wasm.h"
#include "jit_wasm_host.h"
#include "jit_wasm_lower.h"

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
  X86pWasmStoredBlock blocks[X86P_WASM_MAX_LIVE_MODULES];
  unsigned char *buffer;
  size_t capacity;
  size_t used;
  unsigned next_victim;
};

const char *x86p_jit_storage_mechanism(void) {
  return "WebAssembly modules with owned indirect-table entries";
}

size_t x86p_jit_storage_min_capacity(void) {
  return X86P_WASM_MIN_MODULE_BYTES;
}

X86pJitStorage *x86p_jit_storage_create(size_t capacity, char *reason, unsigned reason_len) {
  X86pJitStorage *storage = calloc(1u, sizeof *storage);
  X86pWasmHost host;
  if (!storage) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory creating WebAssembly storage");
    }
    return NULL;
  }
  storage->buffer = malloc(capacity);
  if (!storage->buffer) {
    if (reason && reason_len) {
      snprintf(reason, reason_len, "out of memory allocating %zu-byte module buffer", capacity);
    }
    free(storage);
    return NULL;
  }
  if (!x86p_wasm_host_create(&host, reason, reason_len)) {
    free(storage->buffer);
    free(storage);
    return NULL;
  }
  storage->capacity = capacity;
  x86p_wasm_arena_init(&storage->arena, &host);
  return storage;
}

void x86p_jit_storage_reset(X86pJitStorage *storage) {
  x86p_wasm_arena_release_all(&storage->arena);
  memset(storage->blocks, 0, sizeof storage->blocks);
  storage->used = 0u;
  storage->next_victim = 0u;
}

void x86p_jit_storage_destroy(X86pJitStorage *storage) {
  if (storage) {
    x86p_jit_storage_reset(storage);
    x86p_wasm_host_destroy(&storage->arena.host);
    free(storage->buffer);
    free(storage);
  }
}

int x86p_jit_storage_has_room(const X86pJitStorage *storage) {
  return storage->capacity - storage->used >= X86P_WASM_MIN_MODULE_BYTES &&
         x86p_wasm_arena_live(&storage->arena) < X86P_WASM_MAX_LIVE_MODULES;
}

size_t x86p_jit_storage_used(const X86pJitStorage *storage) {
  return storage->used;
}

int x86p_jit_storage_victim(X86pJitStorage *storage, uint32_t *lo, uint32_t *hi) {
  unsigned scanned;
  for (scanned = 0u; scanned < X86P_WASM_MAX_LIVE_MODULES; ++scanned) {
    unsigned token = storage->next_victim;
    const X86pWasmStoredBlock *block = &storage->blocks[token];
    uint64_t end = (uint64_t)block->guest + block->guest_len;
    storage->next_victim = (token + 1u) % X86P_WASM_MAX_LIVE_MODULES;
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
  for (i = 0; i < X86P_WASM_MAX_LIVE_MODULES; ++i) {
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
  X86pJitStatus status = x86p_jit_translate_bounded(
      mem, eip, storage->buffer, storage->capacity - storage->used, boundary, boundary_user, block, reason, reason_len);
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
