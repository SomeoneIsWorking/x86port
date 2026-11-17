/*
 * insn_exec.h -- the meaning of one decoded guest instruction.
 *
 * This is the one implementation of each instruction's semantics outside the
 * emitters' inline forms. Two callers share it: the product JIT's bounded
 * fallback (jit_fallback.h), which runs it only for a block the backend
 * refused, and the separately linked test interpreter (exec.h), which fetches,
 * decodes and runs it one instruction at a time. It never fetches or decodes
 * the next instruction itself.
 *
 * EVERY OUTCOME IS NAMED. "Ran it" and "skipped it" produce the same register
 * file for exactly the instructions whose effects are subtle, so an
 * instruction without semantics is reported, never stepped over.
 */
#ifndef X86PORT_INSN_EXEC_H
#define X86PORT_INSN_EXEC_H

#include "cpu.h"
#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86pStepStatus {
  kX86pStepOk = 0,
  kX86pStepDecodeFailed, /* the bytes at EIP are not an instruction */
  kX86pStepUnsupported,  /* decoded and named, but no semantics in this build */
  kX86pStepFetchFault,   /* EIP itself is not in mapped memory */
  kX86pStepMemoryFault,  /* the instruction touched unmapped memory */
  kX86pStepDivideError,  /* #DE: the guest must receive this */
  /*
   * The traps and faults below are OUTCOMES, not gaps. An instruction that
   * reports one has been fully modelled: this is what it does. Keeping them
   * distinct from Unsupported is the difference between "this engine has not
   * implemented HLT" and "the guest executed HLT, which a ring-3 process may
   * not do" -- and in a game binary the second usually means execution reached
   * alignment padding, which is worth saying out loud.
   */
  kX86pStepInterrupt,       /* INT n, INT3, INT1, INTO: `trap_vector` says which */
  kX86pStepProtectionFault, /* #GP: a privileged or I/O instruction at ring 3 */
  kX86pStepBoundRange,      /* #BR: BOUND found the index outside its pair */
  kX86pStepStatusCount      /* MUST stay last */
} X86pStepStatus;

/*
 * Copy the bytes of the instruction at `eip` into `bytes`: as many as are
 * mapped, up to X86P_MAX_INSN_LEN. Returns how many, 0 when `eip` itself is
 * not mapped. A short window is not a fault; the decoder decides whether the
 * instruction fits in it.
 */
uint32_t x86p_fetch_insn_bytes(const X86pMem *mem, uint32_t eip, uint8_t bytes[X86P_MAX_INSN_LEN]);

/*
 * Execute an ALREADY DECODED instruction whose address is cpu->eip.
 *
 * `insn` MUST be the instruction at cpu->eip. kX86pInsnUnsupported is
 * checked rather than assumed, because a caller that got it wrong would
 * silently advance EIP past an instruction that never ran.
 *
 * On success EIP has advanced or branched. On a fault EIP is unchanged and
 * `fault_addr`, when not null, receives the guest address that faulted.
 */
X86pStepStatus x86p_execute_decoded(X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, uint32_t *fault_addr);

/* Name a status, for logs and refusals. Never null. */
const char *x86p_step_status_name(X86pStepStatus s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_INSN_EXEC_H */
