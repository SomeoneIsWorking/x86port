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
  int (*instantiate)(void *user, const void *bytes, size_t len);
  int (*resolve)(void *user, int module, const char *field);
  void (*release)(void *user, int module);
  void *user;
} X86pWasmHost;

/*
 * How many modules may be live at once.
 *
 * A number rather than "as many as fit", because the resource being bounded is
 * inside the engine and nothing here can measure it. Browser gameplay has not
 * qualified a larger cap: doubling it raised renderer memory substantially
 * without producing playable frame times.
 */
#define X86P_WASM_MAX_LIVE_MODULES 1024u

typedef struct X86pWasmArenaSlot {
  int live;
  int module; /* the engine's handle from instantiate() */
} X86pWasmArenaSlot;

typedef struct X86pWasmArena {
  X86pWasmHost host;
  X86pWasmArenaSlot slot[X86P_WASM_MAX_LIVE_MODULES];
  unsigned live;
  unsigned published; /* modules successfully instantiated over the arena's life */
  unsigned released;  /* modules handed back to the engine */
  unsigned refusals;  /* publications refused because the cap was reached */
  unsigned failures;  /* publications the engine itself rejected */
} X86pWasmArena;

/* Bind an arena to an engine. A NULL or incomplete host leaves the arena
   refusing every publication, which is what a build with no engine glue must
   do rather than appear to work until the first block runs. */
void x86p_wasm_arena_init(X86pWasmArena *a, const X86pWasmHost *host);

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
unsigned x86p_wasm_arena_published(const X86pWasmArena *a);
unsigned x86p_wasm_arena_released(const X86pWasmArena *a);
unsigned x86p_wasm_arena_refusals(const X86pWasmArena *a);
unsigned x86p_wasm_arena_failures(const X86pWasmArena *a);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_ARENA_H */
