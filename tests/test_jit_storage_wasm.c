/*
 * test_jit_storage_wasm.c -- the WebAssembly storage's own accounting.
 *
 * WHAT IS BEING TESTED. This storage bounds two different resources: the
 * BLOCK RECORDS a translation is filed in, and the engine MODULES those
 * records are published through. Compaction makes the two diverge -- thirty-two
 * records share one module -- so a room check that reads the module count and
 * calls it the record count reports room while every record is taken. That is
 * not a slow run: the caller evicts nothing, translation refuses with "all
 * 65536 block record(s) are live", and the port aborts. It did, in Firefox,
 * the first time the retail menu was driven into a level.
 *
 * The storage is the shipping one, compiled into this test with a stub wasm
 * host, so the accounting under test is the accounting that runs in a browser.
 * The blocks are real, lowered from real guest bytes by the real lowering.
 */
#include "jit_storage.h"

#include "jit_wasm.h"
#include "jit_wasm_arena.h"
#include "jit_wasm_host.h"
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

/* ---- a stub wasm host ----------------------------------------------------- */

/*
 * x86p_wasm_host_create is Emscripten-only, and the storage calls it itself
 * rather than taking a host, so this test supplies the definition. Nothing is
 * instantiated: what is being measured is which numbers the storage keeps.
 */
enum { kEntries = 4096 };

typedef struct Stub {
  int next;
  int live;
  struct {
    int module;
  } table[kEntries];
} Stub;

static Stub g_stub;

static int stub_instantiate(void *user, const void *bytes, size_t len, char *error, unsigned error_len) {
  Stub *s = (Stub *)user;
  if (!bytes || len == 0u) {
    if (error && error_len) {
      snprintf(error, error_len, "empty module");
    }
    return kX86pWasmRefusedModule;
  }
  s->live++;
  return s->next++;
}

static int stub_resolve(void *user, int module, const char *field) {
  Stub *s = (Stub *)user;
  int entry = module + 1;
  (void)field;
  if (entry <= 0 || entry >= kEntries) {
    return 0;
  }
  s->table[entry].module = module;
  return entry;
}

static int stub_adopt(void *user, int to, const char *to_field, int from, const char *from_field, int entry) {
  Stub *s = (Stub *)user;
  (void)to_field;
  (void)from_field;
  if (entry <= 0 || entry >= kEntries || s->table[entry].module != from) {
    return 0;
  }
  s->table[entry].module = to;
  return 1;
}

static void stub_release(void *user, int module) {
  Stub *s = (Stub *)user;
  int i;
  s->live--;
  for (i = 0; i < kEntries; ++i) {
    if (s->table[i].module == module) {
      s->table[i].module = -1;
    }
  }
}

int x86p_wasm_host_create(X86pWasmHost *host, char *reason, unsigned reason_len) {
  (void)reason;
  (void)reason_len;
  memset(&g_stub, 0, sizeof g_stub);
  g_stub.next = 1;
  host->instantiate = stub_instantiate;
  host->resolve = stub_resolve;
  host->adopt = stub_adopt;
  host->release = stub_release;
  host->user = &g_stub;
  return 1;
}

void x86p_wasm_host_destroy(X86pWasmHost *host) {
  memset(host, 0, sizeof *host);
}

const char *x86p_wasm_host_error(const X86pWasmHost *host) {
  (void)host;
  return "";
}

/*
 * Publication, which on a real host is jit_wasm.c's. That file cannot be
 * compiled here at all -- it asserts a 32-bit host pointer -- so the step is
 * taken through the real arena instead. Publication is the BOUNDARY of what is
 * under test: everything this test measures is on the storage's side of it.
 */
int x86p_jit_wasm_publish(
    X86pWasmArena *arena, X86pJitBlock *block, const void *module, size_t len, char *reason, unsigned reason_len) {
  int token = x86p_wasm_arena_publish(arena, module, len, reason, reason_len);
  if (token < 0) {
    return -1;
  }
  block->entry = x86p_wasm_arena_entry(arena, token, x86p_wasm_body_name(0));
  if (!block->entry) {
    x86p_wasm_arena_release(arena, token);
    return -1;
  }
  return token;
}

