/*
 * test_jit_wasm_compact.c -- making one module out of many published blocks.
 *
 * WHAT IS BEING TESTED. Compaction moves running blocks from the modules they
 * were published in onto one shared module, and the thing that must never
 * happen is a half-done move: some blocks reaching the new module, some still
 * reaching modules that are about to be released, and a table entry left
 * pointing at code nobody owns. That state is not something a real engine can
 * be asked to produce on demand, so the engine here is a stub that can be told
 * to fail at a chosen step, and the table it would corrupt is modelled
 * explicitly so the test can look at it.
 *
 * The blocks are real: lowered from real guest bytes by the real lowering, so
 * a change that made the batch unbuildable would fail here rather than in a
 * browser.
 */
#include "jit_wasm_compact.h"

#include "jit_wasm_arena.h"
#include "jit_wasm_lower.h"
#include "jit_wasm_module.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

static void check(const char *what, long long got, long long want) {
  g_checks++;
  if (got != want) {
    g_failed++;
    printf("FAIL %s: got %lld, want %lld\n", what, got, want);
  }
}

/* ---- the stub engine ----------------------------------------------------- */

typedef struct Stub {
  int next;
  int live;
  int refuse_publish;  /* when set, instantiate() fails */
  int refuse_adopt_at; /* when >0, the Nth adopt fails */
  int adopts;
  int releases;
  struct {
    int module;
    const char *field;
  } table[256];
} Stub;

static int stub_instantiate(void *user, const void *bytes, size_t len, char *error, unsigned error_len) {
  Stub *s = (Stub *)user;
  if (!bytes || len == 0 || s->refuse_publish) {
    if (error && error_len) {
      snprintf(error, error_len, "InternalError: out of memory");
    }
    return -1;
  }
  s->live++;
  return s->next++;
}

static int stub_resolve(void *user, int module, const char *field) {
  Stub *s = (Stub *)user;
  int entry = module + 1;
  if (entry >= (int)(sizeof s->table / sizeof s->table[0])) {
    return 0;
  }
  s->table[entry].module = module;
  s->table[entry].field = field;
  return entry;
}

static int stub_adopt(void *user, int to, const char *to_field, int from, const char *from_field, int entry) {
  Stub *s = (Stub *)user;
  (void)from_field;
  if (entry <= 0 || entry >= (int)(sizeof s->table / sizeof s->table[0])) {
    return 0;
  }
  if (s->table[entry].module != from) {
    return 0;
  }
  if (s->refuse_adopt_at > 0 && ++s->adopts == s->refuse_adopt_at) {
    return 0;
  }
  if (s->refuse_adopt_at <= 0) {
    s->adopts++;
  }
  s->table[entry].module = to;
  s->table[entry].field = to_field;
  return 1;
}

static void stub_release(void *user, int module) {
  Stub *s = (Stub *)user;
  unsigned i;
  s->releases++;
  s->live--;
  for (i = 0; i < sizeof s->table / sizeof s->table[0]; ++i) {
    if (s->table[i].module == module) {
      s->table[i].module = -1;
      s->table[i].field = NULL;
    }
  }
}

/* ---- a guest to lower ---------------------------------------------------- */

#define GUEST_LO 0x1000u
#define ARENA_SIZE 0x1000u
#define BLOCKS 4u
/* Far enough apart that each block is its own, close enough to stay in the
   arena. Each holds `nop; ret`, which lowers on every backend. */
#define STRIDE 0x10u

static uint8_t g_guest[ARENA_SIZE];
static uint8_t g_buffer[64 * 4096];

static void seed_guest(void) {
  unsigned i;
  memset(g_guest, 0x90, sizeof g_guest);
  for (i = 0; i < BLOCKS; ++i) {
    g_guest[i * STRIDE] = 0x90;      /* nop  */
    g_guest[i * STRIDE + 1u] = 0xC3; /* ret  */
  }
}

static void seed_mem(X86pMem *mem) {
  memset(mem, 0, sizeof *mem);
  mem->host = g_guest;
  mem->lo = GUEST_LO;
  mem->size = ARENA_SIZE;
}

/*
 * Publish each block the way the storage does -- one module each -- and
 * describe them for the compactor.
 */
