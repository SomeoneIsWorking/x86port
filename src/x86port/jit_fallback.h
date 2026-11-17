/*
 * jit_fallback.h -- the product dispatcher's bounded fallback.
 *
 * Entered from exactly one edge in x86p_jit_engine_run, after the backend's
 * translation gate refused an instruction: at a block's entry, or where an
 * emitted block stopped. It runs that
 * refused block through the one semantic executor (insn_exec.h) and hands
 * control back to JIT dispatch. It is not an engine: every instruction it
 * runs is one the backend's translation gate refuses, and it stops after at
 * most X86P_JIT_FALLBACK_MAX_INSNS of them.
 *
 * A block here ends at the first of: a control transfer it executed, an
 * instruction the backend can translate, a consumer interception point, an
 * unmapped or undecodable next instruction, a guest fault, or the bound. Each
 * entry is counted by reason and by guest address.
 */
#ifndef X86PORT_JIT_FALLBACK_H
#define X86PORT_JIT_FALLBACK_H

#include "cpu.h"
#include "jit_engine.h"
#include "jit_profile.h"
#include "jit_x64.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Instructions one fallback entry may execute before it returns to dispatch. */
#define X86P_JIT_FALLBACK_MAX_INSNS 32u

typedef struct X86pJitFallback X86pJitFallback;

X86pJitFallback *x86p_jit_fallback_create(void);
void x86p_jit_fallback_destroy(X86pJitFallback *f);

/*
 * Run the refused block at cpu->eip.
 *
 * Returns kX86pJitExitBlockEnd when the run goes back to JIT dispatch at the
 * new cpu->eip. kX86pJitExitUnsupported means nothing ran and the refusal
 * stands: the first instruction has no semantics either, or the gate accepts
 * it and the backend refused the block for another reason. Any other exit is the guest outcome of the
 * instruction at cpu->eip, as a translated block reports it.
 */
X86pJitExit x86p_jit_fallback_run(X86pJitFallback *f,
                                  X86pJitFallbackReason reason,
                                  const X86pMem *mem,
                                  X86pJitBoundaryFn boundary,
                                  void *boundary_user,
                                  X86pCpu *cpu);

/* Write this owner's per-reason totals into `stats`. */
void x86p_jit_fallback_stats(const X86pJitFallback *f, X86pJitEngineStats *stats);

/* Fallback entries for `reason`, keyed by the guest address each began at. */
const X86pJitProfile *x86p_jit_fallback_sites(const X86pJitFallback *f, X86pJitFallbackReason reason);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_FALLBACK_H */
