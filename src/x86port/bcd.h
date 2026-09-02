/*
 * bcd.h -- the six decimal-adjust instructions: DAA, DAS, AAA, AAS, AAM, AAD.
 *
 * These are the only instructions in the ISA that READ the auxiliary-carry
 * flag, which is why flags.h carries AF at all when the shipping substrate
 * next door does not. `shared/recomp-x86` classifies them as "embedded data
 * decoded as code" and never executes one; an interpreter decodes what
 * execution actually reaches and cannot make that bet.
 *
 * They are pure functions of AX and EFLAGS, so they are stated here as pure
 * functions of AX and EFLAGS -- no CPU, no memory, nothing to mock. The
 * plumbing lives in exec.c where every other instruction's does.
 *
 * ON THE UNDEFINED FLAGS. The SDM leaves OF undefined for all six, and CF/AF
 * undefined for AAM and AAD. "Undefined" describes the specification, not the
 * silicon: this CPU does something specific and a guest that saves EFLAGS can
 * see it. tests/test_bcd.c compares the whole word against the real
 * instruction, so what this file does with those bits is measured rather than
 * assumed -- the same discipline that found four defects in flags.c.
 */
#ifndef X86PORT_BCD_H
#define X86PORT_BCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86pBcdOp {
  kX86pBcdDaa = 0, /* decimal adjust after addition */
  kX86pBcdDas,     /* ... after subtraction */
  kX86pBcdAaa,     /* ASCII adjust after addition */
  kX86pBcdAas,     /* ... after subtraction */
  kX86pBcdAam,     /* ASCII adjust after multiply; takes the base as an immediate */
  kX86pBcdAad,     /* ... before divide; likewise */
  kX86pBcdOpCount  /* MUST stay last: the denominator for an exhaustive check */
} X86pBcdOp;

const char *x86p_bcd_op_name(X86pBcdOp op);

/*
 * Apply one decimal adjustment in place.
 *
 * `imm` is the base, and is read only by AAM and AAD; every other operation
 * ignores it. Returns 0 for AAM with a base of zero, which is a divide error
 * the guest must receive -- not a silent no-op, and not a division this
 * function performs anyway.
 */
int x86p_bcd_apply(X86pBcdOp op, uint16_t *ax, uint32_t *eflags, uint8_t imm);

#ifdef __cplusplus
}
#endif

#endif
