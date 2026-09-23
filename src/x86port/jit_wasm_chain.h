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
 * transfer is a call_indirect whose result the exiting block returns. Each
 * transfer therefore holds one engine frame until the chain returns, and the
 * run's budget -- which the engine caps at X86P_WASM_CHAIN_TRANSFERS on this
 * host -- is what bounds that depth.
 *
 * WHY A BLOCK CHAINS AT MOST X86P_WASM_CHAIN_SLOTS EXITS. A conditional branch
 * does not end a block here (jit_wasm_state.h, X86pWasmExitCensus), so a block
 * has as many exits as branches, and slots come back only when the code arena
 * is flushed. The engine sizes its slot table by this cap; an exit past it
 * returns to the dispatcher and is counted as unslotted.
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
/* The most transfers one dispatch may make: each holds an engine frame. */
#define X86P_WASM_CHAIN_TRANSFERS 256u
/* One chained exit's instructions beyond a plain exit, with room to spare. */
#define X86P_WASM_CHAIN_EXIT_BYTES 160u

/* Where a block's exits find their slots. */
typedef struct X86pWasmChainUse {
  X86pJitChain *chain; /* NULL: every exit returns to the dispatcher */
  /* At or above zero: the block is being relowered, and its exits take its
     `reuse_count` published slots from here in order instead of claiming. */
  int64_t reuse_first;
  unsigned reuse_count;
} X86pWasmChainUse;

typedef struct X86pWasmChainExits {
  X86pWasmChainUse use;
  int64_t first;      /* the block's first slot, or -1 */
  unsigned slotted;   /* exits given a slot */
  unsigned unslotted; /* exits that asked for one and had none */
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
 */
void x86p_wasm_chain_emit(X86pWasmChainExits *c, X86pWasmEmit *e, uint32_t imm, int local);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_JIT_WASM_CHAIN_H */
