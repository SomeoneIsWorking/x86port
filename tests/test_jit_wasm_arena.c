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

/* What a real engine says when it will not take another module. Firefox's
 * module compiler says exactly this shape, and the arena must repeat it. */
static const char kEngineWords[] = "InternalError: out of memory";

typedef struct Stub {
  int next;           /* the next handle to hand out */
  int live;           /* handles instantiated and not yet released */
  int instantiations; /* calls to instantiate(), successful or not */
  int releases;       /* calls to release() */
  int refuse;         /* when set, instantiate() fails */
  int resolve_fails;  /* when set, resolve() returns 0 -- the null table slot */
  int engine_ceiling; /* when set, the engine refuses beyond this many live */
  int last_released;
  int adoptions;
  int freed_entries;
  /* The indirect table, as the host really has one: which module owns each
     entry and which of its exports it points at. */
  struct {
    int module;
    const char *field;
  } table[64];
} Stub;

static int stub_instantiate(void *user, const void *bytes, size_t len, char *error, unsigned error_len) {
  Stub *s = (Stub *)user;
  s->instantiations++;
  /* An engine that holds fewer modules than the arena was built for, and says
     so only by refusing one. This is Firefox, measured. */
  if (s->engine_ceiling && s->live >= s->engine_ceiling) {
    if (error && error_len) {
      snprintf(error, error_len, "%s", kEngineWords);
    }
    return -1;
  }
  if (!bytes || len == 0 || s->refuse) {
    if (error && error_len) {
      snprintf(error, error_len, "%s", kEngineWords);
    }
    return -1;
  }
  s->live++;
  return s->next++;
}

static int stub_resolve(void *user, int module, const char *field) {
  Stub *s = (Stub *)user;
  if (s->resolve_fails) {
    return 0;
  }
  /* A nonzero, module-specific index, because that is what a table slot is. */
  if (module + 1 < (int)(sizeof s->table / sizeof s->table[0])) {
    s->table[module + 1].module = module;
    s->table[module + 1].field = field;
  }
  return module + 1;
}

/*
 * A stub that models what the real host must do: the entry keeps its value,
 * what sits behind it changes, and ownership of the entry moves with it. The
 * table is modelled explicitly so a release that frees an entry the caller is
 * still entered through is VISIBLE -- that is the defect this operation exists
 * to avoid, and it cannot be seen from the arena's own counters.
 */
static int stub_adopt(void *user, int to, const char *to_field, int from, const char *from_field, int entry) {
  Stub *s = (Stub *)user;
  if (entry <= 0 || entry >= (int)(sizeof s->table / sizeof s->table[0])) {
    return 0;
  }
  if (s->table[entry].module != from) {
    return 0; /* not an entry that module owns */
  }
  (void)from_field;
  s->table[entry].module = to;
  s->table[entry].field = to_field;
  s->adoptions++;
  return 1;
}

static void stub_release(void *user, int module) {
  Stub *s = (Stub *)user;
  unsigned i;
  s->releases++;
  s->live--;
  s->last_released = module;
  /* Freeing the entries this module still owns -- and ONLY those. */
  for (i = 0; i < sizeof s->table / sizeof s->table[0]; ++i) {
    if (s->table[i].module == module) {
      s->table[i].module = -1;
      s->table[i].field = NULL;
      s->freed_entries++;
    }
  }
}

/* Small enough that the cap case runs in no time, large enough that the free
   list is exercised across several slots rather than one. */
enum { kTestCapacity = 64u };

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
  check("the arena took its capacity", x86p_wasm_arena_init(arena, host, kTestCapacity), 1);
  check("and reports it", x86p_wasm_arena_capacity(arena), kTestCapacity);
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
  x86p_wasm_arena_dispose(&arena);
}

