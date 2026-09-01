/*
 * three_dnow.h -- 3DNow! semantics, as pure functions over a register pair.
 *
 * WHY THIS EXISTS, AND WHY IT IS FIRST. Measured on pc/xmen2 (jit-common I004):
 * the static translator left 8,234 instructions untranslated across its emitted
 * corpus, and 7,410 of them -- 90.0% -- are 3DNow!. They arrive as exactly 2,440
 * sites each in libIGGfx_003, cgD3D8_000 and XMen2_011, which is one
 * 3DNow!-compiled Alchemy math library statically linked three times rather than
 * three separate problems. Twenty opcodes cover every one of those sites.
 *
 * It is also the part of the engine that does NOT depend on the decoder
 * decision still open in I004: 3DNow! operates on the MMX registers as two
 * packed 32-bit floats, so its semantics can be written, tested and trusted
 * before anything has decided how an instruction gets from guest memory to
 * here.
 *
 * NUMERIC MODEL. 3DNow! is deliberately not IEEE-754: there are no exceptions,
 * no NaN or infinity handling contract, and denormal inputs are flushed to
 * zero. Rounding is round-to-nearest-even, which is the host default. So each
 * operation flushes its inputs (x86p_pf_daz) and then uses host float
 * arithmetic. Where AMD's behaviour on a non-finite input is not something this
 * file can state from the specification, it is NOT guessed -- see the refusal
 * list below.
 *
 * WHAT IS DELIBERATELY NOT HERE. PFRCP, PFRSQRT, PFRCPIT1, PFRCPIT2 and
 * PFRSQIT1 are approximation instructions whose results come from AMD's
 * hardware tables to a specified number of mantissa bits, and whose
 * Newton-Raphson refinement steps have exact defined forms. Writing "1.0f / x"
 * for PFRCP would agree to about six digits and disagree in the low mantissa
 * bits, and a fast reciprocal that is subtly wrong is the kind of defect that
 * surfaces as geometry drift a thousand frames later. They are therefore
 * unimplemented and REFUSED BY NAME (x86p_3dnow_eval returns 0 and names the
 * op), not approximated. Together they are 384 of the 7,410 sites -- 5.2% --
 * so refusing them costs 94.8% of the family and buys the right to be certain
 * about the rest.
 */
#ifndef X86PORT_THREE_DNOW_H
#define X86PORT_THREE_DNOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An MMX register seen the way 3DNow! sees it: two packed 32-bit floats, lane 0
 * in the low dword. The integer view is carried alongside because PI2FD/PF2ID
 * and PSWAPD read or write the same 64 bits as two signed dwords, and a union
 * keeps that reinterpretation in one place instead of at every call site.
 */
typedef union X86pMm {
  float f[2];
  int32_t i[2];
  uint32_t u[2];
  uint64_t q;
} X86pMm;

/*
 * Every 3DNow! operation this framework knows the name of -- including the five
 * it refuses. A refused opcode must be NAMEABLE: an engine that cannot say
 * "PFRCP, unimplemented" can only say "unknown instruction", and those are
 * different facts about a run.
 *
 * kX86pPfCount stays last: it is the denominator every exhaustive check counts
 * against.
 */
typedef enum X86pPfOp {
  kX86pPfAdd = 0, /* PFADD     lane-wise a + b                                */
  kX86pPfSub,     /* PFSUB     lane-wise a - b                                */
  kX86pPfSubR,    /* PFSUBR    lane-wise b - a                                */
  kX86pPfMul,     /* PFMUL     lane-wise a * b                                */
  kX86pPfAcc,     /* PFACC     horizontal add: {a0+a1, b0+b1}                 */
  kX86pPfNAcc,    /* PFNACC    horizontal sub: {a0-a1, b0-b1}                 */
  kX86pPfPNAcc,   /* PFPNACC   mixed:          {a0-a1, b0+b1}                 */
  kX86pPfMax,     /* PFMAX     lane-wise max                                  */
  kX86pPfMin,     /* PFMIN     lane-wise min                                  */
  kX86pPfCmpEq,   /* PFCMPEQ   lane-wise a == b -> all-ones / all-zeros mask  */
  kX86pPfCmpGe,   /* PFCMPGE   lane-wise a >= b -> mask                       */
  kX86pPfCmpGt,   /* PFCMPGT   lane-wise a >  b -> mask                       */
  kX86pPi2Fd,     /* PI2FD     two signed dwords -> two floats                */
  kX86pPf2Id,     /* PF2ID     two floats -> two signed dwords, truncating    */
  kX86pPi2Fw,     /* PI2FW     two signed words (low of each dword) -> floats */
  kX86pPf2Iw,     /* PF2IW     two floats -> two sign-extended signed words   */
  kX86pPSwapD,    /* PSWAPD    swap the two dwords of the source              */
  kX86pPAvgUsb,   /* PAVGUSB   eight unsigned bytes, rounded average          */
  kX86pPMulHrw,   /* PMULHRW   four signed words, rounded high multiply       */

  /* Refused: approximation instructions. See the header comment. */
  kX86pPfRcp,    /* PFRCP     ~14-bit reciprocal approximation               */
  kX86pPfRsqrt,  /* PFRSQRT   ~15-bit reciprocal-sqrt approximation          */
  kX86pPfRcpIt1, /* PFRCPIT1  first Newton-Raphson step for PFRCP            */
  kX86pPfRcpIt2, /* PFRCPIT2  second Newton-Raphson step for PFRCP/PFRSQRT   */
  kX86pPfRsqIt1, /* PFRSQIT1  first Newton-Raphson step for PFRSQRT          */

  kX86pPfCount /* MUST stay last */
} X86pPfOp;

/* The mnemonic, upper case, exactly as a disassembler spells it. Never returns
   null, including for a value outside the enum -- a null here would turn a
   refusal report into a crash, in the code whose only job is to report. */
const char *x86p_3dnow_name(X86pPfOp op);

/* Text -> op, case-insensitively. False for anything unrecognised, including
   null and empty, and *out is left untouched so ignoring the result cannot
   silently select an operation. The spellings are x86p_3dnow_name's. */
int x86p_3dnow_parse(const char *mnemonic, X86pPfOp *out);

/* Whether this build implements `op`. False for the five approximation
   instructions and for any value outside the enum. */
int x86p_3dnow_implemented(X86pPfOp op);

/*
 * Apply `op` to `a` (the destination operand, read-modify-write) and `b`,
 * writing the result to *out. Returns 1 when it ran and 0 when the operation is
 * one this build refuses -- the caller MUST distinguish those, because a
 * refused instruction that looks like a completed one is how a wrong result
 * enters a run without a report. *out is untouched on refusal.
 */
int x86p_3dnow_eval(X86pPfOp op, X86pMm a, X86pMm b, X86pMm *out);

/* Denormal-to-zero, preserving sign. Exposed because the interpreter's own
   float paths need the same flush, and two copies of a numeric model is how the
   two disagree. */
float x86p_pf_daz(float v);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_THREE_DNOW_H */
