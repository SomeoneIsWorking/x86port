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
 * These are the framework's existing semantic authorities, imported rather
 * than reimplemented in emitted code for the reason jit_x64.h states: a second
 * implementation of the flag rules is free to disagree, and the disagreement
 * surfaces thousands of instructions later as a branch taken the other way.
 */
typedef enum X86pWasmImport {
  kX86pWasmImportAlu,
  kX86pWasmImportAluUnary,
  kX86pWasmImportCond,
  kX86pWasmImportFlagCf,
  kX86pWasmImportMemOk,
  kX86pWasmImportMemLoad,
  kX86pWasmImportMemStore,
  kX86pWasmImportMultiply,
  kX86pWasmImportDivide,
  kX86pWasmImportString,
  kX86pWasmImportLoop,
  kX86pWasmImportGetFlags,
  kX86pWasmImportSetFlags,
  kX86pWasmImportDoubleShift,
  kX86pWasmImportBit,
  kX86pWasmImportBcd,
  kX86pWasmImportPushad,
  kX86pWasmImportPopad,
  kX86pWasmImportEnter,
  kX86pWasmImportTrap,
  kX86pWasmImportSahf,
  kX86pWasmImportLahf,
  kX86pWasmImportCpuid,
  kX86pWasmImportRdtsc,
  kX86pWasmImportX87LoadBits,
  kX86pWasmImportX87Store,
  kX86pWasmImportX87StoreAt,
  kX86pWasmImportX87ArithMemBits,
  kX86pWasmImportX87ArithReg,
  kX86pWasmImportX87CompareMemBits,
  kX86pWasmImportX87Copy,
  kX86pWasmImportX87Constant,
  kX86pWasmImportX87Status,
  kX86pWasmImportX87Clear,
  kX86pWasmImportX87Reset,
  kX86pWasmImportX87Fn,
  kX86pWasmImportX87Pop,
  kX86pWasmImportX87CompareRegister,
  kX86pWasmImportX87Exchange,
  kX86pWasmImportX87Sign,
  kX86pWasmImportX87Test,
  kX86pWasmImportX87CompareFlags,
  kX86pWasmImportX87Free,
  kX86pWasmImportX87Emms,
  kX86pWasmImportSimdArithmetic,
  kX86pWasmImportChainCall,
  kX86pWasmImportCount
} X86pWasmImport;

/* The field name an import is looked up under, inside module "env". */
const char *x86p_wasm_import_field(X86pWasmImport which);

/*
 * A generic function pointer type, because the addresses below have
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
 *
 * NULL for kX86pWasmImportChainCall alone, which no C function can be: the host
 * binds it to the chain trampoline's export (jit_wasm_chain.h).
 */
X86pWasmImportFn x86p_wasm_import_address(X86pWasmImport which);

/* The module's own memory import, named once for both sides. */
#define X86P_WASM_MEMORY_MODULE "env"
#define X86P_WASM_MEMORY_FIELD "memory"

/*
 * The locals a block body has. Index 0 is the first parameter: the guest
 * X86pCpu address, which on a wasm host is an ordinary 32-bit linear-memory
 * offset. Index 1 is the second, a word the block ignores: it is there so a
 * chained transfer can tail-call the block from the chain trampoline, whose
 * signature it must share (jit_wasm_chain.h). The block zeroes it on entry and
 * uses it as the Addr scratch local, so a chained entry and a dispatched one
 * start from the same locals.
 *
 * The rest are named roles rather than numbers because a collision between two
 * of them is silent -- the block simply computes with the wrong value -- and
 * because a stack machine has no register allocator to catch it.
 */
typedef enum X86pWasmLocal {
  kX86pWasmLocalCpu = 0, /* parameter: the X86pCpu address */
  kX86pWasmLocalAddr,    /* parameter, zeroed: an effective address, then the host address for it */
  kX86pWasmLocalA,       /* the flag tuple's first operand */
  kX86pWasmLocalB,       /* the flag tuple's second operand */
  kX86pWasmLocalR,       /* the flag tuple's result, and the value written back */
  kX86pWasmLocalCarry,   /* the carry-in, live across a bounds check */
  kX86pWasmLocalTarget,  /* a computed guest EIP */
  kX86pWasmLocalCount    /* MUST stay last of the i32 locals */
} X86pWasmLocal;

/*
 * The i64 locals, which follow the i32 ones in one further group.
 *
 * A SECOND TYPE AND NOT A PAIR OF HALVES. An ext80 significand is 64 bits and
 * the widening that builds one is a shift across the whole width, so doing it
 * in i32 halves would mean open-coding the carries -- more emitted code, in the
 * path this exists to make shorter. WebAssembly has i64; the block signature
 * does not change, because these are locals rather than parameters.
 *
 * Numbered after kX86pWasmLocalCount because local indices are one flat space
 * across every group: the i32 group occupies 1..kX86pWasmLocalCount-1 after the
 * cpu parameter, and these continue from there.
 */
typedef enum X86pWasmLocal64 {
  kX86pWasmLocal64Bits = (int)kX86pWasmLocalCount, /* an operand's raw bits, then its significand */
  kX86pWasmLocal64Count                            /* MUST stay last */
} X86pWasmLocal64;

#define X86P_WASM_LOCAL64_GROUP ((uint32_t)kX86pWasmLocal64Count - (uint32_t)kX86pWasmLocalCount)

/*
 * THERE IS NO v128 LOCAL GROUP, AND THAT IS DELIBERATE.
 *
 * The packed lowering in jit_wasm_simd_inline.c keeps both of its operands on
 * the stack between their loads and the one store that consumes them, so it
 * needs no local of that type. Declaring one anyway would put a zeroed 16-byte
 * slot in EVERY block body -- including the overwhelming majority that contain
 * no SSE at all -- and a block body is entered hundreds of millions of times on
 * a real route. It would also make every translated block require the
 * fixed-width SIMD feature from the engine, where as things stand only a block
 * that actually contains a packed instruction does.
 */

/*
 * The block function's signature, as an index into the module's type section:
 * two i32 parameters (the cpu and the ignored word) and an i32 result.
 * Published because x86p_jit_enter has to call it and the arena has to describe
 * it to the engine.
 */
#define X86P_WASM_BLOCK_TYPE 1u

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
