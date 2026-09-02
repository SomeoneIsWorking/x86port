/*
 * decode.h -- one instruction, from guest bytes to something the engine can act
 * on.
 *
 * THE DECISION THIS SETTLES (jit-common I004). `shared/recomp-x86` has no
 * decoder: its front end is Ghidra and it lifts disassembled mnemonic TEXT,
 * which is why its failures read "mnemonic PFMUL" rather than an opcode byte.
 * A runtime engine cannot work that way -- Ghidra is a maintainer-only tool and
 * can never be a player prerequisite -- so x86port needs its own decode.
 *
 * It BORROWS rather than writes it, and the boundary is deliberate. Decode is
 * mechanical, exhaustively specified, and has no opinion about memory models,
 * threading or code caches, so embedding a proven implementation costs nothing
 * architecturally -- unlike embedding a whole CPU core, which brings all three.
 * Semantics stay ours, because S043's whole point is that we are the authority
 * on what correct means. Zydis (MIT, allocation-free, pre-generated tables, no
 * build-time code generation) is pinned at v4.1.1 in vendor/zydis.
 *
 * AND THE CHOICE IS MEASURED, NOT ASSERTED. pc/xmen2's Ghidra corpus carries
 * the raw bytes of every instruction beside Ghidra's own reading of them --
 * 2,168,629 real instructions across 20 modules. `x86p_decode_diff` decodes all
 * of them through THIS function and reports agreement with a denominator. See
 * tools/decode_diff.c.
 */
#ifndef X86PORT_DECODE_H
#define X86PORT_DECODE_H

#include "alu.h"
#include "cond.h"
#include "x87.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest an x86 instruction may be. A decoder handed more than this has
   been handed data, not code. */
#define X86P_MAX_INSN_LEN 15

/*
 * OPERANDS, IN THIS FRAMEWORK'S OWN TERMS.
 *
 * Zydis is confined to decode.c and nothing else in the repo includes it. That
 * is the boundary the decoder decision rests on: decode may be borrowed,
 * semantics may not, and a borrowed type spreading through the execution code
 * would quietly turn "we use a decode library" into "our semantics are
 * whatever that library models". If the decoder is ever replaced, only decode.c
 * changes.
 */
typedef enum X86pOperandKind {
  kX86pOperandNone = 0,
  kX86pOperandReg,
  kX86pOperandMem,
  kX86pOperandImm,
  /* An x87 stack POSITION, ST(i). Deliberately not folded into Reg: `reg`
     there is an encoded GPR number, and ST(3) is not register 3 -- it is
     whatever physical register TOP+3 currently names. Sharing the kind is how
     an engine ends up indexing the FPU array with a value that means something
     else. `reg` holds i, 0-7. */
  kX86pOperandSt,
  kX86pOperandKindCount /* MUST stay last */
} X86pOperandKind;

typedef struct X86pOperand {
  uint8_t kind; /* X86pOperandKind */
  /* Operand width in BYTES. 1, 2 or 4 for the integer engine; an x87 memory
     operand is additionally 8 (double) or 10 (the extended format), and those
     two widths are accepted ONLY for an x87 instruction -- see decode.c. */
  uint8_t size;
  /* kind == Reg: the ENCODED register number, and at size 1 that is a byte
     register, where 4-7 are AH..BH rather than ESP..EDI (see cpu.h). */
  int8_t reg;
  /* kind == Mem: base + index*scale + disp, each part optional. -1 means the
     part is absent, which is a different fact from register 0 -- conflating
     them makes `[disp32]` read as `[EAX+disp32]`. */
  int8_t base;
  int8_t index;
  uint8_t scale; /* 1, 2, 4 or 8 */
  int32_t disp;
  /* kind == Imm: already sign-extended to 32 bits, because the guest encoding
     stores a byte and means a dword, and every caller would otherwise have to
     remember to widen it the same way. */
  uint32_t imm;
  /* A branch displacement is RELATIVE TO THE NEXT INSTRUCTION, not to the one
     it appears in. Carried as a flag rather than resolved at decode time
     because decode does not know where the instruction lives -- and folding
     the address in would make the same bytes decode to different values
     depending on where they were read from, which a cache must never do. */
  uint8_t relative;
} X86pOperand;

/*
 * What the instruction DOES, as this framework models it.
 *
 * The arithmetic operations are NOT re-enumerated here: `op` says "an ALU
 * operation" and `alu` says which, reusing X86pAluOp. A second list of the same
 * operations is a second place for ADC and SBB to be swapped.
 */