static void test_cap_refuses_rather_than_evicting(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  unsigned i;
  int token;
  char reason[256];
  bind(&arena, &stub, &host);

  for (i = 0; i < kTestCapacity; i++) {
    if (x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason) < 0) {
      break;
    }
  }
  check("the cap is reached exactly", i, kTestCapacity);
  check("every slot is live", x86p_wasm_arena_live(&arena), kTestCapacity);

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
  check("the engine was not asked again", stub.instantiations, (int)kTestCapacity);

  /* Releasing one makes room for exactly one. */
  x86p_wasm_arena_release(&arena, 0);
  check(
      "publishing succeeds again", x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason), 0);
  check("still at the cap", x86p_wasm_arena_live(&arena), kTestCapacity);
  x86p_wasm_arena_dispose(&arena);
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
  x86p_wasm_arena_dispose(&arena);
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
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason),
        -1);
  /*
   * Counted apart from a cap refusal, because they are different facts: one
   * says the caller is holding too many modules, the other says the engine
   * would not take this one. A single counter would make a broken lowering
   * look like a leak.
   */
  check("counted as a failure", x86p_wasm_arena_failures(&arena), 1);
  /*
   * And it repeats what the engine said. A refusal that reads only "rejected a
   * 42-byte module" cannot tell a bad lowering from an engine that has run out
   * of memory, and the second one is what silently ended a whole browser run.
   */
  check("the engine's own words survive", strstr(reason, kEngineWords) != NULL, 1);
  check("not counted as a refusal", x86p_wasm_arena_refusals(&arena), 0);
  check("nothing live", x86p_wasm_arena_live(&arena), 0);
  x86p_wasm_arena_dispose(&arena);
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
  x86p_wasm_arena_dispose(&arena);
}

static void test_no_engine_refuses(void) {
  X86pWasmArena arena;
  char reason[128];
  /*
   * An arena with no host. A build with no engine glue must refuse rather than
   * appear to work until the first block runs, and the refusal must SAY that
   * is what happened.
   */
  check("an arena with no host still takes a capacity", x86p_wasm_arena_init(&arena, NULL, kTestCapacity), 1);
  reason[0] = '\0';
  check("publishing without an engine is refused",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason),
        -1);
  check("the refusal says something", reason[0] != '\0', 1);
  check("counted", x86p_wasm_arena_refusals(&arena), 1);
  x86p_wasm_arena_dispose(&arena);
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
  check("the arena took its capacity", x86p_wasm_arena_init(&arena, &host, kTestCapacity), 1);
  check("a host that cannot release is refused", x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, NULL, 0), -1);
  check("the engine was never asked", stub.instantiations, 0);
  x86p_wasm_arena_dispose(&arena);
}

static void test_zero_capacity_is_refused(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  memset(&stub, 0, sizeof stub);
  memset(&host, 0, sizeof host);
  host.instantiate = stub_instantiate;
  host.resolve = stub_resolve;
  host.release = stub_release;
  host.user = &stub;
  /* A capacity of zero is not "unbounded", it is a caller mistake, and an
     arena that accepted it would refuse every publication later with a message
     about slots rather than about the number it was given. */
  check("a zero capacity is refused", x86p_wasm_arena_init(&arena, &host, 0u), 0);
  check("and it holds nothing", x86p_wasm_arena_capacity(&arena), 0);
  check("so publishing is refused", x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, NULL, 0), -1);
  check("the engine was never asked", stub.instantiations, 0);
  x86p_wasm_arena_dispose(&arena);
}

/*
 * The arena is given more slots than the engine can hold, which is what a
 * browser does: Firefox 156 refused the 16,112th module of a 65,536-slot arena
 * and the run died with room to spare by the arena's own accounting. The arena
 * must take the refusal as the measurement it is.
 */
