/*
 * alu.h -- the integer ALU: what an operation PRODUCES and what it does to the
 * flags, decided together, in one place.
 *
 * WHY THEY ARE ONE CALL. `pc/xmen2`'s substrate computes the result at the call
 * site and then records the flags with a separate `SETFLAGS(C, FK_..., ...)`
 * macro. That is two facts about one instruction, kept in two places, at 400,614
 * sites -- and nothing checks that the kind passed to the second matches the
 * arithmetic done in the first. Pairing SUB's result with FK_ADD is a silent,
 * correct-looking line. Here the operation owns both, so the pairing cannot be
 * got wrong by a caller and does not need to be reviewed at every site.
 *
 * SCOPE, MEASURED. These 24 operations are 400,614 of the 2,168,629 instructions
 * in the shipped corpus -- 18.47%. They were not chosen by intuition: the
 * ranking comes from `tools/corpus_extract.py` over the real Ghidra export, and
 * it is also what says ROL/ROR/RCL/RCR are worth 54 instructions in total and
 * MOV/PUSH/CALL/POP/LEA are a different job (addressing, not arithmetic).
 *
 * WIDTH IS AN ARGUMENT, NOT A TYPE. Every entry point takes `w` in bytes,
 * because the same guest opcode operates at 8, 16 and 32 bits and the flags,
 * the sign bit and the wrap all move with it. Values are passed and returned in
 * a uint32_t with the bits above the width UNDEFINED on input and ZERO on
 * output, so a caller can neither smuggle high bits in nor read stale ones out.
 */
#ifndef X86PORT_ALU_H
#define X86PORT_ALU_H

#include "flags.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The binary operations, IN x86's OWN GROUP-1 ORDER: the ModRM reg field of
 * opcodes 0x80-0x83 selects ADD OR ADC SBB AND SUB XOR CMP as 0-7. Keeping the
 * enum in that order means the decoder indexes it directly instead of carrying
 * a translation table that can disagree with the manual.
 */
typedef enum X86pAluOp {
  kX86pAluAdd = 0,
  kX86pAluOr = 1,
  kX86pAluAdc = 2,
  kX86pAluSbb = 3,
  kX86pAluAnd = 4,
  kX86pAluSub = 5,
  kX86pAluXor = 6,
  kX86pAluCmp = 7, /* SUB that discards its result */
  kX86pAluTest,    /* AND that discards its result */
  /* Shifts and rotates. Group 2's reg field is ROL ROR RCL RCR SHL SHR ?? SAR,
     which is NOT contiguous with the above, so it is not encoded here -- an
     order that is almost the manual's is worse than one that plainly is not. */
  kX86pAluShl,
  kX86pAluShr,
  kX86pAluSar,
  kX86pAluRol,
  kX86pAluRor,
  kX86pAluRcl,
  kX86pAluRcr,
  kX86pAluOpCount /* MUST stay last */
} X86pAluOp;

typedef enum X86pAluUnOp {
  kX86pAluNot = 0, /* the only one of the four that writes NO flags */
  kX86pAluNeg,
  kX86pAluInc,
  kX86pAluDec,
  kX86pAluUnOpCount /* MUST stay last */
} X86pAluUnOp;

/*
 * Apply a binary operation and update `f`.
 *
 * Returns the result, masked to `w`. For CMP and TEST the result is still
 * returned -- it is what the flags were derived from, and a caller that wants
 * it for a diagnostic should not have to recompute it -- but the instruction
 * discards it.
 *
 * `b` is the shift or rotate count for the shift and rotate ops. The count is
 * masked to 5 bits HERE, once, because that masking is architectural (386 and
 * later) and a caller that forgets it produces a shift by 33 that C leaves
 * undefined.
 */
uint32_t x86p_alu(X86pAluOp op, uint32_t a, uint32_t b, int w, X86pFlags *f);

/* Apply a unary operation and update `f`. NOT writes no flags at all, which is
   why it is in this enum rather than expressed as `XOR a, -1`. */
uint32_t x86p_alu_unary(X86pAluUnOp op, uint32_t a, int w, X86pFlags *f);

/*
 * The widening multiplies. `hi` receives the upper half (EDX), `lo` the lower
 * (EAX). Both are written on every call, so a caller cannot read a stale EDX
 * after a multiply that "didn't need" it.
 *
 * CF and OF say the same thing for both: the result did not fit in the lower
 * half alone. ZF, SF, PF and AF are architecturally undefined; the values this
 * writes are the ones a real CPU produces -- see tests/test_alu.c, which
 * compares them.
 */
void x86p_alu_mul(uint32_t a, uint32_t b, int w, uint32_t *lo, uint32_t *hi, X86pFlags *f);
void x86p_alu_imul(uint32_t a, uint32_t b, int w, uint32_t *lo, uint32_t *hi, X86pFlags *f);

/*
 * The divides. `hi:lo` is the dividend, `d` the divisor.
 *
 * Returns 1 on success, writing the quotient and remainder. Returns 0 for a
 * DIVIDE ERROR -- a zero divisor, or a quotient too large for the destination
 * -- and writes NOTHING in that case. That is a guest-visible exception (#DE),
 * not a host failure: the engine has to deliver it to the guest, so it must be
 * reported rather than aborted, and it must be distinguishable from a divide
 * that legitimately produced zero.
 */
int x86p_alu_div(uint32_t hi, uint32_t lo, uint32_t d, int w, uint32_t *quot, uint32_t *rem, X86pFlags *f);
int x86p_alu_idiv(uint32_t hi, uint32_t lo, uint32_t d, int w, uint32_t *quot, uint32_t *rem, X86pFlags *f);

/* Names, for refusal messages and divergence reports. Never null, including for
   a value outside the enum. */
const char *x86p_alu_name(X86pAluOp op);
const char *x86p_alu_unary_name(X86pAluUnOp op);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_ALU_H */
