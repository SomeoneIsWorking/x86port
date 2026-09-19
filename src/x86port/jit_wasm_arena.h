/*
 * jit_wasm_arena.h -- the lifetime of instantiated modules.
 *
 * WHY THIS FILE EXISTS AT ALL. On every other host, publishing a translated
 * block means making bytes executable, and forgetting to reclaim them costs
 * address space that the block cache already accounts for. WebAssembly has no
 * equivalent: handing a module to the engine creates an object that lives for
 * as long as anything can reach it. The host clears its table entry and drops
 * the module reference on release, allowing collection. A play session can
 * translate millions of blocks, so the number of simultaneously reachable
 * modules must remain bounded.
 *
 * THE ANSWER IS A CAP AND A REFUSAL, NOT SILENT EVICTION. This object holds a
 * bounded number of live instantiations and REFUSES to publish beyond it,
 * naming the refusal and counting it. The engine explicitly evicts a cached
 * translation before releasing its module here. Releasing behind the engine's
 * back would leave a cache address pointing at an empty or reused table slot.
 *
 * THE ENGINE IS A VTABLE. Instantiation is the one operation in this backend
 * that genuinely needs the wasm host, so it is the one operation behind an
 * interface. That keeps the accounting -- which is where the bugs are --
 * testable on any machine with a stub host, and confines the Emscripten-only
 * code to the small implementation that fills the vtable in.
 */
#ifndef X86PORT_JIT_WASM_ARENA_H
#define X86PORT_JIT_WASM_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the arena needs an engine to do. Three operations, because that is
 * what instantiating and calling a module actually takes and no fewer.
 *
 * `instantiate` binds the imports listed in jit_wasm_module.h -- the host asks
 * x86p_wasm_import_field() and x86p_wasm_import_address() for them rather than
 * matching names against its own symbol table.
 *
 * `resolve` returns an address the block can be CALLED at. On a wasm host that
 * is an indirect-table index, which is exactly what a function pointer is
 * there, so the value goes straight into X86pJitBlock::entry. A return of 0 or
 * below is a failure: table index 0 is the null entry.
 *
 * `adopt` is what makes a block's module replaceable underneath it. A block
 * that is already running holds ONE address -- its table entry -- and every
 * cached reference and every compiled call goes through that entry, so a
 * module can be swapped for another that exports the same code as long as the
 * entry keeps working. `adopt` points an existing entry at another module's
 * export and moves the entry's ownership with it, so releasing the module the
 * block came from no longer frees an entry the block is still entered
 * through. It is optional: a host that cannot do it leaves the pointer NULL
 * and its caller does not compact, rather than compacting incorrectly.
 */
/*
 * What a refused instantiation says about the ENGINE, as opposed to about the
 * module that was offered.
 *
 * The distinction decides whether a refusal teaches a ceiling. It must: one
 * module the engine would not compile taught a ceiling of 2,363 modules, and
 * the run spent the next four minutes evicting live code to stay under a limit
 * that did not exist.
 */
enum {
  kX86pWasmRefusedByEngine = -1, /* the engine would not take another module */
  kX86pWasmRefusedModule = -2    /* this module would not compile or link */
};

typedef struct X86pWasmHost {
  /* Returns the module handle, or one of the negatives above. On refusal, write
     why into `error` (never longer than `error_len`, always terminated). The
     arena repeats it: "a bad module" and "the engine is out of memory" are
     different problems with different fixes, and the one that lost a whole run
     was the one with no words. */
  int (*instantiate)(void *user, const void *bytes, size_t len, char *error, unsigned error_len);
  int (*resolve)(void *user, int module, const char *field);
  /* Returns 0 without changing anything when either module is gone or `to`
     has no export named `to_field`. */
  int (*adopt)(void *user, int to, const char *to_field, int from, const char *from_field, int entry);
  void (*release)(void *user, int module);
  void *user;
} X86pWasmHost;

