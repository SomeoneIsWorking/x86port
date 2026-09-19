#include "jit_storage.h"

#include "jit_wasm.h"
#include "jit_wasm_compact.h"
#include "jit_wasm_host.h"
#include "jit_wasm_lower.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * HOW MANY BLOCKS SHARE A MODULE.
 *
 * Every published module is a permanent object inside the WebAssembly engine,
 * and an engine has a limit on those: measured in Firefox 156, instantiation
 * is refused at about 16,350 live modules, which this port's title reaches
 * four seconds into its boot. Blocks are still published one to a module so
 * they can run the moment they are translated, and then this many of them are
 * rebuilt as one -- see jit_wasm_compact.h.
 *
 * 32 rather than the 64 a module may hold, because the batch is lowered into
 * a buffer of its own and each body's worst case is about 20 KB: 32 costs
 * ~660 KB per guest thread and turns 16,000 modules into about 500, which is
 * two orders of magnitude of headroom under the measured ceiling. Doubling it
 * would double the buffer to halve a number that is already far enough below.
 */
#define X86P_WASM_COMPACT_BATCH 32u

typedef struct X86pWasmStoredBlock {
  uint32_t guest;
  uint32_t guest_len;
  int token;   /* the module it is published in */
  void *entry; /* the address it is entered at, which outlives its module */
  int live;
} X86pWasmStoredBlock;

/* What a published module costs and how many blocks still need it. A module is
   released when its last block goes, not when any one of them does. */
typedef struct X86pWasmStoredModule {
  size_t bytes;
  unsigned blocks;
} X86pWasmStoredModule;