static void test_the_engine_ceiling_is_learned_from_a_refusal(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  char reason[256];
  int token;
  unsigned i;
  bind(&arena, &stub, &host);
  stub.engine_ceiling = 3;
  check("no ceiling is assumed before one is shown", x86p_wasm_arena_ceiling(&arena), 0);
  check("and the arena has room", x86p_wasm_arena_has_room(&arena), 1);
  for (i = 0; i < 3u; ++i) {
    check("publishing under the engine's ceiling succeeds",
          x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason) >= 0,
          1);
  }
  reason[0] = '\0';
  check("the engine refuses the next one",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason),
        -1);
  check("the ceiling is what was live when it refused", x86p_wasm_arena_ceiling(&arena), 3);
  check("the reason names it", strstr(reason, "no more than 3") != NULL, 1);
  check("and the arena is now full at that number, though slots remain", x86p_wasm_arena_has_room(&arena), 0);
  check("the slots really do remain", x86p_wasm_arena_capacity(&arena) > 3u, 1);
  /*
   * The point of learning it is that the run continues. Release one, and the
   * arena must accept a replacement -- a ceiling that refused forever would be
   * the same dead run with a better message.
   */
  token = 0;
  x86p_wasm_arena_release(&arena, token);
  check("after a release there is room again", x86p_wasm_arena_has_room(&arena), 1);
  check("and publishing succeeds",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason) >= 0,
        1);
  check("the ceiling is not raised by that success", x86p_wasm_arena_ceiling(&arena), 3);
  x86p_wasm_arena_dispose(&arena);
}

/*
 * A ceiling learned from one refusal is not a line the arena may then sit on.
 * Measured in Firefox: publication was refused a second time with the arena
 * already below the ceiling it had just learned, so a run that evicts one and
 * republishes dies on the next block. The arena therefore backs off by a
 * margin, and this is what that margin has to be worth: releasing one is not
 * enough to declare room, and the refusal says the working limit rather than
 * the raw ceiling.
 */
static void test_a_learned_ceiling_is_worked_below_not_on(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  char reason[256];
  int token[32];
  unsigned headroom;
  unsigned i;
  bind(&arena, &stub, &host);
  /* Under the arena's own capacity, so the refusal can only come from the
     engine -- a full arena would refuse for a different reason and prove
     nothing about the ceiling. */
  stub.engine_ceiling = 32;
  for (i = 0; i < 32u; ++i) {
    token[i] = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
    check("publishing under the engine's ceiling succeeds", token[i] >= 0, 1);
  }
  reason[0] = '\0';
  check("the engine refuses the next one",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason),
        -1);
  check("the ceiling is what was live when it refused", x86p_wasm_arena_ceiling(&arena), 32);
  headroom = x86p_wasm_arena_headroom(&arena);
  check("and a ceiling that large carries headroom", headroom, 32u / kX86pWasmCeilingHeadroomDivisor);
  check("the reason names the working limit and the ceiling it came from",
        strstr(reason, "no more than 30 of a 32") != NULL,
        1);
  /* Releasing back to just under the ceiling is not room: that is the state
     that was measured refusing. Only clearing the headroom as well is. */
  for (i = 0; i < headroom; ++i) {
    x86p_wasm_arena_release(&arena, token[i]);
    check("still no room while the arena is inside its headroom", x86p_wasm_arena_has_room(&arena), 0);
  }
  x86p_wasm_arena_release(&arena, token[headroom]);
  check("clearing the headroom is what gives room", x86p_wasm_arena_has_room(&arena), 1);
  check("and publishing succeeds",
        x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason) >= 0,
        1);
  check("the ceiling is not raised by that success", x86p_wasm_arena_ceiling(&arena), 32);
  x86p_wasm_arena_dispose(&arena);
}

/*
 * A BLOCK OUTLIVES THE MODULE IT WAS PUBLISHED IN.
 *
 * Publishing one module per block is what makes a browser refuse at about
 * 16,350 live modules, so blocks get rebuilt into shared modules -- and a
 * block that is already running can only survive that if the address it is
 * entered at survives. Adoption is that operation. What has to be true after
 * it: the entry's VALUE is unchanged, it now reaches the new module, and
 * releasing the module the block came from does not free it.
 *
 * The last of those is the one a counter cannot show, so the stub models the
 * table and this asserts on the table, not on the arena's opinion of it.
 */
