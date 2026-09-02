/*
 * simd.h -- MMX, SSE and 3DNow!, as one instruction family.
 *
 * One family because they are one problem: they share the MMX register file
 * (3DNow! IS MMX registers holding packed floats), they share the operand
 * shapes, and a guest built by one compiler uses all three in the same
 * function. Splitting them by marketing name would put the same lane loop in
 * three files.
 *
 * WHAT DECIDES THE NUMERIC MODEL, per family:
 *
 *   - MMX integer operations are exact. There is nothing to approximate: a
 *     packed add of eight bytes has one answer, and saturation has a defined
 *     one too.
 *   - SSE packed-single arithmetic is IEEE-754 binary32 with round-to-nearest,
 *     which is what the host does with `float`. Denormals are NOT flushed
 *     unless MXCSR says so, and this framework does not yet model MXCSR's
 *     flush-to-zero bit -- an instruction whose result would depend on it is
 *     computed at the default setting, which is what every process this
 *     targets uses.
 *   - 3DNow! is deliberately not IEEE-754 and has its own module. This one
 *     calls it rather than restating it.
 *
 * WHAT IS REFUSED BY NAME, and why refusing is the honest answer:
 * RCPPS, RCPSS, RSQRTPS, RSQRTSS and the 3DNow! approximation family produce
 * results from hardware tables to a specified number of mantissa bits. Writing
 * 1.0f/x agrees to about six digits and differs in the low bits, and a fast
 * reciprocal that is subtly wrong surfaces as drift a thousand frames later.
 * They return a named refusal, and the count of them is published rather than
 * buried: 100% coverage that included a guess would be worth less than 99.99%
 * that does not.
 */
#ifndef X86PORT_SIMD_H
#define X86PORT_SIMD_H

#include "cpu.h"
#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Every SIMD operation this framework can name, including the ones it refuses.
 * A refused opcode must be NAMEABLE: "RCPPS, unimplemented" and "unknown
 * instruction" are different facts about a run.
 */
