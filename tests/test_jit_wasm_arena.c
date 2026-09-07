/*
 * test_jit_wasm_arena.c -- module lifetime.
 *
 * WHAT IS BEING TESTED AND WHY IT NEEDS NO ENGINE. An instantiated
 * WebAssembly module is permanent: the engine has no unload, so a module per
 * translated block is a permanent engine object per block and a play session
 * translates tens of thousands. The arena is the accounting that keeps that
 * bounded, and the accounting is where the mistakes are -- an off-by-one in
 * the slot search, a release that forgets to decrement, a refusal that reports
 * a count it never checked. None of that is about WebAssembly, so the engine
 * is a stub host here and every case runs everywhere.
 *
 * A stub host is not a weaker test of the accounting; it is a stronger one. It
 * can be made to FAIL instantiation on demand, which a real engine cannot, and
 * it counts what it was asked to do, so "released everything" is checked
 * against the host rather than against the arena's own opinion.
 */
#include "jit_wasm_arena.h"

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
  int next;            /* the next handle to hand out */
  int live;            /* handles instantiated and not yet released */
  int instantiations;  /* calls to instantiate(), successful or not */
  int releases;        /* calls to release() */
  int refuse;          /* when set, instantiate() fails */
  int resolve_fails;   /* when set, resolve() returns 0 -- the null table slot */
  int last_released;
} Stub;

static int stub_instantiate(void *user, const void *bytes, size_t len) {
  Stub *s = (Stub *)user;
  s->instantiations++;
  if (!bytes || len == 0 || s->refuse) {
    return -1;
  }
  s->live++;
  return s->next++;
}

static int stub_resolve(void *user, int module, const char *field) {
  Stub *s = (Stub *)user;
  (void)field;
  if (s->resolve_fails) {
    return 0;
  }
  /* A nonzero, module-specific index, because that is what a table slot is. */
  return module + 1;
}

static void stub_release(void *user, int module) {
  Stub *s = (Stub *)user;
  s->releases++;
  s->live--;
  s->last_released = module;
}

static void bind(X86pWasmArena *arena, Stub *stub, X86pWasmHost *host) {
  memset(stub, 0, sizeof *stub);
  memset(host, 0, sizeof *host);
  host->instantiate = stub_instantiate;
  host->resolve = stub_resolve;
  host->release = stub_release;
  host->user = stub;
  x86p_wasm_arena_init(arena, host);
}

/* ---- cases --------------------------------------------------------------- */

static const uint8_t kModule[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

static void test_publish_and_release(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  int token;
  void *entry;
  char reason[128];
  bind(&arena, &stub, &host);

  reason[0] = '\0';
  token = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  check("publish returns a token", token >= 0, 1);
  check("one module live", x86p_wasm_arena_live(&arena), 1);
  check("the engine was asked once", stub.instantiations, 1);

  entry = x86p_wasm_arena_entry(&arena, token, "b0");
  check("the entry is callable", entry != NULL, 1);

  x86p_wasm_arena_release(&arena, token);
  check("nothing live after release", x86p_wasm_arena_live(&arena), 0);
  check("the engine released it", stub.live, 0);
  check("released is counted", x86p_wasm_arena_released(&arena), 1);

  /* A released token has no entry. Asking for one is how a caller that kept a
     stale token finds out, instead of calling through a slot that now holds
     something else. */
  check("a released token has no entry", x86p_wasm_arena_entry(&arena, token, "b0") == NULL, 1);

  /* Releasing twice is a caller being careful, not an error. */
  x86p_wasm_arena_release(&arena, token);
  check("a second release does nothing", stub.releases, 1);
}

static void test_cap_refuses_rather_than_evicting(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  unsigned i;
  int token;
  char reason[256];
  bind(&arena, &stub, &host);

  for (i = 0; i < X86P_WASM_MAX_LIVE_MODULES; i++) {
    if (x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason) < 0) {
      break;
    }
  }
  check("the cap is reached exactly", i, X86P_WASM_MAX_LIVE_MODULES);
  check("every slot is live", x86p_wasm_arena_live(&arena), X86P_WASM_MAX_LIVE_MODULES);

  reason[0] = '\0';
  token = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  check("publishing past the cap is refused", token, -1);
  check("the refusal is counted", x86p_wasm_arena_refusals(&arena), 1);
  check("the refusal says something", reason[0] != '\0', 1);
  /*
   * THE POINT OF THE WHOLE FILE. Nothing was evicted to make room: the block
   * cache holds entry addresses this arena handed out and does not ask before
   * entering one, so an evicted module's entry would be a call through a table
   * slot that now belongs to something else.
   */
  check("nothing was evicted", stub.releases, 0);
  check("the engine was not asked again", stub.instantiations, (int)X86P_WASM_MAX_LIVE_MODULES);

  /* Releasing one makes room for exactly one. */
  x86p_wasm_arena_release(&arena, 0);
  check("publishing succeeds again", x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason),
        0);
  check("still at the cap", x86p_wasm_arena_live(&arena), X86P_WASM_MAX_LIVE_MODULES);
}

