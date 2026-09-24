/*
 * jit_wasm_lower.h -- guest basic block to a WebAssembly function body.
 *
 * The WebAssembly counterpart of jit_x64.c's translation half, with one
 * deliberate difference: THIS IS NOT THE x86p_jit_* BACKEND. It produces a
 * function body inside a module and takes the guest mapping as three plain
 * integers, so it neither knows nor needs to know that the host it runs on is
 * the host that will execute the result.
 *
 * That separation is what makes the lowering testable. A machine-code backend
 * can only be exercised on the architecture it emits for; a module can be
 * built anywhere and handed to any engine, so the differential against the
 * interpreter oracle runs on a developer machine with no Emscripten toolchain
 * at all. jit_wasm.c is the thin adapter that presents this as the
 * x86p_jit_translate contract on a wasm host, and it is the only part of the
 * backend that cannot be built and run everywhere.
 *
 * WHAT IS INLINED AND WHAT IS CALLED. The same split the other backends make,
 * for the same reason: data movement, address arithmetic and the ALU shapes
 * whose flag rules are a plain tuple are emitted inline; ADC, SBB, the shifts
 * and the rotates call the one semantic authority (x86p_alu) rather than
 * becoming a second implementation of the eager flag derivation. The imports
 * that make those calls possible are listed in jit_wasm_module.h.
 */
#ifndef X86PORT_JIT_WASM_LOWER_H
#define X86PORT_JIT_WASM_LOWER_H

#include "cpu.h"
#include "decode.h"
#include "jit_wasm_module.h"
#include "jit_wasm_state.h"
#include "jit_x64.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest instructions per lowered block. */
#define X86P_WASM_MAX_INSNS 64

/*
 * The most bytes one guest instruction lowers to.
 *
 * Exported for the same reason the machine-code backends export theirs: a
 * caller has to decide when its buffer is too full to lower into, and a number
 * it chose itself would be a second opinion about this file's worst case.
 *
 * ENFORCED, not estimated: the lowering refuses a block holding an instruction
 * past it (jit_wasm_lower.c), so every form a test lowers is measured. The
 * largest are the binary64 x87 arithmetic (jit_wasm_x87_arith.h), 470 bytes,
 * and the inline FSTP, 378; at 320 this was already short of the FSTP.
 */
#define X86P_WASM_WORST_CASE_INSN_BYTES 512u
/* CPU local, EIP/status constants, store, return and body end. */
#define X86P_WASM_EXIT_BYTES 24u
/* Framing/types/exports plus a bounded field name and import descriptor per helper. */
#define X86P_WASM_MODULE_OVERHEAD_BYTES (512u + 48u * (unsigned)kX86pWasmImportCount)
#define X86P_WASM_MIN_MODULE_BYTES                                                                                     \
  (X86P_WASM_WORST_CASE_INSN_BYTES + X86P_WASM_MODULE_OVERHEAD_BYTES + X86P_WASM_EXIT_BYTES +                          \
   X86P_WASM_CHAIN_SLOTS * X86P_WASM_CHAIN_EXIT_BYTES)
/*
 * The largest a single lowered module can be, which is what a caller has to
 * size a scratch buffer to. A block holds at most X86P_WASM_MAX_INSNS guest
 * instructions, so the worst case is that many at their worst case plus the
 * framing -- not the caller's whole live-code budget, which bounds a different
 * resource entirely.
 */
#define X86P_WASM_MAX_MODULE_BYTES                                                                                     \
  ((size_t)X86P_WASM_MAX_INSNS * X86P_WASM_WORST_CASE_INSN_BYTES + X86P_WASM_MODULE_OVERHEAD_BYTES +                   \
   X86P_WASM_EXIT_BYTES + (size_t)X86P_WASM_CHAIN_SLOTS * X86P_WASM_CHAIN_EXIT_BYTES)

/*
 * Where the guest memory a block is lowered against will live in the ENGINE's
 * linear memory, derived from the host mapping it is lowered FROM.
 *
 * One owner, because the two differ only in what a sparse mapping needs and a
 * second copy of that rule is a second answer to it. Every caller that lowers
 * a block -- one at a time or a batch into one module -- goes through this.
 */
void x86p_wasm_plan_from_mem(const X86pMem *mem, X86pWasmPlan *plan);

/*
 * Lower the basic block at `eip` into the module's next function body.
 *
 * `fetch` is where the guest BYTES are read from at lowering time -- an
 * ordinary host mapping, with a host pointer. `plan` describes where that same
 * guest memory will live in the ENGINE's linear memory when the block runs.
 * On a wasm host they describe the same mapping and jit_wasm.c derives one
 * from the other; keeping them apart is what lets a test lower against an
 * image it merely holds the bytes of.
 *
 * `chain` names the slots its exits chain through, or is NULL (jit_wasm_chain.h).
 *
 * `out->entry` is NOT set: a module is not an address, and it becomes callable
 * only once the engine has instantiated it. jit_wasm_arena.h owns that step.
 * Every other field of `out` is filled in as the other backends fill it.
 */
X86pJitStatus x86p_wasm_lower_block(X86pWasmModule *m,
                                    const X86pMem *fetch,
                                    const X86pWasmPlan *plan,
                                    uint32_t eip,
                                    X86pJitBoundaryFn boundary,
                                    void *boundary_user,
                                    const X86pWasmChainUse *chain,
                                    X86pJitBlock *out,
                                    char *reason,
                                    unsigned reason_len);

/*
 * Would this instruction be lowered, if a block reached it?
 *
 * The honest denominator for a corpus count: the ranked list of block ENDERS
 * undercounts, because everything after the first refusal in a function is
 * never looked at.
 */
int x86p_wasm_can_lower(const X86pInsn *insn);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_WASM_LOWER_H */
