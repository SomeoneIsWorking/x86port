/*
 * exec.h -- one guest instruction, executed.
 *
 * This is the thing S043 exists to produce: the authority on what an x86-32
 * instruction means, against which a translator is checked. It is deliberately
 * a SINGLE STEP rather than a run loop, because every use this framework has
 * for it -- lockstep comparison against another engine, first-divergence
 * reporting, taking over at a dispatch miss -- needs to stop after each
 * instruction and look. A caller that wants a loop writes three lines.
 *
 * EVERY OUTCOME IS NAMED. There is no "returned normally but did nothing".
 * The three ways an instruction can fail to execute -- the bytes are not an
 * instruction, they are an instruction this build has no semantics for, or the
 * guest touched memory it does not have -- are different facts about a run, and
 * an engine that cannot tell them apart cannot be debugged. The middle one in
 * particular must never be silent: "the interpreter ran it" and "the
 * interpreter skipped it" produce the same register file for exactly the
 * instructions whose effects are subtle.
 */
#ifndef X86PORT_EXEC_H
#define X86PORT_EXEC_H

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
  kX86pStepStatusCount   /* MUST stay last */
} X86pStepStatus;

/*
 * What happened, in enough detail to act on.
 *
 * `mnemonic` is filled even when the status is Unsupported -- that is the whole
 * point of naming a refusal, and it is what makes the unmodelled instructions a
 * ranked work list rather than a count.
 */
typedef struct X86pStepReport {
  uint8_t status;
  uint32_t eip;         /* where the instruction was */
  uint32_t length;      /* its encoded length, 0 if it did not decode */
  const char *mnemonic; /* never null after a decode; "?" otherwise */
  uint8_t op;           /* X86pInsnOp */
  uint32_t fault_addr;  /* the guest address that faulted, when one did */
} X86pStepReport;

/*
 * Execute the instruction at cpu->eip.
 *
 * On success EIP has advanced (or branched) and the report says what ran. On
 * any failure the machine is left as close to untouched as the instruction
 * allows and EIP still points AT the failing instruction, so a caller can
 * report it, hand it to another engine, or retry after mapping memory.
 *
 * `report` may be null when the caller only wants the status, but a caller
 * that discards the report cannot tell an unsupported instruction from a fault
 * and should not be doing that.
 */
X86pStepStatus x86p_step(X86pCpu *cpu, const X86pMem *mem, X86pStepReport *report);

/* Name a status, for logs and refusals. Never null. */
const char *x86p_step_status_name(X86pStepStatus s);

/*
 * Widen a value from `from_bytes` bytes, sign-extending.
 *
 * Exported because it is a SEMANTIC RULE, not a utility: PUSH imm8 pushes a
 * sign-extended dword, and a translator that folded its own copy of that rule
 * would be a second authority on what the instruction means. There is one
 * implementation and both engines call it.
 */
uint32_t x86p_sign_extend(uint32_t v, int from_bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_EXEC_H */