static int publish_singly(X86pWasmArena *arena, const X86pMem *mem, X86pWasmCompactBlock *out, unsigned count) {
  unsigned i;
  for (i = 0; i < count; ++i) {
    uint32_t eip = GUEST_LO + i * STRIDE;
    X86pJitBlock block;
    char reason[256];
    size_t bytes;
    int token;
    memset(&block, 0, sizeof block);
    bytes = x86p_jit_translate_batch(
        mem, &eip, 1u, g_buffer, sizeof g_buffer, NULL, NULL, NULL, NULL, &block, reason, sizeof reason);
    if (bytes == 0) {
      printf("FAIL could not lower the block at %08X: %s\n", eip, reason);
      g_failed++;
      return 0;
    }
    token = x86p_wasm_arena_publish(arena, g_buffer, bytes, reason, sizeof reason);
    if (token < 0) {
      printf("FAIL could not publish the block at %08X: %s\n", eip, reason);
      g_failed++;
      return 0;
    }
    out[i].guest = block.guest_eip;
    out[i].guest_len = block.guest_len;
    out[i].token = token;
    out[i].chain_first = block.chain_first_slot;
    out[i].chain_exits = block.chain_exits;
    out[i].leaf_site = block.leaf_site;
    out[i].entry = x86p_wasm_arena_entry(arena, token, x86p_wasm_body_name(0));
    if (!out[i].entry) {
      printf("FAIL the block at %08X has no entry\n", eip);
      g_failed++;
      return 0;
    }
  }
  return 1;
}

static void bind(X86pWasmArena *arena, Stub *stub, X86pWasmHost *host) {
  unsigned i;
  memset(stub, 0, sizeof *stub);
  for (i = 0; i < sizeof stub->table / sizeof stub->table[0]; ++i) {
    stub->table[i].module = -1;
  }
  memset(host, 0, sizeof *host);
  host->instantiate = stub_instantiate;
  host->resolve = stub_resolve;
  host->adopt = stub_adopt;
  host->release = stub_release;
  host->user = stub;
  x86p_wasm_arena_init(arena, host, 64u);
}

/* ---- cases --------------------------------------------------------------- */

static void test_four_modules_become_one(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  X86pWasmCompactBlock blocks[BLOCKS];
  X86pWasmCompactResult result;
  char reason[256];
  void *entries[BLOCKS];
  unsigned i;

  seed_guest();
  bind(&arena, &stub, &host);
  {
    X86pMem mem;
    seed_mem(&mem);
    if (!publish_singly(&arena, &mem, blocks, BLOCKS)) {
      return;
    }
    check("one module per block, to start with", x86p_wasm_arena_live(&arena), BLOCKS);
    for (i = 0; i < BLOCKS; ++i) {
      entries[i] = blocks[i].entry;
    }
    reason[0] = '\0';
    check("the batch is compacted",
          x86p_wasm_compact(
              &arena, &mem, g_buffer, sizeof g_buffer, NULL, blocks, BLOCKS, &result, reason, sizeof reason),
          1);
  }
  check("every block moved", result.moved, BLOCKS);
  check("and they are all in ONE module now", x86p_wasm_arena_live(&arena), 1);
  for (i = 0; i < BLOCKS; ++i) {
    char what[64];
    snprintf(what, sizeof what, "block %u is entered at the same address", i);
    check(what, blocks[i].entry == entries[i], 1);
    snprintf(what, sizeof what, "block %u's entry reaches the shared module", i);
    check(what, stub.table[(int)(uintptr_t)entries[i]].module, result.token);
    snprintf(what, sizeof what, "block %u is its own body of it", i);
    check(what, strcmp(stub.table[(int)(uintptr_t)entries[i]].field, x86p_wasm_body_name(i)), 0);
  }
  x86p_wasm_arena_dispose(&arena);
}

/*
 * The case that matters. An adopt failing part way through must leave every
 * block where it started -- not some on the new module and some on modules
 * this is about to release.
 */