/*
 * HOW MANY MODULES MAY BE LIVE AT ONCE IS THE CALLER'S TO CHOOSE.
 *
 * It used to be a fixed 1024 here, which quietly made the cap disagree with
 * the block cache the engine was given: a consumer asking for 8192 cached
 * blocks got 8192 cache entries and room for 1024 translations, so seven
 * eighths of its cache could never hold anything and every translation past
 * the thousandth evicted a block the program was still running. Measured in
 * the browser on X-Men Legends II's Dead Zone route, that cost 7,244
 * retranslations per second -- 42.7% of the busy worker's samples inside
 * `new WebAssembly.Module`/`Instance` and the host glue, plus 9.8% in the
 * cache and storage invalidation those evictions drive.
 *
 * So the cap is a parameter, and the engine passes the same number it sized
 * its cache with. The resource being bounded is still inside the WebAssembly
 * engine and still cannot be measured from here; what changed is that a
 * caller who knows its budget can now say so. Measured in Chrome 128, a live
 * module shaped like a translated block -- 1,569 bytes, a shared memory and
 * twelve function imports -- costs about 3.2 KB of renderer memory and 13.7
 * us to compile and instantiate.
 */

typedef struct X86pWasmArenaSlot {
  int live;
  int module;         /* the engine's handle from instantiate() */
  unsigned next_free; /* index + 1 of the next free slot, 0 when this is last */
} X86pWasmArenaSlot;

/* One slot in this many is kept free below a learned ceiling -- see the
   `headroom` field for what working at the ceiling itself was measured to do.
   A ceiling smaller than this divides to no headroom, which is the right
   answer for a host that holds only a handful of modules: there is nothing to
   hold back. */
enum { kX86pWasmCeilingHeadroomDivisor = 16u };

typedef struct X86pWasmArena {
  X86pWasmHost host;
  X86pWasmArenaSlot *slot; /* `capacity` entries, owned */
  unsigned capacity;
  /*
   * A free list, not a scan. Publication used to walk the slots from zero to
   * find a hole, which is O(capacity) per translated block and gets worse
   * exactly as the cap is raised to stop the retranslation it was hiding.
   */
  unsigned free_head; /* index + 1 of the first free slot, 0 when full */
  unsigned live;
  /* The live count the engine last refused to exceed; 0 while none is in
     force.

     It is a hypothesis, not a boundary, and it is retired as soon as the
     arena has backed off below it -- see `ceilings_retired`. Measured in
     Firefox 156: a refusal arrived on a 121,950-byte module with 863 modules
     live, reason "InternalError: out of memory", and the same host had
     already been seen to accept a module again after nothing more than
     releasing a thousand or yielding to the event loop. Held for the run,
     that one refusal cost 1,641 evictions and 51,664 retranslated blocks in a
     five-second window, because the arena spent the run defending a limit the
     engine was no longer applying. */
  unsigned ceiling;
  /* How far below the ceiling the arena keeps itself.
     A ceiling learned from one refusal is not a boundary that holds: measured
     in Firefox, a publication was refused again with the arena already BELOW
     the ceiling it had just learned, after releasing two modules. Whatever the
     engine is counting, it is not exactly this arena's live count, so working
     at the last refusal minus one is a run that refuses, evicts one, refuses
     again and dies. Backing off by a margin gives the eviction something to
     buy: one batch of evictions, then a long run of publications that succeed,
     and if a refusal still comes it arrives lower and ratchets the ceiling
     down again. */
  unsigned headroom;
  unsigned published;        /* modules successfully instantiated over the arena's life */
  unsigned released;         /* modules handed back to the engine */
  unsigned refusals;         /* publications refused because the cap was reached */
  unsigned failures;         /* publications the engine itself rejected */
  unsigned adoptions;        /* entries moved from one module to another */
  unsigned ceilings_learned; /* refusals that put a ceiling in force */
  unsigned ceilings_retired; /* ceilings dropped again after backing off below them */
} X86pWasmArena;

/*
 * Bind an arena to an engine, with room for `capacity` live modules.
 *
 * Returns 0 when the slots could not be allocated, which leaves the arena
 * refusing every publication rather than holding a capacity it does not have.
 * A NULL or incomplete host also leaves it refusing -- what a build with no
 * engine glue must do rather than appear to work until the first block runs.
 */
int x86p_wasm_arena_init(X86pWasmArena *a, const X86pWasmHost *host, unsigned capacity);

/* Release every live module and free the slots. The arena is unusable
   afterwards until it is initialised again. */
void x86p_wasm_arena_dispose(X86pWasmArena *a);

