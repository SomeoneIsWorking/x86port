/*
 * jit_wasm_module.h -- the module a translated block lives in.
 *
 * WHY THIS IS AN OBJECT AND NOT A FUNCTION. On every other host a translated
 * block is bytes in a buffer that the caller makes executable. WebAssembly has
 * no such thing: the engine only accepts a whole MODULE, and it accepts it
 * once. So the unit this backend produces is a module, several blocks may
 * share one, and the thing that knows the module's shape -- how many function
 * bodies it promised, which type each has, which import sits at which index --
 * has to persist across the lowering of each of those blocks. That is state
 * with an identity, so it is an instance, and every operation takes it.
 *
 * THE FUNCTION COUNT IS DECLARED UP FRONT, and that is the format's rule
 * rather than a convenience. A section is length-prefixed and its element
 * count is a plain LEB128 written before the elements -- there is no slot to
 * patch afterwards. The function and export sections both precede the code
 * section, so the number of bodies is fixed before the first one is lowered.
 * A caller that wants to batch blocks decides how many it is batching first.
 *
 * WHY BATCHING IS A CORRECTNESS CONCERN AND NOT TUNING. An instantiated module
 * is permanent for as long as anything can reach it; the engine has no
 * "unload". One module per translated block therefore costs a permanent engine
 * object per block, and a play session translates tens of thousands. The
 * lifetime is owned by jit_wasm_arena.h, which refuses rather than growing
 * without bound; this file is the half that lets a caller put many blocks in
 * one module in the first place.
 */
#ifndef X86PORT_JIT_WASM_MODULE_H
#define X86PORT_JIT_WASM_MODULE_H

#include "emit_wasm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The functions an emitted module imports, in the order that fixes their
 * function indices. This enum IS the contract between the emitted code and
 * whatever instantiates it: the host builds its import object from
 * x86p_wasm_import_field() and x86p_wasm_import_address(), so neither side
 * spells a name the other does not know.
 *
 * All four are the framework's existing semantic authorities, imported rather
 * than reimplemented in emitted code for the reason jit_x64.h states: a second
 * implementation of the flag rules is free to disagree, and the disagreement
 * surfaces thousands of instructions later as a branch taken the other way.
 */
typedef enum X86pWasmImport {
  kX86pWasmImportAlu = 0,  /* x86p_alu(op, a, b, w, flags) -> result */
  kX86pWasmImportAluUnary, /* x86p_alu_unary(op, a, w, flags) -> result */
  kX86pWasmImportCond,     /* x86p_cond(cc, flags) -> 0 or 1 */
  kX86pWasmImportFlagCf,   /* x86p_flag_cf(flags) -> 0 or 1 */
  kX86pWasmImportCount     /* MUST stay last */
} X86pWasmImport;

/* The field name an import is looked up under, inside module "env". */
const char *x86p_wasm_import_field(X86pWasmImport which);

/*
 * A generic function pointer type, because the addresses below have four
 * different signatures and ISO C has no single pointer that fits them all.
 * The host casts to the signature the enum documents.
 */
typedef void (*X86pWasmImportFn)(void);

/*
 * The C function an import must be bound to.
 *
 * Published so the host binds ADDRESSES rather than matching names against its
 * own symbol table: on the wasm host these are indirect-table indices, and a
 * glue layer that looked the name up itself would be a second opinion about
 * which implementation the emitted code calls.
 */
X86pWasmImportFn x86p_wasm_import_address(X86pWasmImport which);

/* The module's own memory import, named once for both sides. */
#define X86P_WASM_MEMORY_MODULE "env"
#define X86P_WASM_MEMORY_FIELD "memory"

/*
 * The locals a block body has. Index 0 is the parameter: the guest X86pCpu
 * address, which on a wasm host is an ordinary 32-bit linear-memory offset.
 *
 * The rest are named roles rather than numbers because a collision between two
 * of them is silent -- the block simply computes with the wrong value -- and
 * because a stack machine has no register allocator to catch it.
 */
typedef enum X86pWasmLocal {
  kX86pWasmLocalCpu = 0, /* parameter: the X86pCpu address */
  kX86pWasmLocalAddr,    /* an effective address, then the host address for it */
  kX86pWasmLocalA,       /* the flag tuple's first operand */
  kX86pWasmLocalB,       /* the flag tuple's second operand */
  kX86pWasmLocalR,       /* the flag tuple's result, and the value written back */
  kX86pWasmLocalCarry,   /* the carry-in, live across a bounds check */
  kX86pWasmLocalTarget,  /* a computed guest EIP */
  kX86pWasmLocalCount    /* MUST stay last */
} X86pWasmLocal;

/*
 * The block function's signature, as an index into the module's type section.
 * Published because x86p_jit_enter has to call it and the arena has to describe
 * it to the engine.
 */
#define X86P_WASM_BLOCK_TYPE 0u

/* The most bodies one module may hold. */
#define X86P_WASM_MAX_BODIES 64u

/*
 * The export name of the `index`th body: "b0", "b1", ... NULL past the cap.
 *
 * A free function rather than a per-module formatter, because the name is what
 * a PUBLISHER asks the engine for and the publisher does not hold the builder
 * any more -- and a second site that formed the name itself would be free to
 * format it differently from the export section that carries it.
 */
const char *x86p_wasm_body_name(unsigned index);

typedef struct X86pWasmModule {
  X86pWasmEmit e;
  X86pWasmSize code_section;
  /* The open body's size slot. Held here rather than handed to the caller so
     bodies cannot be closed out of the order they were opened in. */
  X86pWasmSize body;
  unsigned promised; /* bodies the function/export sections declared */
  unsigned written;  /* bodies actually appended */
  int open;          /* a body is currently open */
  int finished;
} X86pWasmModule;

/*
 * Begin a module in `buf` that will hold exactly `functions` block bodies.
 *
 * Writes the type, import, function and export sections and opens the code
 * section, so the next thing a caller does is append that many bodies. A
 * `functions` of zero, or one above X86P_WASM_MAX_BODIES, is a caller defect
 * and leaves the module refusing -- x86p_wasm_module_finish then returns 0
 * rather than a module the engine would reject for a reason nobody recorded.
 */
void x86p_wasm_module_init(X86pWasmModule *m, void *buf, size_t cap, unsigned functions);

/* x86p_wasm_body_name, restricted to the bodies THIS module promised: NULL for
   an index it does not have, which is how a caller asking for a body that was
   never written finds out rather than guessing a name. */
const char *x86p_wasm_module_export_name(const X86pWasmModule *m, unsigned index);

/* The emitter, for the lowering units that write instructions into a body. */
X86pWasmEmit *x86p_wasm_module_emitter(X86pWasmModule *m);

/*
 * Open the next block body, with the full local frame declared. Returns the
 * index of the body being written, or -1 when the module already holds every
 * body it promised (or is already refusing).
 */
int x86p_wasm_module_body_begin(X86pWasmModule *m);

/* Close the body opened by body_begin. */
void x86p_wasm_module_body_end(X86pWasmModule *m);

/*
 * Close the code section and return the module's length in bytes, or 0 when
 * anything went wrong -- an overflowed buffer, an unbalanced region, or fewer
 * bodies than the sections promised. Zero is the ONLY success-looking value a
 * broken module can produce, so a caller cannot accidentally hand the engine a
 * prefix of one.
 */
size_t x86p_wasm_module_finish(X86pWasmModule *m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_MODULE_H */
