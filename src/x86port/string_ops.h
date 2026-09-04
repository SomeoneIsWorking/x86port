/*
 * string_ops.h -- MOVS, STOS, LODS, SCAS, CMPS, with their repeat prefix.
 *
 * Their own module rather than five more arms in the interpreter's switch,
 * because they are the only instructions here that are a LOOP. Everything else
 * in exec.c reads operands, computes, and writes once; these advance two
 * pointers by a signed stride the direction flag chooses, and a repeat prefix
 * turns one of them into up to four billion memory accesses whose termination
 * depends on ECX and, for the two that compare, on ZF as well.
 *
 * THE WHOLE REPEAT RUNS AS ONE STEP. x86 permits an interrupt between
 * iterations and resumes with ECX and the pointers where they stand, which is
 * exactly why the architecture leaves them in registers -- but this framework
 * has no interrupts to take, and pretending otherwise would mean inventing a
 * resumption point no caller can use. A caller that needs to bound the work
 * bounds ECX.
 *
 * A FAULT STOPS THE LOOP WHERE IT HAPPENED, and the registers keep the values
 * they had reached. That is what the guest would see, and it is what lets a
 * caller report which address failed rather than which instruction started.
 */
#ifndef X86PORT_STRING_OPS_H
#define X86PORT_STRING_OPS_H

#include "cpu.h"
#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86pStringStatus {
  kX86pStringOk = 0,
  kX86pStringFault,       /* an access was outside the mapping */
  kX86pStringUnsupported, /* a repeat prefix that operation does not define */
  kX86pStringStatusCount  /* MUST stay last */
} X86pStringStatus;

/*
 * Execute one string instruction, repeat prefix included.
 *
 * Reads and updates ESI, EDI, ECX, EAX and the flags directly, because that is
 * what the instruction does -- there are no operands to read. `fault_addr`, if
 * not null, receives the guest address that failed when the status is Fault.
 */
X86pStringStatus x86p_string_execute(X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, uint32_t *fault_addr);

/* Translation-time shape gate shared with the runtime helper, so the JIT
   cannot admit a prefix/operation combination the semantic owner refuses. */
int x86p_string_is_supported(X86pStringOp op, X86pRepKind rep, int width);

const char *x86p_string_status_name(X86pStringStatus s);
const char *x86p_string_op_name(X86pStringOp op);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_STRING_OPS_H */