/* How many modules this arena may hold at once. */
unsigned x86p_wasm_arena_capacity(const X86pWasmArena *a);

/*
 * Instantiate a module and return a token for it, or -1 with `reason` set.
 *
 * The token identifies the whole module, not one block: a module built to hold
 * several blocks is one instantiation and is released as one.
 */
int x86p_wasm_arena_publish(X86pWasmArena *a, const void *bytes, size_t len, char *reason, unsigned reason_len);

/*
 * The address a published block may be entered at, for the export named
 * `field` -- x86p_wasm_module_export_name() is what names it. NULL when the
 * token is not live or the export is not there.
 */
void *x86p_wasm_arena_entry(X86pWasmArena *a, int token, const char *field);

/*
 * Move `entry` -- an address a previous x86p_wasm_arena_entry() returned for
 * `from`'s export `from_field` -- onto `to`'s export `to_field`.
 *
 * This is how a block outlives the module it was first published in. The
 * entry's value does not change, so every cached reference and every compiled
 * call that already goes through it keeps working, and `from` may then be
 * released without taking the entry with it.
 *
 * Returns 0, changing nothing, when either token is not live, when the host
 * cannot adopt, or when the export is not there.
 */
int x86p_wasm_arena_adopt(
    X86pWasmArena *a, int to, const char *to_field, int from, const char *from_field, void *entry);

/* Whether this arena's engine can move an entry between modules at all. */
int x86p_wasm_arena_can_adopt(const X86pWasmArena *a);

/* Hand a module back to the engine. Releasing a token twice, or one that was
   never live, does nothing and is not an error: a block cache that discards
   the same block twice is a caller being careful, not a defect. */
void x86p_wasm_arena_release(X86pWasmArena *a, int token);

/* Release every live module. What a guest-memory remap or a cache flush needs,
   because every block was translated against a mapping that no longer holds. */
void x86p_wasm_arena_release_all(X86pWasmArena *a);

unsigned x86p_wasm_arena_live(const X86pWasmArena *a);

/*
 * THE CAP THE CALLER CHOSE IS NOT ALWAYS THE CAP THE ENGINE HAS.
 *
 * A browser may hold far fewer live modules than the caller asked for, and it
 * says so only by refusing one. Measured in Firefox 156: a 65,536-slot arena
 * was refused at 16,111 live modules with `InternalError: out of memory`,
 * roughly three seconds into a run, while Chrome held the same working set
 * without complaint. The arena had room by its own accounting, so nothing
 * evicted, and the refusal ended the run.
 *
 * So the arena LEARNS it: a refused instantiation records the live count as
 * the engine's observed ceiling, and the arena is full at that number until it
 * has backed off below it. The caller's eviction path then does what it
 * already does for a full arena. Once the back-off has happened the ceiling is
 * retired, because the refusals seen in practice do not survive it and a
 * ceiling kept past its back-off throttles the run for good. Zero means no
 * ceiling is in force.
 */
unsigned x86p_wasm_arena_ceiling(const X86pWasmArena *a);

/* How many slots below the ceiling the arena refuses to use. Zero until a
   ceiling has been learned, and zero for a ceiling too small to divide. */
unsigned x86p_wasm_arena_headroom(const X86pWasmArena *a);

/* How many times a refusal put a ceiling in force, and how many of those were
   retired again once the arena had backed off below them. A run whose two
   numbers are equal and large is being throttled by refusals that do not
   survive the back-off; one that learns a ceiling and never retires it is
   sitting under a limit that really holds. */
unsigned x86p_wasm_arena_ceilings_learned(const X86pWasmArena *a);
unsigned x86p_wasm_arena_ceilings_retired(const X86pWasmArena *a);
/* Whether one more module may be published: below the caller's capacity AND
   below any ceiling the engine has shown us. */
int x86p_wasm_arena_has_room(const X86pWasmArena *a);
unsigned x86p_wasm_arena_published(const X86pWasmArena *a);
unsigned x86p_wasm_arena_released(const X86pWasmArena *a);
unsigned x86p_wasm_arena_refusals(const X86pWasmArena *a);
unsigned x86p_wasm_arena_failures(const X86pWasmArena *a);
unsigned x86p_wasm_arena_adoptions(const X86pWasmArena *a);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_ARENA_H */
