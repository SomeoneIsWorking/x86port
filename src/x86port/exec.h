/*
 * exec.h -- the test interpreter: one guest instruction, fetched, decoded and
 * executed.
 *
 * Linked only into the separate x86port_test_oracle target. It is the
 * authority a translator is checked against, and deliberately a SINGLE STEP
 * rather than a run loop, because every use the tests have for it -- lockstep
 * comparison against another engine, first-divergence reporting -- needs to
 * stop after each instruction and look. The semantics it runs are the
 * product's own (insn_exec.h); this file adds only fetch, decode and the
 * report.
 */
#ifndef X86PORT_EXEC_H
#define X86PORT_EXEC_H

#include "cpu.h"
#include "decode.h"
#include "decode_cache.h"
#include "insn_exec.h"

#ifdef __cplusplus
extern "C" {
#endif

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
  uint8_t trap_vector;  /* the interrupt vector, when status is Interrupt; also on the CPU */
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

/*
 * As x86p_step, answering the decode from a caller-owned cache.
 *
 * The cache is the interpreter's largest single saving (see decode_cache.h)
 * and it is exact: an entry is used only while the guest bytes at that address
 * still match the ones decoded. A NULL cache is the uncached path, so the two
 * can be run against each other.
 */
X86pStepStatus x86p_step_cached(X86pCpu *cpu, const X86pMem *mem, X86pDecodeCache *cache, X86pStepReport *report);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_EXEC_H */