static void test_a_block_survives_the_release_of_the_module_it_came_from(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  char reason[256];
  int single;
  int shared;
  void *entry;
  bind(&arena, &stub, &host);
  check("this host can move an entry", x86p_wasm_arena_can_adopt(&arena), 1);

  single = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  shared = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  check("the block's own module was published", single >= 0, 1);
  check("and the module it is to share", shared >= 0, 1);
  entry = x86p_wasm_arena_entry(&arena, single, "b0");
  check("the block has an entry", entry != NULL, 1);
  check("which the module it came from owns", stub.table[(int)(uintptr_t)entry].module, single);

  check("the entry is adopted", x86p_wasm_arena_adopt(&arena, shared, "b7", single, "b0", entry), 1);
  check("the arena counted it", x86p_wasm_arena_adoptions(&arena), 1);
  check("the entry now reaches the shared module", stub.table[(int)(uintptr_t)entry].module, shared);
  check("at the body it was moved onto", strcmp(stub.table[(int)(uintptr_t)entry].field, "b7"), 0);

  /* THE POINT. */
  x86p_wasm_arena_release(&arena, single);
  check("releasing the old module freed no entry", stub.freed_entries, 0);
  check("and the block is still entered through the same address", stub.table[(int)(uintptr_t)entry].module, shared);

  /* And the shared module still owns it, so releasing THAT does free it. */
  x86p_wasm_arena_release(&arena, shared);
  check("releasing the module it moved to frees it", stub.freed_entries, 1);
  x86p_wasm_arena_dispose(&arena);
}

/* Adoption refuses rather than half-doing it. Each of these would otherwise
   leave an entry pointing at something nobody owns. */
static void test_adoption_refuses_what_it_cannot_do(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  char reason[256];
  int single;
  int shared;
  void *entry;
  bind(&arena, &stub, &host);
  single = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  shared = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  entry = x86p_wasm_arena_entry(&arena, single, "b0");
  check("the null table entry is never adopted", x86p_wasm_arena_adopt(&arena, shared, "b7", single, "b0", NULL), 0);
  check("nor an entry the source does not own", x86p_wasm_arena_adopt(&arena, shared, "b7", shared, "b0", entry), 0);
  x86p_wasm_arena_release(&arena, shared);
  check("nor onto a module that is not live", x86p_wasm_arena_adopt(&arena, shared, "b7", single, "b0", entry), 0);
  check("and none of that was counted as an adoption", x86p_wasm_arena_adoptions(&arena), 0);
  check("the entry still belongs where it did", stub.table[(int)(uintptr_t)entry].module, single);
  x86p_wasm_arena_dispose(&arena);
}

/* A host with no adopt at all: the arena says so instead of pretending. */
static void test_an_engine_that_cannot_adopt_says_so(void) {
  X86pWasmArena arena;
  X86pWasmHost host;
  Stub stub;
  char reason[256];
  int single;
  int shared;
  void *entry;
  bind(&arena, &stub, &host);
  single = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  shared = x86p_wasm_arena_publish(&arena, kModule, sizeof kModule, reason, sizeof reason);
  entry = x86p_wasm_arena_entry(&arena, single, "b0");
  arena.host.adopt = NULL;
  check("an arena whose host cannot adopt says so", x86p_wasm_arena_can_adopt(&arena), 0);
  check("and refuses to", x86p_wasm_arena_adopt(&arena, shared, "b7", single, "b0", entry), 0);
  x86p_wasm_arena_dispose(&arena);
}

int main(void) {
  test_publish_and_release();
  test_cap_refuses_rather_than_evicting();
  test_release_all();
  test_engine_failure_is_not_a_refusal();
  test_unreachable_export_is_released();
  test_no_engine_refuses();
  test_partial_host_is_no_host();
  test_zero_capacity_is_refused();
  test_the_engine_ceiling_is_learned_from_a_refusal();
  test_a_learned_ceiling_is_worked_below_not_on();
  test_a_block_survives_the_release_of_the_module_it_came_from();
  test_adoption_refuses_what_it_cannot_do();
  test_an_engine_that_cannot_adopt_says_so();
  printf("test_jit_wasm_arena: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