struct X86pJitStorage {
  X86pWasmArena arena;
  X86pWasmStoredBlock *blocks; /* `capacity_blocks` entries, owned */
  X86pWasmStoredModule *modules;
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
  /* The batch is lowered into its own buffer, allocated the first time a
     thread actually fills a batch: a thread that translates a handful of
     blocks never pays for it. */
  unsigned char *batch;
  size_t batch_bytes;
  /* Blocks published singly and not yet shared into a module. */
  unsigned pending[X86P_WASM_COMPACT_BATCH];
  unsigned pending_count;
  /* Set once the engine has told us it cannot move a block's entry, because
     then no batch will ever compact and asking again each time is waste. */
  int cannot_compact;
  unsigned compactions;         /* batches that became one module */
  unsigned compaction_refusals; /* batches left as they were, with a reason */
  size_t byte_budget;           /* how many bytes of LIVE module may be held at once */
  size_t used;
  unsigned next_victim; /* the module eviction looks at next */
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
  storage->modules = calloc(max_blocks, sizeof *storage->modules);
  storage->buffer_bytes = X86P_WASM_MAX_MODULE_BYTES;
  storage->buffer = malloc(storage->buffer_bytes);
  if (!storage->blocks || !storage->modules || !storage->buffer) {
    if (reason && reason_len) {
      snprintf(reason,
               reason_len,
               "out of memory: %zu block record(s) plus a %zu-byte module buffer",
               max_blocks,
               storage->buffer_bytes);
    }
    free(storage->blocks);
    free(storage->modules);
    free(storage->buffer);
    free(storage);
    return NULL;
  }
  if (!x86p_wasm_host_create(&host, reason, reason_len)) {
    free(storage->blocks);
    free(storage->modules);
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
    free(storage->modules);
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
  memset(storage->modules, 0, (size_t)storage->capacity_blocks * sizeof *storage->modules);
  storage->pending_count = 0u;
  storage->used = 0u;
  storage->next_victim = 0u;
}

void x86p_jit_storage_destroy(X86pJitStorage *storage) {
  if (storage) {
    X86pWasmHost host = storage->arena.host;
    x86p_wasm_arena_dispose(&storage->arena);
    x86p_wasm_host_destroy(&host);
    free(storage->blocks);
    free(storage->modules);
    free(storage->buffer);
    free(storage->batch);
    free(storage);
  }
}

int x86p_jit_storage_has_room(const X86pJitStorage *storage) {
  return storage->byte_budget - storage->used >= X86P_WASM_MIN_MODULE_BYTES &&
         x86p_wasm_arena_live(&storage->arena) < storage->capacity_blocks &&
         /* ... and below whatever ceiling the engine has actually shown us,
            which may be far under the capacity this storage was created with. */
         x86p_wasm_arena_has_room(&storage->arena);
}

size_t x86p_jit_storage_used(const X86pJitStorage *storage) {
  return storage->used;
}

size_t x86p_jit_storage_capacity(const X86pJitStorage *storage) {
  return storage->byte_budget;
}

unsigned x86p_jit_storage_block_records(const X86pJitStorage *storage) {
  return storage->capacity_blocks;
}

/* ---- block and module bookkeeping ---------------------------------------- */

/* Blocks are indexed independently of modules now that several of them share
   one, so a published block needs a slot of its own. */
static int take_block_slot(X86pJitStorage *storage) {
  unsigned i;
  for (i = 0; i < storage->capacity_blocks; ++i) {
    if (!storage->blocks[i].live) {
      return (int)i;
    }
  }
  return -1;
}

static void hold_module(X86pJitStorage *storage, int token, size_t bytes) {
  X86pWasmStoredModule *module = &storage->modules[token];
  if (module->blocks == 0u) {
    module->bytes = bytes;
    storage->used += bytes;
  }
  module->blocks++;
}

/* Drop one block's claim on its module, releasing the module with the last
   claim. A module outlives any single block in it, which is the whole point of
   sharing one, so releasing on the first drop would free code still running. */
static void drop_module(X86pJitStorage *storage, int token) {
  X86pWasmStoredModule *module = &storage->modules[token];
  if (module->blocks == 0u) {
    return;
  }
  if (--module->blocks > 0u) {
    return;
  }
  x86p_wasm_arena_release(&storage->arena, token);
  storage->used -= module->bytes;
  module->bytes = 0u;
}

static void forget_pending(X86pJitStorage *storage, unsigned slot) {
  unsigned i;
  for (i = 0; i < storage->pending_count; ++i) {
    if (storage->pending[i] == slot) {
      storage->pending[i] = storage->pending[--storage->pending_count];
      return;
    }
  }
}

static void drop_block(X86pJitStorage *storage, unsigned slot) {
  X86pWasmStoredBlock *block = &storage->blocks[slot];
  if (!block->live) {
    return;
  }
  forget_pending(storage, slot);
  drop_module(storage, block->token);
  memset(block, 0, sizeof *block);
}

/* ---- sharing a module between blocks -------------------------------------- */

/*
 * Rebuild the blocks published since the last batch as ONE module.
 *
 * Nothing here is allowed to break a run: a refusal leaves every block exactly
 * where it was, still entered through the same address, and the run carries on
 * with modules it already has. The batch is cleared either way, because
 * retrying the same set that just refused would refuse again every time.
 */
static void
share_a_module(X86pJitStorage *storage, const X86pMem *mem, X86pJitBoundaryFn boundary, void *boundary_user) {
  X86pWasmCompactBlock batch[X86P_WASM_COMPACT_BATCH];
  X86pWasmCompactResult result;
  char why[256];
  unsigned i;

  if (storage->cannot_compact || storage->pending_count == 0u) {
    return;
  }
  if (!storage->batch) {
    /* Worst case for every body, plus the framing they share. */
    storage->batch_bytes = (size_t)X86P_WASM_COMPACT_BATCH * X86P_WASM_MAX_MODULE_BYTES;
    storage->batch = malloc(storage->batch_bytes);
    if (!storage->batch) {
      /* No buffer, no compaction -- and no failure either: the run is exactly
         as correct as it was, just with more modules. */
      storage->batch_bytes = 0u;
      storage->cannot_compact = 1;
      return;
    }
  }
  for (i = 0; i < storage->pending_count; ++i) {
    const X86pWasmStoredBlock *block = &storage->blocks[storage->pending[i]];
    batch[i].guest = block->guest;
    batch[i].guest_len = block->guest_len;
    batch[i].token = block->token;
    batch[i].entry = block->entry;
  }
  why[0] = '\0';
  if (!x86p_wasm_compact(&storage->arena,
                         mem,
                         storage->batch,
                         storage->batch_bytes,
                         boundary,
                         boundary_user,
                         batch,
                         storage->pending_count,
                         &result,
                         why,
                         sizeof why)) {
    storage->compaction_refusals++;
    if (!x86p_wasm_arena_can_adopt(&storage->arena)) {
      storage->cannot_compact = 1;
    }
    storage->pending_count = 0u;
    return;
  }
  for (i = 0; i < result.moved; ++i) {
    X86pWasmStoredBlock *block = &storage->blocks[storage->pending[i]];
    /* The module each block came from was released BY the compaction, so its
       claim is dropped here rather than released a second time. A pending
       block is always the only block in its module -- it was published singly
       and nothing shares a module until it is compacted -- and if that ever
       stopped being true this would be silently mis-accounting, so it is
       checked rather than assumed. */
    X86pWasmStoredModule *was = &storage->modules[block->token];
    if (was->blocks == 1u) {
      storage->used -= was->bytes;
      was->bytes = 0u;
      was->blocks = 0u;
    }
    block->token = result.token;
  }
  storage->modules[result.token].bytes = result.bytes;
  storage->modules[result.token].blocks = result.moved;
  storage->used += result.bytes;
  storage->compactions++;
  storage->pending_count = 0u;
}

/*
 * The module to empty next, taken round-robin so the run loses its oldest
 * translations rather than the ones it just made.
 *
 * Round-robin rather than "the module holding the fewest blocks": finding the
 * smallest means reading every module record on a path that runs whenever the
 * arena is full, and the modules a sweep is about to reach are the ones a
 * cursor already prefers.
 *
 * Returns -1 when no module is live at all, which is the caller's signal that
 * evicting cannot help and the cache has to be rewound.
 */
static int module_to_empty(X86pJitStorage *storage) {
  unsigned scanned;
  for (scanned = 0u; scanned < storage->capacity_blocks; ++scanned) {
    const unsigned token = storage->next_victim;
    storage->next_victim = (token + 1u) % storage->capacity_blocks;
    if (storage->modules[token].blocks > 0u) {
      return (int)token;
    }
  }
  return -1;
}

unsigned x86p_jit_storage_evict(X86pJitStorage *storage, X86pJitStorageDropFn drop, void *user) {
  const int token = module_to_empty(storage);
  unsigned dropped = 0u;
  unsigned i;
  if (token < 0) {
    return 0u;
  }
  /*
   * Every block in that module goes, in one call. Half-emptying it would free
   * nothing -- the module is released with its last block -- and the caller
   * would read the unmoved used figure as "evicting does not work here".
   */
  for (i = 0; i < storage->capacity_blocks && storage->modules[token].blocks > 0u; ++i) {
    X86pWasmStoredBlock *block = &storage->blocks[i];
    uint64_t end = (uint64_t)block->guest + block->guest_len;
    if (!block->live || block->token != token) {
      continue;
    }
    /* The caller forgets the block BEFORE its exec address can be handed to
       another one, which is why this is told rather than returned. */
    if (drop && end <= UINT32_MAX) {
      drop(user, block->guest, (uint32_t)end);
    }
    drop_block(storage, i);
    dropped++;
  }
  return dropped;
}

void x86p_jit_storage_invalidate(X86pJitStorage *storage, uint32_t lo, uint32_t hi) {
  unsigned i;
  if (lo >= hi) {
    return;
  }
  for (i = 0; i < storage->capacity_blocks; ++i) {
    X86pWasmStoredBlock *block = &storage->blocks[i];
    if (block->live && block->guest < hi && (uint64_t)block->guest + block->guest_len > lo) {
      drop_block(storage, i);
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
  X86pJitStatus status;
  int slot;
  int token;

  slot = take_block_slot(storage);
  if (slot < 0) {
    /* Every block record is in use. Not a translation failure: the caller
       evicts and comes back, exactly as it does for a full arena. */
    if (reason && reason_len) {
      snprintf(reason, reason_len, "all %u block record(s) are live", storage->capacity_blocks);
    }
    return kX86pJitOutOfSpace;
  }
  status =
      x86p_jit_translate_bounded(mem, eip, storage->buffer, room, boundary, boundary_user, block, reason, reason_len);
  if (status != kX86pJitOk) {
    return status;
  }
  token = x86p_jit_wasm_publish(&storage->arena, block, storage->buffer, block->host_bytes, reason, reason_len);
  if (token < 0) {
    /* The arena has already written why, WITH its denominators. Label it
       rather than replacing it: overwriting here is what reduced "out of
       memory with 40 modules live of 8192" to "out of memory". */
    const char *host_error = x86p_wasm_host_error(&storage->arena.host);
    if (reason && reason_len && reason[0]) {
      char said[320];
      snprintf(said, sizeof said, "%s", reason);
      snprintf(reason, reason_len, "WebAssembly publication: %s", said);
    } else if (host_error[0] && reason && reason_len) {
      snprintf(reason, reason_len, "WebAssembly publication: %s", host_error);
    }
    return kX86pJitOutOfSpace;
  }
  storage->blocks[slot] = (X86pWasmStoredBlock){block->guest_eip, block->guest_len, token, block->entry, 1};
  hold_module(storage, token, block->host_bytes);
  if (storage->pending_count < X86P_WASM_COMPACT_BATCH) {
    storage->pending[storage->pending_count++] = (unsigned)slot;
  }
  if (storage->pending_count == X86P_WASM_COMPACT_BATCH) {
    share_a_module(storage, mem, boundary, boundary_user);
  }
  return kX86pJitOk;
}

unsigned x86p_jit_storage_compactions(const X86pJitStorage *storage) {
  return storage ? storage->compactions : 0u;
}

unsigned x86p_jit_storage_compaction_refusals(const X86pJitStorage *storage) {
  return storage ? storage->compaction_refusals : 0u;
}

unsigned x86p_jit_storage_compaction_pending(const X86pJitStorage *storage) {
  return storage ? storage->pending_count : 0u;
}

int x86p_jit_storage_compaction_stopped(const X86pJitStorage *storage) {
  return storage ? storage->cannot_compact : 0;
}