static void test_release_all(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  int i;
  bind(&arena, &stub, &host);
  for (i = 0; i < 5; i++) {
    x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, NULL, 0);
  }
  x86p_wasm_arena_release_all(&arena);
  check("nothing live", x86p_wasm_arena_live(&arena), 0);
  check("the engine released all five", stub.releases, 5);
  check("the engine holds none", stub.live, 0);
}

static void test_engine_failure_is_not_a_refusal(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  char reason[128];
  bind(&arena, &stub, &host);
  stub.refuse = 1;
  reason[0] = '\0';
  check("the engine's rejection is reported",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason), -1);
  /*
   * Counted apart from a cap refusal, because they are different facts: one
   * says the caller is holding too many modules, the other says the engine
   * would not take this one. A single counter would make a broken lowering
   * look like a leak.
   */
  check("counted as a failure", x86p_wasm_arena_failures(&arena), 1);
  check("not counted as a refusal", x86p_wasm_arena_refusals(&arena), 0);
  check("nothing live", x86p_wasm_arena_live(&arena), 0);
}

static void test_unreachable_export_is_released(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  int token;
  bind(&arena, &stub, &host);
  stub.resolve_fails = 1;
  token = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, NULL, 0);
  check("published", token >= 0, 1);
  check("the null table slot is not an entry", x86p_wasm_arena_entry(&arena, token, "b0") == NULL, 1);
}

static void test_no_engine_refuses(void) {
  X86pWasmArena arena;
  char reason[128];
  /*
   * An arena with no host. A build with no engine glue must refuse rather than
   * appear to work until the first block runs, and the refusal must SAY that
   * is what happened.
   */
  x86p_wasm_arena_init(&arena, NULL);
  reason[0] = '\0';
  check("publishing without an engine is refused",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason), -1);
  check("the refusal says something", reason[0] != '\0', 1);
  check("counted", x86p_wasm_arena_refusals(&arena), 1);
}

static void test_partial_host_is_no_host(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  memset(&stub, 0, sizeof stub);
  memset(&host, 0, sizeof host);
  /* Instantiate but no release: a host that can create and never destroy is
     exactly the shape this file exists to prevent, so it is not accepted at
     all. */
  host.instantiate = stub_instantiate;
  host.resolve = stub_resolve;
  host.user = &stub;
  x86p_wasm_arena_init(&arena, &host);
  check("a host that cannot release is refused",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, NULL, 0), -1);
  check("the engine was never asked", stub.instantiations, 0);
}

int main(void) {
  test_publish_and_release();
  test_cap_refuses_rather_than_evicting();
  test_release_all();
  test_engine_failure_is_not_a_refusal();
  test_unreachable_export_is_released();
  test_no_engine_refuses();
  test_partial_host_is_no_host();
  printf("test_jit_wasm_arena: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
