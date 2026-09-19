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
 */
typedef struct X86pWasmHost {
  /* On refusal, write why into `error` (never longer than `error_len`,
     always terminated). The arena repeats it: "a bad module" and "the
     engine is out of memory" are different problems with different fixes,
     and the one that lost a whole run was the one with no words. */
  int (*instantiate)(void *user, const void *bytes, size_t len, char *error, unsigned error_len);
  int (*resolve)(void *user, int module, const char *field);
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
  /* The largest live count the engine has refused to exceed; 0 until one is
     observed. Never raised, so a ceiling learned once is honoured for the run. */
  unsigned ceiling;
  unsigned published; /* modules successfully instantiated over the arena's life */
  unsigned released;  /* modules handed back to the engine */
  unsigned refusals;  /* publications refused because the cap was reached */
  unsigned failures;  /* publications the engine itself rejected */
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
 * the engine's observed ceiling, and from then on the arena is full at that
 * number. The caller's eviction path then does what it already does for a full
 * arena. Zero means no ceiling has been observed.
 */
unsigned x86p_wasm_arena_ceiling(const X86pWasmArena *a);
/* Whether one more module may be published: below the caller's capacity AND
   below any ceiling the engine has shown us. */
int x86p_wasm_arena_has_room(const X86pWasmArena *a);
unsigned x86p_wasm_arena_published(const X86pWasmArena *a);
unsigned x86p_wasm_arena_released(const X86pWasmArena *a);
unsigned x86p_wasm_arena_refusals(const X86pWasmArena *a);
unsigned x86p_wasm_arena_failures(const X86pWasmArena *a);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_ARENA_H */
