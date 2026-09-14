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

int x86p_jit_storage_unit_range(X86pJitStorage *storage, uint32_t address, uint32_t *lo, uint32_t *hi) {
  unsigned i;
  if (!storage || !lo || !hi) {
    return 0;
  }
  for (i = 0; i < X86P_WASM_MAX_LIVE_MODULES; ++i) {
    const X86pWasmStoredBlock *block = &storage->blocks[i];
    if (block->bytes && address >= block->guest && (uint64_t)address < (uint64_t)block->guest + block->guest_len) {
      *lo = block->guest;
      *hi = block->guest + block->guest_len;
      return 1;
    }
  }
  return 0;
}

int x86p_jit_storage_invalidate_unit(
    X86pJitStorage *storage, uint32_t lo, uint32_t hi, uint32_t *lo_out, uint32_t *hi_out) {
  uint32_t wide_lo = lo;
  uint32_t wide_hi = hi;
  unsigned i;
  int released = 0;
  if (!storage || lo >= hi) {
    return 0;
  }
  /* Widen to every module the range touches, and widen again while the union has
     grown: one module can reach past another's end, and a caller told only about
     the first would keep a stale entry for the rest. */
  for (i = 0; i < 8u; ++i) {
    uint32_t before = wide_lo;
    uint32_t before_hi = wide_hi;
    uint32_t w;
    for (w = 0; w < X86P_WASM_MAX_LIVE_MODULES; ++w) {
      const X86pWasmStoredBlock *block = &storage->blocks[w];
      uint64_t end;
      if (!block->bytes) {
        continue;
      }
      end = (uint64_t)block->guest + block->guest_len;
      if (block->guest < wide_hi && end > wide_lo) {
        if (block->guest < wide_lo) {
          wide_lo = block->guest;
        }
        if (end > wide_hi && end <= UINT32_MAX) {
          wide_hi = (uint32_t)end;
        }
      }
    }
    if (wide_lo == before && wide_hi == before_hi) {
      break;
    }
  }
  for (i = 0; i < X86P_WASM_MAX_LIVE_MODULES; ++i) {
    X86pWasmStoredBlock *block = &storage->blocks[i];
    if (block->bytes && block->guest < wide_hi && (uint64_t)block->guest + block->guest_len > wide_lo) {
      x86p_wasm_arena_release(&storage->arena, (int)i);
      storage->used -= block->bytes;
      memset(block, 0, sizeof *block);
      released = 1;
    }
  }
  if (released && lo_out && hi_out) {
    *lo_out = wide_lo;
    *hi_out = wide_hi;
  }
  return released;
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
  X86pJitBlock first;
  unsigned run = 0u;
  unsigned i;
  size_t module_bytes = 0u;
  X86pJitStatus status;

  if (!storage || !blocks || !count || max_blocks == 0u) {
    snprintf(reason, reason_len, "no room for a block");
    return kX86pJitOutOfSpace;
  }
  *count = 0u;
  if (!x86p_jit_storage_has_room(storage)) {
    snprintf(reason, reason_len, "the storage has no room for another module");
    return kX86pJitOutOfSpace;
  }
  status = x86p_jit_translate_chain(mem,
                                    eip,
                                    storage->buffer,
                                    storage->capacity - storage->used,
                                    boundary,
                                    boundary_user,
                                    blocks,
                                    max_blocks,
                                    &run,
                                    &module_bytes,
                                    reason,
                                    reason_len);
  if (status != kX86pJitOk) {
    return status;
  }
  /*
   * One instantiation for the run, then one entry per body. Publishing sets no
   * entry: which export a block is entered at is the block's own index, and
   * x86p_wasm_body_name is the same function the module's export section used.
   */
  {
    int token = x86p_wasm_arena_publish(&storage->arena, storage->buffer, module_bytes, reason, reason_len);
    if (token < 0) {
      const char *host_error = x86p_wasm_host_error(&storage->arena.host);
      if (host_error[0] && reason && reason_len) {
        snprintf(reason, reason_len, "WebAssembly publication: %s", host_error);
      }
      return kX86pJitOutOfSpace;
    }
    for (i = 0u; i < run; i++) {
      const char *field = x86p_wasm_module_export_name(NULL, i);
      void *entry = x86p_wasm_arena_entry(&storage->arena, token, field ? field : x86p_wasm_body_name(i));
      if (!entry) {
        x86p_wasm_arena_release(&storage->arena, token);
        snprintf(reason, reason_len, "the module has no callable export named %s", x86p_wasm_body_name(i));
        return kX86pJitOutOfSpace;
      }
      blocks[i].entry = entry;
    }
    /* The record is the MODULE's: its guest range is what invalidation matches
       and its bytes are what the storage is holding, so a run of blocks is one
       entry here and is released as one. */
    first = blocks[0];
    storage->blocks[token] = (X86pWasmStoredBlock){
        first.guest_eip, (blocks[run - 1u].guest_eip + blocks[run - 1u].guest_len) - first.guest_eip, module_bytes};
    storage->used += module_bytes;
  }
  *count = run;
  return kX86pJitOk;
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