typedef enum X86pInsnOp {
  kX86pInsnUnsupported = 0, /* decoded and named, but no semantics in this build */
  kX86pInsnAlu,             /* `alu` holds an X86pAluOp; operands[0] is the destination */
  kX86pInsnAluUnary,        /* `alu` holds an X86pAluUnOp */
  kX86pInsnMov,
  kX86pInsnMovzx,
  kX86pInsnMovsx,
  kX86pInsnLea,
  kX86pInsnPush,
  kX86pInsnPop,
  kX86pInsnXchg,
  kX86pInsnJmp,
  kX86pInsnJcc,   /* `cond` holds an X86pCond */
  kX86pInsnSetcc, /* `cond` holds an X86pCond */
  kX86pInsnCmovcc,
  kX86pInsnCall,
  kX86pInsnRet,
  kX86pInsnLeave,
  kX86pInsnNop,
  kX86pInsnCdq,  /* sign-extend EAX into EDX:EAX */
  kX86pInsnCwde, /* sign-extend AX into EAX */
  kX86pInsnMul,
  kX86pInsnImul,
  kX86pInsnDiv,
  kX86pInsnIdiv,
  kX86pInsnPushfd,
  kX86pInsnPopfd,
  /*
   * x87. `x87` holds an X86pX87Insn saying which, for the same reason `alu`
   * does: one list of floating-point operations, not two.
   *
   * The POP SUFFIX IS PART OF THE OPERATION, not a detail of it. FADD and
   * FADDP compute the same sum and leave the stack at different depths, and a
   * model that treats the P as cosmetic drifts by one register and then reads
   * every subsequent value from the wrong place.
   */
  kX86pInsnX87,
  /*
   * The string operations. `str` holds an X86pStringOp, `rep` an X86pRepKind,
   * and `str_width` the element size in bytes.
   *
   * The REP PREFIX IS PART OF THE INSTRUCTION, not a modifier on it, for the
   * same reason the x87 pop suffix is: REP MOVSD and MOVSD leave different
   * machines, and a model that recorded only the mnemonic would move four
   * bytes where the guest moved four megabytes.
   */
  kX86pInsnString,
  kX86pInsnCld,    /* DF = 0: the string operations count upward */
  kX86pInsnStd,    /* DF = 1: ... and downward */
  kX86pInsnOpCount /* MUST stay last */
} X86pInsnOp;

/*
 * Which string operation, by what it touches.
 *
 * Named by direction of travel rather than by mnemonic: MOVS reads at ESI and
 * writes at EDI, STOS only writes, LODS only reads, and SCAS and CMPS write
 * nothing but flags. That distinction is what decides which pointers advance,
 * so it is the thing worth being explicit about.
 */
typedef enum X86pStringOp {
  kX86pStringMovs = 0, /* [EDI] <- [ESI]; both advance */
  kX86pStringStos,     /* [EDI] <- AL/AX/EAX; EDI advances */
  kX86pStringLods,     /* AL/AX/EAX <- [ESI]; ESI advances */
  kX86pStringScas,     /* CMP AL/AX/EAX, [EDI]; EDI advances */
  kX86pStringCmps,     /* CMP [ESI], [EDI]; both advance */
  kX86pStringOpCount   /* MUST stay last */
} X86pStringOp;

/*
 * The repeat prefix.
 *
 * F3 and F2 are the same byte pair for every string operation and mean
 * different things depending on it: on MOVS/STOS/LODS, F3 repeats CX times and
 * F2 is not defined; on SCAS/CMPS, F3 repeats WHILE EQUAL and F2 WHILE NOT
 * EQUAL. Recorded as the prefix that was present rather than as a resolved
 * condition, because resolving it needs the operation and the decoder should
 * not be the second place that knows the pairing.
 */
typedef enum X86pRepKind {
  kX86pRepNone = 0,
  kX86pRepRep,   /* F3 */
  kX86pRepRepne, /* F2 */
  kX86pRepKindCount
} X86pRepKind;

#define X86P_MAX_OPERANDS 3

typedef struct X86pInsn {
  uint32_t length;      /* encoded bytes consumed; 0 only when decode failed */
  const char *mnemonic; /* upper case, static storage; never null on success */
  int operands;         /* explicit operand count */
  uint8_t op;           /* X86pInsnOp -- Unsupported is a REPORTABLE outcome */
  uint8_t alu;          /* X86pAluOp or X86pAluUnOp, per `op` */
  uint8_t cond;         /* X86pCond, per `op` */
  uint8_t x87;          /* X86pX87Insn, when op == kX86pInsnX87 */
  uint8_t x87_op;       /* X86pX87Op, when x87 == kX86pX87InsnArith */
  /* The pop suffix. Set for FSTP, FADDP, FCOMP and friends; FCOMPP pops TWICE,
     so this counts rather than flags -- a single bit would make FCOMPP either
     FCOMP or a second instruction, and both are wrong. */
  uint8_t x87_pops;
  uint8_t x87_reverse; /* FSUBR/FDIVR: the operands, and only the operands */
  uint8_t str;         /* X86pStringOp, when op == kX86pInsnString */
  uint8_t rep;         /* X86pRepKind, when op == kX86pInsnString */
  uint8_t str_width;   /* element size in bytes: 1, 2 or 4 */
  /* The FI forms -- FIADD, FIMUL, FICOM -- read their memory operand as a
     two's-complement INTEGER, not as a float of the same width. Recorded here
     rather than recovered from the mnemonic spelling, because a check on the
     letter 'I' is a decision made from a display string. */
  uint8_t x87_mem_int;
  X86pOperand operand[X86P_MAX_OPERANDS];
} X86pInsn;

/* Name of an X86pInsnOp, for traces and refusals. Never null. */
const char *x86p_insn_op_name(int op);

/*
 * Decode one 32-bit-mode instruction from `bytes` (at most `len` readable).
 * Returns the instruction length, or 0 if these bytes are not a decodable
 * instruction -- and 0 is a REPORTABLE fact, not a signal to skip a byte and
 * try again. A decoder that resynchronises silently turns "this is data" into
 * "this is some other instruction", which is the failure mode static analysis
 * already has and the one a runtime engine exists to avoid.
 *
 * *out is untouched on failure.
 */
uint32_t x86p_decode(const uint8_t *bytes, size_t len, X86pInsn *out);

/* The decoder implementation and its pinned version, for run reports and for
   the "which decoder produced this evidence" question a divergence raises. */
const char *x86p_decoder_id(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_DECODE_H */
