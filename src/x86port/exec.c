/* exec.c -- the test interpreter's step; the semantics are insn_exec.c's. */
#include "exec.h"

#include <string.h>

X86pStepStatus x86p_step(X86pCpu *cpu, const X86pMem *mem, X86pStepReport *report) {
  return x86p_step_cached(cpu, mem, NULL, report);
}

X86pStepStatus x86p_step_cached(X86pCpu *cpu, const X86pMem *mem, X86pDecodeCache *cache, X86pStepReport *report) {
  uint8_t bytes[X86P_MAX_INSN_LEN];
  X86pInsn insn;
  uint32_t avail;
  uint32_t fault_addr = 0u;
  X86pStepStatus st;

  if (report) {
    memset(report, 0, sizeof *report);
    report->mnemonic = "?";
  }
  if (!cpu) {
    return kX86pStepFetchFault;
  }
  if (report) {
    report->eip = cpu->eip;
  }

  avail = x86p_fetch_insn_bytes(mem, cpu->eip, bytes);
  if (avail == 0u) {
    if (report) {
      report->status = kX86pStepFetchFault;
      report->fault_addr = cpu->eip;
    }
    return kX86pStepFetchFault;
  }

  if (!x86p_decode_cached(cache, cpu->eip, bytes, avail, &insn)) {
    if (report) {
      report->status = kX86pStepDecodeFailed;
      report->fault_addr = cpu->eip;
    }
    return kX86pStepDecodeFailed;
  }
  if (report) {
    report->length = insn.length;
    report->mnemonic = insn.mnemonic;
    report->op = insn.op;
  }
  if (insn.op == kX86pInsnUnsupported) {
    /* EIP stays on it so the caller can report it or add semantics for it. */
    if (report) {
      report->status = kX86pStepUnsupported;
    }
    return kX86pStepUnsupported;
  }

  st = x86p_execute_decoded(cpu, mem, &insn, &fault_addr);
  if (report) {
    report->status = (uint8_t)st;
    if (st != kX86pStepOk) {
      report->fault_addr = fault_addr;
    }
  }
  return st;
}