static void test_a_failed_move_puts_every_block_back(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  X86pMem mem;
  X86pWasmCompactBlock blocks[BLOCKS];
  X86pWasmCompactResult result;
  char reason[256];
  int tokens[BLOCKS];
  void *entries[BLOCKS];
  unsigned i;

  seed_guest();
  bind(&arena, &stub, &host);
  seed_mem(&mem);
  if (!publish_singly(&arena, &mem, blocks, BLOCKS)) {
    return;
  }
  for (i = 0; i < BLOCKS; ++i) {
    tokens[i] = blocks[i].token;
    entries[i] = blocks[i].entry;
  }
  stub.refuse_adopt_at = 3; /* the third of four */
  reason[0] = '\0';
  check(
      "the compaction refuses",
      x86p_wasm_compact(&arena, &mem, g_buffer, sizeof g_buffer, NULL, blocks, BLOCKS, &result, reason, sizeof reason),
      0);
  check("it says which block stopped it", strstr(reason, "could not be moved") != NULL, 1);
  check("nothing is reported as moved", result.moved, 0);
  check("the module it had published is gone again", x86p_wasm_arena_live(&arena), BLOCKS);
  for (i = 0; i < BLOCKS; ++i) {
    char what[64];
    snprintf(what, sizeof what, "block %u is still in the module it was published in", i);
    check(what, stub.table[(int)(uintptr_t)entries[i]].module, tokens[i]);
    snprintf(what, sizeof what, "block %u is still entered at the same address", i);
    check(what, blocks[i].entry == entries[i], 1);
  }
  x86p_wasm_arena_dispose(&arena);
}

/* Guest code that changed under a block must not be quietly rebuilt: the body
   would no longer be the code its callers were entered into. */
static void test_a_block_whose_guest_changed_is_refused(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  X86pMem mem;
  X86pWasmCompactBlock blocks[BLOCKS];
  X86pWasmCompactResult result;
  char reason[256];

  seed_guest();
  bind(&arena, &stub, &host);
  seed_mem(&mem);
  if (!publish_singly(&arena, &mem, blocks, BLOCKS)) {
    return;
  }
  /* The guest rewrites the second block: what was `nop; ret` is now a longer
     run of instructions, so the block no longer covers what it did. */
  g_guest[STRIDE + 1u] = 0x90;
  g_guest[STRIDE + 2u] = 0x90;
  g_guest[STRIDE + 3u] = 0xC3;
  reason[0] = '\0';
  check(
      "the compaction refuses",
      x86p_wasm_compact(&arena, &mem, g_buffer, sizeof g_buffer, NULL, blocks, BLOCKS, &result, reason, sizeof reason),
      0);
  check("and says the guest changed it", strstr(reason, "needs invalidating") != NULL, 1);
  check("every block is still published as it was", x86p_wasm_arena_live(&arena), BLOCKS);
  x86p_wasm_arena_dispose(&arena);
}

/* A module that cannot be published leaves everything alone, and says so. */
static void test_a_refused_publication_changes_nothing(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  X86pMem mem;
  X86pWasmCompactBlock blocks[BLOCKS];
  X86pWasmCompactResult result;
  char reason[256];

  seed_guest();
  bind(&arena, &stub, &host);
  seed_mem(&mem);
  if (!publish_singly(&arena, &mem, blocks, BLOCKS)) {
    return;
  }
  stub.refuse_publish = 1;
  reason[0] = '\0';
  check(
      "the compaction refuses",
      x86p_wasm_compact(&arena, &mem, g_buffer, sizeof g_buffer, NULL, blocks, BLOCKS, &result, reason, sizeof reason),
      0);
  check("with the engine's own words", strstr(reason, "out of memory") != NULL, 1);
  check("and nothing was released", x86p_wasm_arena_live(&arena), BLOCKS);
  check("nor moved", result.moved, 0);
  x86p_wasm_arena_dispose(&arena);
}

/* An engine with no adopt: refused before anything is built, so a host that
   cannot support this never half-supports it. */
static void test_an_engine_that_cannot_adopt_is_refused_first(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  X86pMem mem;
  X86pWasmCompactBlock blocks[BLOCKS];
  X86pWasmCompactResult result;
  char reason[256];

  seed_guest();
  bind(&arena, &stub, &host);
  seed_mem(&mem);
  if (!publish_singly(&arena, &mem, blocks, BLOCKS)) {
    return;
  }
  arena.host.adopt = NULL;
  reason[0] = '\0';
  check(
      "the compaction refuses",
      x86p_wasm_compact(&arena, &mem, g_buffer, sizeof g_buffer, NULL, blocks, BLOCKS, &result, reason, sizeof reason),
      0);
  check("saying the engine cannot move an entry", strstr(reason, "cannot move") != NULL, 1);
  check("and it published nothing to find that out", x86p_wasm_arena_live(&arena), BLOCKS);
  x86p_wasm_arena_dispose(&arena);
}

int main(void) {
  test_four_modules_become_one();
  test_a_failed_move_puts_every_block_back();
  test_a_block_whose_guest_changed_is_refused();
  test_a_refused_publication_changes_nothing();
  test_an_engine_that_cannot_adopt_is_refused_first();
  printf("test_jit_wasm_compact: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