typedef enum X86pSimdOp {
  /* ---- movement ---- */
  kX86pSimdMovq = 0, /* MOVQ    64 bits, MMX register or memory */
  kX86pSimdMovd,     /* MOVD    32 bits between an MMX/XMM register and r/m32 */
  kX86pSimdMovaps,   /* MOVAPS/MOVUPS/MOVDQA/MOVDQU: 128 bits, either way */
  kX86pSimdMovss,    /* MOVSS   the low float only, or a zeroing load */
  kX86pSimdMovlps,   /* MOVLPS/MOVLPD  the low 64 bits */
  kX86pSimdMovhps,   /* MOVHPS/MOVHPD  the high 64 bits */
  kX86pSimdMovhlps,  /* MOVHLPS xmm.low <- src.high */
  kX86pSimdMovlhps,  /* MOVLHPS xmm.high <- src.low */
  kX86pSimdMovmskps, /* MOVMSKPS  four sign bits into a general register */
  kX86pSimdPmovmskb, /* PMOVMSKB  sign bit of each byte */

  /* ---- MMX / SSE integer ---- */
  kX86pSimdPand,
  kX86pSimdPandn,
  kX86pSimdPor,
  kX86pSimdPxor,
  kX86pSimdPaddb,
  kX86pSimdPaddw,
  kX86pSimdPaddd,
  kX86pSimdPaddq,
  kX86pSimdPaddsb,
  kX86pSimdPaddsw,
  kX86pSimdPaddusb,
  kX86pSimdPaddusw,
  kX86pSimdPsubb,
  kX86pSimdPsubw,
  kX86pSimdPsubd,
  kX86pSimdPsubq,
  kX86pSimdPsubsb,
  kX86pSimdPsubsw,
  kX86pSimdPsubusb,
  kX86pSimdPsubusw,
  kX86pSimdPcmpeqb,
  kX86pSimdPcmpeqw,
  kX86pSimdPcmpeqd,
  kX86pSimdPcmpgtb,
  kX86pSimdPcmpgtw,
  kX86pSimdPcmpgtd,
  kX86pSimdPunpcklbw,
  kX86pSimdPunpcklwd,
  kX86pSimdPunpckldq,
  kX86pSimdPunpckhbw,
  kX86pSimdPunpckhwd,
  kX86pSimdPunpckhdq,
  kX86pSimdPackuswb,
  kX86pSimdPacksswb,
  kX86pSimdPackssdw,
  kX86pSimdPsllw,
  kX86pSimdPslld,
  kX86pSimdPsllq,
  kX86pSimdPsrlw,
  kX86pSimdPsrld,
  kX86pSimdPsrlq,
  kX86pSimdPsraw,
  kX86pSimdPsrad,
  kX86pSimdPmullw,
  kX86pSimdPmulhw,
  kX86pSimdPmulhuw,
  kX86pSimdPmaddwd,
  kX86pSimdPavgb,
  kX86pSimdPavgw,
  kX86pSimdPminub,
  kX86pSimdPmaxub,
  kX86pSimdPminsw,
  kX86pSimdPmaxsw,
  kX86pSimdPsadbw,
  kX86pSimdPextrw,
  kX86pSimdPinsrw,
  kX86pSimdPshufw,
  kX86pSimdPshufd,

  /* ---- SSE single precision ---- */
  kX86pSimdAddps,
  kX86pSimdSubps,
  kX86pSimdMulps,
  kX86pSimdDivps,
  kX86pSimdMinps,
  kX86pSimdMaxps,
  kX86pSimdSqrtps,
  kX86pSimdAddss,
  kX86pSimdSubss,
  kX86pSimdMulss,
  kX86pSimdDivss,
  kX86pSimdMinss,
  kX86pSimdMaxss,
  kX86pSimdSqrtss,
  kX86pSimdAndps,
  kX86pSimdAndnps,
  kX86pSimdOrps,
  kX86pSimdXorps,
  kX86pSimdCmpps,
  kX86pSimdCmpss,
  kX86pSimdComiss,
  kX86pSimdUcomiss,
  kX86pSimdShufps,
  kX86pSimdUnpcklps,
  kX86pSimdUnpckhps,
  kX86pSimdCvtsi2ss,
  kX86pSimdCvtss2si,
  kX86pSimdCvttss2si,
  kX86pSimdCvtpi2ps,
  kX86pSimdCvtps2pi,
  kX86pSimdCvttps2pi,

  /* ---- 3DNow!, evaluated by three_dnow.c ---- */
  kX86pSimdPf,

  /* ---- state ---- */
  kX86pSimdEmms, /* EMMS and FEMMS: the register file becomes empty */
  kX86pSimdLdmxcsr,
  kX86pSimdStmxcsr,
  kX86pSimdFence,    /* SFENCE/LFENCE/MFENCE: nothing to order here */
  kX86pSimdPrefetch, /* PREFETCH*: architecturally a hint, and has no result */

  /* ---- refused: approximation instructions ---- */
  kX86pSimdRcpps,
  kX86pSimdRcpss,
  kX86pSimdRsqrtps,
  kX86pSimdRsqrtss,

  kX86pSimdOpCount /* MUST stay last */
} X86pSimdOp;

typedef enum X86pSimdStatus {
  kX86pSimdOk = 0,
  kX86pSimdFault,       /* an access was outside the mapping */
  kX86pSimdUnsupported, /* named, and deliberately not implemented */
  kX86pSimdStatusCount  /* MUST stay last */
} X86pSimdStatus;

X86pSimdStatus x86p_simd_execute(X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, uint32_t *fault_addr);

/* Map a disassembler's spelling to an operation. Returns 0 when this framework
   has no name for it, which is a different fact from having no semantics. */
int x86p_simd_parse(const char *mnemonic, X86pSimdOp *out);

const char *x86p_simd_op_name(X86pSimdOp op);
const char *x86p_simd_status_name(X86pSimdStatus s);

/* Whether this build implements the operation, as opposed to naming it. The
   refusal list is queryable so a coverage tool can report it as a decision
   rather than as a gap. */
int x86p_simd_is_implemented(X86pSimdOp op);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_SIMD_H */