/* ---- a guest to translate ------------------------------------------------- */

#define GUEST_LO 0x1000u
#define RECORDS 64u
#define STRIDE 0x10u
#define GUEST_BYTES (RECORDS * STRIDE)

static uint8_t g_guest[GUEST_BYTES];

static void seed_mem(X86pMem *mem) {
  unsigned i;
  memset(g_guest, 0x90, sizeof g_guest);
  for (i = 0; i < RECORDS; ++i) {
    g_guest[i * STRIDE] = 0x90;      /* nop */
    g_guest[i * STRIDE + 1u] = 0xC3; /* ret */
  }
  memset(mem, 0, sizeof *mem);
  mem->host = g_guest;
  mem->lo = GUEST_LO;
  mem->size = GUEST_BYTES;
}

static unsigned g_dropped;

static void note_drop(void *user, uint32_t lo, uint32_t hi) {
  (void)user;
  (void)lo;
  (void)hi;
  g_dropped++;
}

int main(void) {
  char reason[256] = {0};
  X86pMem mem;
  X86pJitStorage *storage;
  unsigned i;
  unsigned translated = 0u;
  unsigned dropped;

  seed_mem(&mem);
  /* Bytes are deliberately not the limit under test: a budget this large keeps
     the out-of-bytes answer out of the way, so a refusal here is about records
     or modules and nothing else. */
  storage = x86p_jit_storage_create(64u * 1024u * 1024u, RECORDS, reason, sizeof reason);
  if (!storage) {
    printf("FAIL storage create: %s\n", reason);
    return 1;
  }

  /* The negative first: an empty storage has room, and says so with the one
     answer that means room. */
  check("empty storage has room", x86p_jit_storage_room(storage), kX86pJitStorageRoom);
  check("empty storage has_room", x86p_jit_storage_has_room(storage), 1);
  check("records reported", x86p_jit_storage_block_records(storage), RECORDS);

  for (i = 0; i < RECORDS; ++i) {
    X86pJitBlock block;
    X86pJitStatus st =
        x86p_jit_storage_translate(storage, &mem, GUEST_LO + i * STRIDE, NULL, NULL, &block, reason, sizeof reason);
    if (st != kX86pJitOk) {
      printf("FAIL translate %u of %u: %s\n", i, RECORDS, reason);
      break;
    }
    translated++;
    if (i + 1u < RECORDS) {
      check("room while records remain", x86p_jit_storage_room(storage), kX86pJitStorageRoom);
    }
  }
  check("every record filled", translated, RECORDS);

  /*
   * The discriminator. Compaction has left far fewer modules live than there
   * are records, so a room check reading the module count answers "room" here
   * -- and the next translation then refuses with nothing evicted.
   */
  check("blocks were compacted into shared modules", x86p_jit_storage_compactions(storage), RECORDS / 32u);
  check("full of records", x86p_jit_storage_room(storage), kX86pJitStorageOutOfSlots);
  check("full storage has_room", x86p_jit_storage_has_room(storage), 0);

  g_dropped = 0u;
  dropped = x86p_jit_storage_evict(storage, note_drop, NULL);
  check("eviction dropped a whole module's worth", dropped > 0u, 1);
  check("caller was told about each", g_dropped, dropped);
  check("room again after eviction", x86p_jit_storage_room(storage), kX86pJitStorageRoom);

  /* The count came back down rather than only up: translating again fits. */
  {
    X86pJitBlock block;
    X86pJitStatus st = x86p_jit_storage_translate(storage, &mem, GUEST_LO, NULL, NULL, &block, reason, sizeof reason);
    check("translates again after eviction", st, kX86pJitOk);
  }

  /* A reset puts every record back, not just the modules. */
  x86p_jit_storage_reset(storage);
  check("reset restores room", x86p_jit_storage_room(storage), kX86pJitStorageRoom);

  x86p_jit_storage_destroy(storage);
  printf("%s: %d check(s), %d failure(s)\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
