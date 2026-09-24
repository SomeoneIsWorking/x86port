/*
 * jit_wasm_chain.h -- chained exits on the WebAssembly host (jit_chain.h).
 *
 * A block's exit to a next guest EIP asks its slot, exactly as the machine-code
 * backends' exits do: linked, not the run's stop address, and budget left ->
 * the slot's translation runs now; otherwise the exit names its slot as
 * pending and returns to the dispatcher, which links it.
 *
 * WHAT DIFFERS IS THE TRANSFER. WebAssembly has no jump into another
 * function, so a slot's `host` is the target's indirect-table index and the
 * transfer is a TAIL call (the tail-call proposal, in every browser with
 * WebGPU): the exiting block's frame is gone before its successor runs, so a
 * chain of any length runs in constant stack. With ordinary calls each
 * transfer held two frames, and a guest worker in Chrome overflowed its stack
 * at 256 transfers per dispatch once RETs chained too.
 *
 * THE CALL GOES THROUGH A TRAMPOLINE, NOT THE TABLE. A block module that
 * imports the host's function table makes V8 keep a dispatch table for that
 * instance and regrow it every time the table grows -- and publishing a block
 * grows it. Measured in Chrome: thousands of block modules importing it ran the
 * renderer out of memory in WasmDispatchTable::Grow seconds into a run. So the
 * exit tail-calls one import, kX86pWasmImportChainCall, and only the module
 * behind it imports the table.
 *
 * THAT MODULE IS NOT THE MAIN ONE. The trampoline is a module of its own, which
 * each host instantiates once (x86p_wasm_chain_trampoline), because the main
 * module cannot hold a tail call: Binaryen's asyncify pass, which the browser
 * product needs for its blocking calls, refuses any function that has one --
 * its remove list included ("tail calls not yet supported in asyncify").
 *
 * WHY A BLOCK CHAINS AT MOST X86P_WASM_CHAIN_SLOTS EXITS. A conditional branch
 * does not end a block here (jit_wasm_state.h, X86pWasmExitCensus), so a block
 * has as many exits as branches, and slots come back only when the code arena
 * is flushed. The engine sizes its slot table by this cap; an exit past it
 * returns to the dispatcher and is counted as unslotted.
 *
 * SIBLING CALLS SKIP THE TRAMPOLINE. The trampoline is a call into another
 * instance and an indirect call out of it, on every transfer: about 4% of the
 * browser's guest-worker samples. A block relowered into a shared module
 * (jit_wasm_compact.h) knows the other blocks lowered with it, so an exit to
 * one of their addresses compares its slot's host with that block's entry and,
 * when they match, tail-calls the body in its own module. The match proves
 * the call is the one the table would have made: a module owns the entries of
 * its blocks until it is released as a whole, so while the calling body
 * exists no other block can be entered through that entry. Any other host --
 * a retranslation, a block in another module -- goes through the trampoline.
 *
 * WHY A RELOWERED BLOCK REUSES ITS SLOTS. The storage relowers published
 * blocks into one shared module (jit_wasm_compact.h) and keeps their table
 * entries, so the links that name those entries stay right. Claiming fresh
 * slots there would spend a second set per block and flush twice as often.
 */
#ifndef X86PORT_JIT_WASM_CHAIN_H
#define X86PORT_JIT_WASM_CHAIN_H

#include "emit_wasm.h"
#include "jit_chain.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The most exits of one block that chain. */
#define X86P_WASM_CHAIN_SLOTS 4u
/* One chained exit's instructions beyond a plain exit, its front-array probe
   included, with room to spare. */
#define X86P_WASM_CHAIN_EXIT_BYTES 320u

/*
 * A block lowered into the same module as the exit being emitted: its guest
 * address, the table entry it is entered through, and its function index in
 * that module.
 */
typedef struct X86pWasmChainSibling {
  uint32_t guest;
  uint32_t entry;
  uint32_t function;
} X86pWasmChainSibling;

/* Where a block's exits find their slots. */
typedef struct X86pWasmChainUse {
  X86pJitChain *chain; /* NULL: every exit returns to the dispatcher */
  /* At or above zero: the block is being relowered, and its exits take its
     `reuse_count` published slots from here in order instead of claiming. */
  int64_t reuse_first;
  unsigned reuse_count;
  /* The blocks sharing this module, the block itself included, or NULL. An
     exit to one of their addresses whose slot names that block's entry calls
     it directly (SIBLING CALLS, above). */
  const X86pWasmChainSibling *siblings;
  unsigned sibling_count;
} X86pWasmChainUse;

typedef struct X86pWasmChainExits {
  X86pWasmChainUse use;
  int64_t first;      /* the block's first slot, or -1 */
  unsigned slotted;   /* exits given a slot */
  unsigned unslotted; /* exits that asked for one and had none */
  /* Bytes the chained attempts took, which x86p_wasm_chain_reserve set aside
     apart from their instructions' own, and the first attempt that took more
     than X86P_WASM_CHAIN_EXIT_BYTES of them, or 0. */
  size_t bytes;
  size_t oversized;
  /* Exits that call a sibling directly when their slot names it. */
  unsigned direct;
} X86pWasmChainExits;

/* `use` may be NULL, for a block that does not chain. */
void x86p_wasm_chain_exits_init(X86pWasmChainExits *c, const X86pWasmChainUse *use);

/* Bytes to keep free for the chained exits this block may still emit. */
size_t x86p_wasm_chain_reserve(const X86pWasmChainExits *c);

/*
 * Emit the chained attempt of one exit to the guest EIP that `imm` names, or
 * that local `local` holds when `local` is not negative. Called with cpu->eip
 * already stored; leaves the stack as it found it. Emits nothing when the
 * block does not chain or has no slot left.
 *
 * An exit to a computed EIP -- a RET, an indirect JMP or CALL -- that misses
 * its slot then asks the block cache's front array (jit_chain.h, THE PROBE),
 * which clobbers kX86pWasmLocalAddr; `local` must be another local.
 */
void x86p_wasm_chain_emit(X86pWasmChainExits *c, X86pWasmEmit *e, uint32_t imm, int local);

/* The trampoline module's table import, in module X86P_WASM_MEMORY_MODULE,
   and its one export: the host binds kX86pWasmImportChainCall to it. */
#define X86P_WASM_CHAIN_TABLE_FIELD "table"
#define X86P_WASM_CHAIN_TRAMPOLINE_EXPORT "chain_call"

/*
 * Write the trampoline module into `buf`: one function of a block's signature,
 * (cpu, index) -> exit, that tail-calls table entry `index` with the same two
 * words. Returns its length, or 0 when `cap` is too small.
 */
size_t x86p_wasm_chain_trampoline(void *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_JIT_WASM_CHAIN_H */
