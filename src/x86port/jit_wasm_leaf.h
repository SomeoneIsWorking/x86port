/*
 * jit_wasm_leaf.h -- leaves on the WebAssembly host (jit_x64.h, X86pJitLeafFn).
 *
 * A CALL whose callee the consumer completes in host code -- a native override
 * or a thunk small enough that leaving the block costs more than its body --
 * calls that code from inside the block, as the x86-64 backend does, instead
 * of returning to the dispatcher to be handed to it. A leaf that completes
 * returns into the block, which leaves through a chained exit to the return
 * address; one that declines changed nothing, and the CALL leaves as an
 * ordinary CALL would.
 *
 * WHY IT MATTERS MORE HERE. Every return to the dispatcher is a return out of
 * a tail-called chain into C and a lookup back in: measured in Chrome before
 * this existed, the same guest code handed back 24,000 times a frame where the
 * native product, whose blocks call leaves, handed back 1,700.
 *
 * HOW A LEAF IS CALLED. A leaf is a C function of the main module; on this
 * host its pointer IS its index in that module's function table. A block
 * module must not import the table (jit_wasm_chain.h says why), so the block
 * passes the index to one import, kX86pWasmImportLeafCall, which makes the
 * indirect call inside the main module. A CALL through a register or memory
 * finds its leaf through a site (jit_leaf_sites.h) that the block reads as
 * plain linear memory, and asks kX86pWasmImportLeafSiteFill only for a target
 * the site has not seen.
 *
 * Only a chaining block calls leaves: the return lands on a chained exit, under
 * the run's stop and budget like any transfer.
 */
#ifndef X86PORT_JIT_WASM_LEAF_H
#define X86PORT_JIT_WASM_LEAF_H

#include "cpu.h"
#include "jit_x64.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct X86pJitLeafSite;
struct X86pWasmLower;

/*
 * How one block's CALLs find their leaves. `resolve` NULL: none. A first
 * lowering claims a site from `sites` for a CALL through a register or memory;
 * a RELOWERING (jit_wasm_compact.h) claims nothing and gives that CALL `site`,
 * the one it was published with, so the rebuilt body reads the same site and
 * the pool is not spent twice for one block.
 */
typedef struct X86pWasmLeafUse {
  X86pJitLeafResolveFn resolve;
  void *user;
  X86pJitLeafSites *sites;
  int relowering;
  struct X86pJitLeafSite *site;
} X86pWasmLeafUse;

/*
 * Lower the CALL at `pc`, whose return address is already pushed, when it has a
 * leaf: a direct CALL whose target `resolve` names one, or an indirect CALL
 * (its target in kX86pWasmLocalTarget) given a site. Emits the block's exits
 * and returns 1; returns 0, having emitted nothing, for a CALL that leaves the
 * ordinary way.
 */
int x86p_wasm_leaf_call_lower(struct X86pWasmLower *l, uint32_t target, uint32_t next, int indirect);

/* The import behind kX86pWasmImportLeafCall: `leaf(cpu)`. */
int x86p_wasm_leaf_call(X86pCpu *cpu, X86pJitLeafFn leaf);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_JIT_WASM_LEAF_H */
