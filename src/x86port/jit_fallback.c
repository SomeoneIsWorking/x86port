#include "jit_fallback.h"

#include "decode.h"
#include "insn_exec.h"

#include <stdlib.h>

/* Distinct fallback addresses tracked per reason; later ones are counted as dropped keys. */
#define FALLBACK_SITES_PER_REASON 64u

struct X86pJitFallback {
  uint64_t blocks[kX86pJitFallbackReasonCount];
  uint64_t insns[kX86pJitFallbackReasonCount];
  X86pJitProfile *sites[kX86pJitFallbackReasonCount];
};

X86pJitFallback *x86p_jit_fallback_create(void) {
  X86pJitFallback *f = (X86pJitFallback *)calloc(1u, sizeof *f);
  unsigned r;
  if (!f) {
    return NULL;
  }
  for (r = 0u; r < (unsigned)kX86pJitFallbackReasonCount; r++) {
    f->sites[r] = x86p_jit_profile_create(FALLBACK_SITES_PER_REASON);
    if (!f->sites[r]) {
      x86p_jit_fallback_destroy(f);
      return NULL;
    }
  }
  return f;
}

void x86p_jit_fallback_destroy(X86pJitFallback *f) {
  unsigned r;
  if (!f) {
    return;
  }
  for (r = 0u; r < (unsigned)kX86pJitFallbackReasonCount; r++) {
    x86p_jit_profile_destroy(f->sites[r]);
  }
  free(f);
}

static X86pJitExit exit_for_step(X86pStepStatus s) {
  switch (s) {
  case kX86pStepOk:
    return kX86pJitExitBlockEnd;
  case kX86pStepMemoryFault:
    return kX86pJitExitMemoryFault;
  case kX86pStepDivideError:
    return kX86pJitExitDivideError;
  case kX86pStepInterrupt:
    return kX86pJitExitInterrupt;
  case kX86pStepProtectionFault:
    return kX86pJitExitProtectionFault;
  case kX86pStepBoundRange:
    return kX86pJitExitBoundRange;
  case kX86pStepDecodeFailed:
  case kX86pStepUnsupported:
  case kX86pStepFetchFault:
  case kX86pStepStatusCount:
    break;
  }
  return kX86pJitExitUnsupported;
}

/* Decode the instruction at `eip` if this fallback entry may run it. */
static int refused_insn_at(const X86pMem *mem, uint32_t eip, X86pInsn *insn) {
  uint8_t bytes[X86P_MAX_INSN_LEN];
  const uint32_t avail = x86p_fetch_insn_bytes(mem, eip, bytes);
  return avail != 0u && x86p_decode(bytes, avail, insn) != 0u && !x86p_jit_can_translate(insn);
}

X86pJitExit x86p_jit_fallback_run(X86pJitFallback *f,
                                  X86pJitFallbackReason reason,
                                  const X86pMem *mem,
                                  X86pJitBoundaryFn boundary,
                                  void *boundary_user,
                                  X86pCpu *cpu) {
  const uint32_t start = cpu->eip;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  uint32_t n;

  for (n = 0u; n < X86P_JIT_FALLBACK_MAX_INSNS; n++) {
    const uint32_t eip = cpu->eip;
    X86pInsn insn;
    uint32_t fault_addr = 0u;
    if (n != 0u && boundary && boundary(eip, boundary_user)) {
      break;
    }
    if (!refused_insn_at(mem, eip, &insn) || insn.op == (uint8_t)kX86pInsnUnsupported) {
      if (n == 0u) {
        return kX86pJitExitUnsupported;
      }
      break;
    }
    const X86pJitExit outcome = exit_for_step(x86p_execute_decoded(cpu, mem, &insn, &fault_addr));
    if (outcome == kX86pJitExitUnsupported) {
      if (n == 0u) {
        return kX86pJitExitUnsupported;
      }
      break;
    }
    if (outcome != kX86pJitExitBlockEnd) {
      /* EIP stays on the instruction, as a translated block leaves it. */
      exit = outcome;
      break;
    }
    if (cpu->eip != eip + insn.length) {
      n++;
      break;
    }
  }

  f->blocks[reason]++;
  f->insns[reason] += n;
  x86p_jit_profile_hit(f->sites[reason], start);
  return exit;
}

void x86p_jit_fallback_stats(const X86pJitFallback *f, X86pJitEngineStats *stats) {
  unsigned r;
  for (r = 0u; r < (unsigned)kX86pJitFallbackReasonCount; r++) {
    stats->fallback_blocks[r] = f->blocks[r];
    stats->fallback_insns[r] = f->insns[r];
  }
}

const X86pJitProfile *x86p_jit_fallback_sites(const X86pJitFallback *f, X86pJitFallbackReason reason) {
  return f && (unsigned)reason < (unsigned)kX86pJitFallbackReasonCount ? f->sites[reason] : NULL;
}
