/*
 * x87_exec.h -- executing an x87 instruction against guest state.
 *
 * Separate from exec.c because the FPU is a separate state machine: a rotating
 * stack, its own control and status words, and its own operand formats. Folding
 * it into the integer interpreter would put two unrelated machines in one file
 * and make the 500-line integer path grow past the point where its own
 * structure is visible.
 *
 * THE ADDRESSING IS NOT DUPLICATED HERE. exec.c already owns effective-address
 * computation, so it resolves the memory operand and passes the address in.
 * A second implementation of base+index*scale+disp is exactly the kind of copy
 * that drifts, and the one place it would drift is the place nobody tests.
 */
#ifndef X86PORT_X87_EXEC_H
#define X86PORT_X87_EXEC_H

#include "cpu.h"
#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The outcome, named. `Unsupported` here means an x87 instruction this build
 * decodes but does not execute -- a different fact from one it cannot decode,
 * and the caller reports which.
 */
typedef enum X86pX87ExecStatus {
  kX86pX87ExecOk = 0,
  kX86pX87ExecMemoryFault,
  kX86pX87ExecUnsupported,
  kX86pX87ExecStatusCount /* MUST stay last */
} X86pX87ExecStatus;

/*
 * Execute one decoded x87 instruction.
 *
 * `mem_addr` is the already-resolved effective address of the instruction's
 * memory operand, and `has_mem` says whether there is one. Passing an address
 * that was never computed is how an engine reads from a stale one, so the flag
 * is separate rather than encoded as a sentinel address -- zero is a legal
 * guest address.
 *
 * On a memory fault *fault_addr receives the address, so the caller can report
 * where rather than only that.
 */
X86pX87ExecStatus x86p_x87_execute(
    X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, int has_mem, uint32_t mem_addr, uint32_t *fault_addr);

/* Name an outcome. Never null. */
const char *x86p_x87_exec_status_name(X86pX87ExecStatus s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_EXEC_H */
