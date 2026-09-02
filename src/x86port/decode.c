/* decode.c -- see decode.h for why decode is borrowed and semantics are not. */
#include "decode.h"

#include "simd.h"
#include "three_dnow.h"

#include <Zydis/Zydis.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * Zydis spells mnemonics in lower case; everything else in this framework (and
 * the Ghidra corpus the decoder is measured against) uses upper. Uppercasing at
 * every call site is how the two conventions come to disagree, so it happens
 * once, here, into a small cache of static storage -- the returned pointer must
 * outlive the call, and Zydis's own string is already static, so this only
 * changes the case.
 *
 * The cache is a fixed table indexed by Zydis's mnemonic enum, filled lazily.
 * Bounded, no allocation, and safe to hand out because an entry, once written,
 * never changes.
 */
#define MNEMONIC_CACHE_MAX ZYDIS_MNEMONIC_MAX_VALUE
static char g_mnemonic[MNEMONIC_CACHE_MAX + 1][32];

static const char *upper_mnemonic(ZydisMnemonic m) {
  const char *src;
  size_t i;
  if ((unsigned)m > (unsigned)MNEMONIC_CACHE_MAX) {
    return "UNKNOWN";
  }
  if (g_mnemonic[m][0] != '\0') {
    return g_mnemonic[m];
  }
  src = ZydisMnemonicGetString(m);
  if (!src) {
    return "UNKNOWN";
  }
  for (i = 0; i + 1 < sizeof g_mnemonic[0] && src[i]; i++) {
    g_mnemonic[m][i] = (char)toupper((unsigned char)src[i]);
  }
  g_mnemonic[m][i] = '\0';
  return g_mnemonic[m];
}

/* ---- Zydis -> this framework's own terms --------------------------------
 *
 * Everything below exists so that nothing outside this file includes Zydis.
 * See the operand comment in decode.h for why that boundary is the decoder
 * decision rather than a tidiness preference.
 */

static const char *kOpNames[] = {
    "unsupported", "alu",  "alu-unary", "mov",    "movzx",  "movsx", "lea",   "push",   "pop",  "xchg",
    "jmp",         "jcc",  "setcc",     "cmovcc", "call",   "ret",   "leave", "nop",    "cdq",  "cwde",
    "mul",         "imul", "div",       "idiv",   "pushfd", "popfd", "x87",   "string", "simd", "cld",
    "std",         "bcd",  "bit",       "shld",   "shrd",   "sahf",  "lahf",  "stc",    "clc",  "cmc",
    "salc",        "xlat", "pushad",    "popad",  "enter",  "loop",  "loope", "loopne", "jecxz"};
_Static_assert((int)(sizeof kOpNames / sizeof kOpNames[0]) == (int)kX86pInsnOpCount, "every X86pInsnOp needs a name");

const char *x86p_insn_op_name(int op) {
  if (op < 0 || op >= (int)kX86pInsnOpCount) {
    return "unknown";
  }
  return kOpNames[op];
}

/*
 * A Zydis register to an ENCODED register number plus a width.
 *
 * Zydis numbers its 8-bit class AL, CL, DL, BL, AH, CH, DH, BH -- which is the
 * encoding's own order and therefore exactly cpu.h's byte-register convention,
 * where 4-7 are high bytes. That agreement is asserted by a test rather than
 * assumed here, because it is the kind of thing a library is entitled to
 * change and we are not entitled to be surprised by.
 *
 * Returns 0 for a register this framework has no model for -- segment,
 * control, debug, x87, MMX, SSE -- so the caller refuses by name instead of
 * silently executing against register 0, which is EAX.
 */
/*
 * A Zydis segment register to this framework's own number.
 *
 * Refuses anything outside the six rather than defaulting to DS: an unknown
 * segment silently read as DS is an address computed against the wrong base,
 * which is exactly the class of bug a flat model makes invisible until the one
 * process where it is not flat.
 */
static int map_segment(ZydisRegister r, uint8_t *out) {
  switch (r) {
  case ZYDIS_REGISTER_ES:
    *out = (uint8_t)kX86pSegEs;
    return 1;
  case ZYDIS_REGISTER_CS:
    *out = (uint8_t)kX86pSegCs;
    return 1;
  case ZYDIS_REGISTER_SS:
    *out = (uint8_t)kX86pSegSs;
    return 1;
  case ZYDIS_REGISTER_DS:
  case ZYDIS_REGISTER_NONE:
    *out = (uint8_t)kX86pSegDs;
    return 1;
  case ZYDIS_REGISTER_FS:
    *out = (uint8_t)kX86pSegFs;
    return 1;
  case ZYDIS_REGISTER_GS:
    *out = (uint8_t)kX86pSegGs;
    return 1;
  default:
    return 0;
  }
}

static int map_register(ZydisRegister r, int8_t *index, uint8_t *size) {
  ZydisRegisterClass cls = ZydisRegisterGetClass(r);
  ZyanI8 id = ZydisRegisterGetId(r);
  if (id < 0 || id > 7) {
    return 0; /* a 64-bit or extended register cannot appear in 32-bit code */
  }
  switch (cls) {
  case ZYDIS_REGCLASS_GPR8:
    *index = (int8_t)id;
    *size = 1;
    return 1;
  case ZYDIS_REGCLASS_GPR16:
    *index = (int8_t)id;
    *size = 2;
    return 1;
  case ZYDIS_REGCLASS_GPR32:
    *index = (int8_t)id;
    *size = 4;
    return 1;
  default:
    return 0;
  }
}

/* Fill one operand. Returns 0 when it is a shape this framework does not
   model, which makes the whole instruction unsupported rather than partly
   decoded -- a half-filled operand is how an engine executes against a base
   register that was never there. */
static int
map_operand(const ZydisDecodedInstruction *insn, const ZydisDecodedOperand *o, X86pOperand *out, int wide_ok) {
  (void)insn; /* the segment now comes from the operand, not the prefix set */
  memset(out, 0, sizeof *out);
  out->reg = -1;
  out->base = -1;
  out->index = -1;
  out->scale = 1;

  switch (o->type) {
  case ZYDIS_OPERAND_TYPE_REGISTER: {
    uint8_t size = 0;
    if (ZydisRegisterGetClass(o->reg.value) == ZYDIS_REGCLASS_X87) {
      ZyanI8 id = ZydisRegisterGetId(o->reg.value);
      if (id < 0 || id > 7) {
        return 0;
      }
      /* A POSITION, not a register -- kept in its own operand kind so that
         nothing downstream can index the GPR file with it. */
      out->kind = kX86pOperandSt;
      out->reg = (int8_t)id;
      out->size = 10;
      return 1;
    }
    if (ZydisRegisterGetClass(o->reg.value) == ZYDIS_REGCLASS_MMX) {
      ZyanI8 id = ZydisRegisterGetId(o->reg.value);
      if (id < 0 || id > 7) {
        return 0;
      }
      out->kind = kX86pOperandMmx;
      out->reg = (int8_t)id;
      out->size = 8;
      return 1;
    }
    if (ZydisRegisterGetClass(o->reg.value) == ZYDIS_REGCLASS_XMM) {
      ZyanI8 id = ZydisRegisterGetId(o->reg.value);
      /* Only XMM0..XMM7 exist in 32-bit code; a higher number means the bytes
         were decoded as something other than what the guest executes. */
      if (id < 0 || id > 7) {
        return 0;
      }
      out->kind = kX86pOperandXmm;
      out->reg = (int8_t)id;
      out->size = 16;
      return 1;
    }
    if (ZydisRegisterGetClass(o->reg.value) == ZYDIS_REGCLASS_SEGMENT) {
      uint8_t sreg = 0;
      if (!map_segment(o->reg.value, &sreg)) {
        return 0;
      }
      out->kind = kX86pOperandSeg;
      out->reg = (int8_t)sreg;
      /* A selector is sixteen bits wide wherever it appears. MOV r32, Sreg
         zero-extends into the destination, and recording the operand as four
         bytes would make the write look like a full-width move. */
      out->size = 2;
      return 1;
    }
    if (!map_register(o->reg.value, &out->reg, &size)) {
      return 0;
    }
    out->kind = kX86pOperandReg;
    out->size = size;
    return 1;
  }
  case ZYDIS_OPERAND_TYPE_MEMORY: {
    /* Which segment the address is relative to. Recorded rather than refused:
       FS-relative accesses are how a Win32 guest reaches its TEB, and there
       are 3,350 of them in X-Men Legends II alone. Only FS and GS have a base
       to add -- see cpu.h on why that is a contract and not an assumption. */
    if (!map_segment(o->mem.segment, &out->seg)) {
      return 0;
    }
    if (o->mem.base != ZYDIS_REGISTER_NONE) {
      uint8_t bsize = 0;
      if (!map_register(o->mem.base, &out->base, &bsize) || bsize != 4) {
        return 0; /* 16-bit addressing is not something this guest emits */
      }
    }
    if (o->mem.index != ZYDIS_REGISTER_NONE) {
      uint8_t isize = 0;
      if (!map_register(o->mem.index, &out->index, &isize) || isize != 4) {
        return 0;
      }
    }
    out->scale = o->mem.scale ? o->mem.scale : 1;
    out->disp = o->mem.disp.has_displacement ? (int32_t)o->mem.disp.value : 0;
    out->kind = kX86pOperandMem;
    out->size = (uint8_t)(o->size / 8);
    if (out->size == 1 || out->size == 2 || out->size == 4) {
      return 1;
    }
    /*
     * The wide widths -- 8 and 10 for x87's double and extended, 8 and 16 for
     * MMX and SSE -- are accepted only for the instruction families that have
     * a register wide enough to hold them.
     *
     * Gated rather than allowed unconditionally, because an integer
     * instruction with an 8- or 16-byte memory operand is not an instruction
     * this framework decoded correctly, and letting it through would hand the
     * engine an operand it would then read four bytes of.
     */
    if (wide_ok && (out->size == 8 || out->size == 10 || out->size == 16)) {
      return 1;
    }
    return 0;
  }
  case ZYDIS_OPERAND_TYPE_IMMEDIATE:
    out->kind = kX86pOperandImm;
    /* Sign-extension has ALREADY happened, by Zydis, per the encoding: a
       byte immediate in `ADD EAX, imm8` really does mean a sign-extended
       dword, and leaving it to each caller is how one of them forgets. */
    out->imm = (uint32_t)o->imm.value.u;
    out->relative = o->imm.is_relative ? 1u : 0u;
    out->size = (uint8_t)(o->size / 8);
    if (out->size == 0 || out->size > 4) {
      out->size = 4;
    }
    return 1;
  default:
    return 0;
  }
}

/* Mnemonic -> what we model. Anything absent stays Unsupported, which is a
   REPORTABLE outcome carrying the mnemonic, not a silent no-op. */
/*
 * Which repeat prefix a string instruction carries.
 *
 * ZYDIS_ATTRIB_HAS_REP is F3 and HAS_REPNE is F2; Zydis also spells F3 on
 * SCAS/CMPS as REPE, which is the same byte. Checked in that order so a
 * decoder update that starts reporting the REPE spelling cannot silently turn
 * a repeat into no repeat.
 */
static X86pRepKind string_rep(const ZydisDecodedInstruction *insn) {
  if (insn->attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE)) {
    return kX86pRepRep;
  }
  if (insn->attributes & (ZYDIS_ATTRIB_HAS_REPNE)) {
    return kX86pRepRepne;
  }
  return kX86pRepNone;
}

static void map_mnemonic(const ZydisDecodedInstruction *insn, X86pInsn *out) {
  switch (insn->mnemonic) {
#define ALU(m, a)                                                                                                      \
  case m:                                                                                                              \
    out->op = kX86pInsnAlu;                                                                                            \
    out->alu = (uint8_t)(a);                                                                                           \
    return
    ALU(ZYDIS_MNEMONIC_ADD, kX86pAluAdd);
    ALU(ZYDIS_MNEMONIC_OR, kX86pAluOr);
    ALU(ZYDIS_MNEMONIC_ADC, kX86pAluAdc);
    ALU(ZYDIS_MNEMONIC_SBB, kX86pAluSbb);
    ALU(ZYDIS_MNEMONIC_AND, kX86pAluAnd);
    ALU(ZYDIS_MNEMONIC_SUB, kX86pAluSub);
    ALU(ZYDIS_MNEMONIC_XOR, kX86pAluXor);
    ALU(ZYDIS_MNEMONIC_CMP, kX86pAluCmp);
    ALU(ZYDIS_MNEMONIC_TEST, kX86pAluTest);
    ALU(ZYDIS_MNEMONIC_SHL, kX86pAluShl);
    ALU(ZYDIS_MNEMONIC_SHR, kX86pAluShr);
    ALU(ZYDIS_MNEMONIC_SAR, kX86pAluSar);
    ALU(ZYDIS_MNEMONIC_ROL, kX86pAluRol);
    ALU(ZYDIS_MNEMONIC_ROR, kX86pAluRor);
    ALU(ZYDIS_MNEMONIC_RCL, kX86pAluRcl);
    ALU(ZYDIS_MNEMONIC_RCR, kX86pAluRcr);
#undef ALU
#define UN(m, a)                                                                                                       \
  case m:                                                                                                              \
    out->op = kX86pInsnAluUnary;                                                                                       \
    out->alu = (uint8_t)(a);                                                                                           \
    return
    UN(ZYDIS_MNEMONIC_NOT, kX86pAluNot);
    UN(ZYDIS_MNEMONIC_NEG, kX86pAluNeg);
    UN(ZYDIS_MNEMONIC_INC, kX86pAluInc);
    UN(ZYDIS_MNEMONIC_DEC, kX86pAluDec);
#undef UN
#define SIMPLE(m, o)                                                                                                   \
  case m:                                                                                                              \
    out->op = (uint8_t)(o);                                                                                            \
    return
    SIMPLE(ZYDIS_MNEMONIC_MOV, kX86pInsnMov);
    SIMPLE(ZYDIS_MNEMONIC_MOVZX, kX86pInsnMovzx);
    SIMPLE(ZYDIS_MNEMONIC_MOVSX, kX86pInsnMovsx);
    SIMPLE(ZYDIS_MNEMONIC_LEA, kX86pInsnLea);
    SIMPLE(ZYDIS_MNEMONIC_PUSH, kX86pInsnPush);
    SIMPLE(ZYDIS_MNEMONIC_POP, kX86pInsnPop);
    SIMPLE(ZYDIS_MNEMONIC_XCHG, kX86pInsnXchg);
    SIMPLE(ZYDIS_MNEMONIC_JMP, kX86pInsnJmp);
    SIMPLE(ZYDIS_MNEMONIC_CALL, kX86pInsnCall);
    SIMPLE(ZYDIS_MNEMONIC_RET, kX86pInsnRet);
    SIMPLE(ZYDIS_MNEMONIC_LEAVE, kX86pInsnLeave);
    SIMPLE(ZYDIS_MNEMONIC_NOP, kX86pInsnNop);
    SIMPLE(ZYDIS_MNEMONIC_CDQ, kX86pInsnCdq);
    SIMPLE(ZYDIS_MNEMONIC_CWDE, kX86pInsnCwde);
    SIMPLE(ZYDIS_MNEMONIC_MUL, kX86pInsnMul);
    SIMPLE(ZYDIS_MNEMONIC_IMUL, kX86pInsnImul);
    SIMPLE(ZYDIS_MNEMONIC_DIV, kX86pInsnDiv);
    SIMPLE(ZYDIS_MNEMONIC_IDIV, kX86pInsnIdiv);
    SIMPLE(ZYDIS_MNEMONIC_PUSHFD, kX86pInsnPushfd);
    SIMPLE(ZYDIS_MNEMONIC_POPFD, kX86pInsnPopfd);
    SIMPLE(ZYDIS_MNEMONIC_CLD, kX86pInsnCld);
    SIMPLE(ZYDIS_MNEMONIC_STD, kX86pInsnStd);
    SIMPLE(ZYDIS_MNEMONIC_SHLD, kX86pInsnShld);
    SIMPLE(ZYDIS_MNEMONIC_SHRD, kX86pInsnShrd);
    SIMPLE(ZYDIS_MNEMONIC_SAHF, kX86pInsnSahf);
    SIMPLE(ZYDIS_MNEMONIC_LAHF, kX86pInsnLahf);
    SIMPLE(ZYDIS_MNEMONIC_STC, kX86pInsnStc);
    SIMPLE(ZYDIS_MNEMONIC_CLC, kX86pInsnClc);
    SIMPLE(ZYDIS_MNEMONIC_CMC, kX86pInsnCmc);
    SIMPLE(ZYDIS_MNEMONIC_SALC, kX86pInsnSalc);
    SIMPLE(ZYDIS_MNEMONIC_XLAT, kX86pInsnXlat);
    SIMPLE(ZYDIS_MNEMONIC_PUSHAD, kX86pInsnPushad);
    SIMPLE(ZYDIS_MNEMONIC_POPAD, kX86pInsnPopad);
    SIMPLE(ZYDIS_MNEMONIC_ENTER, kX86pInsnEnter);
    SIMPLE(ZYDIS_MNEMONIC_LOOP, kX86pInsnLoop);
    SIMPLE(ZYDIS_MNEMONIC_LOOPE, kX86pInsnLoope);
    SIMPLE(ZYDIS_MNEMONIC_LOOPNE, kX86pInsnLoopne);
    SIMPLE(ZYDIS_MNEMONIC_JECXZ, kX86pInsnJecxz);
#undef SIMPLE

#define BCD(m, k)                                                                                                      \
  case m:                                                                                                              \
    out->op = (uint8_t)kX86pInsnBcd;                                                                                   \
    out->bcd = (uint8_t)(k);                                                                                           \
    return
    BCD(ZYDIS_MNEMONIC_DAA, kX86pBcdDaa);
    BCD(ZYDIS_MNEMONIC_DAS, kX86pBcdDas);
    BCD(ZYDIS_MNEMONIC_AAA, kX86pBcdAaa);
    BCD(ZYDIS_MNEMONIC_AAS, kX86pBcdAas);
    BCD(ZYDIS_MNEMONIC_AAM, kX86pBcdAam);
    BCD(ZYDIS_MNEMONIC_AAD, kX86pBcdAad);
#undef BCD

#define BIT(m, k)                                                                                                      \
  case m:                                                                                                              \
    out->op = (uint8_t)kX86pInsnBit;                                                                                   \
    out->bit = (uint8_t)(k);                                                                                           \
    return
    BIT(ZYDIS_MNEMONIC_BT, kX86pBitTest);
    BIT(ZYDIS_MNEMONIC_BTS, kX86pBitSet);
    BIT(ZYDIS_MNEMONIC_BTR, kX86pBitReset);
    BIT(ZYDIS_MNEMONIC_BTC, kX86pBitComp);
#undef BIT

#define SIMPLE(m, k)                                                                                                   \
  case m:                                                                                                              \
    out->op = (uint8_t)(k);                                                                                            \
    return
#undef SIMPLE

/*
 * The string operations, keyed on operation and element width.
 *
 * The width is in the MNEMONIC here, not in an operand: Zydis reports the
 * implicit ESI/EDI/EAX operands, but the suffix is the authority on how many
 * bytes move, and reading it from an implicit operand would make the decoder
 * depend on how a third-party library chose to expose them.
 *
 * The repeat prefix is read from the instruction's attributes rather than from
 * the mnemonic, because Zydis spells REP MOVSD as MOVSD with an attribute --
 * see the decode_diff run recorded in CLAUDE.md, where Ghidra folds REP into
 * the mnemonic and Zydis does not.
 */
#define STR(m, k, w)                                                                                                   \
  case m:                                                                                                              \
    out->op = kX86pInsnString;                                                                                         \
    out->str = (uint8_t)(k);                                                                                           \
    out->str_width = (uint8_t)(w);                                                                                     \
    out->rep = (uint8_t)string_rep(insn);                                                                              \
    return
    STR(ZYDIS_MNEMONIC_MOVSB, kX86pStringMovs, 1);
    STR(ZYDIS_MNEMONIC_MOVSW, kX86pStringMovs, 2);
    STR(ZYDIS_MNEMONIC_MOVSD, kX86pStringMovs, 4);
    STR(ZYDIS_MNEMONIC_STOSB, kX86pStringStos, 1);
    STR(ZYDIS_MNEMONIC_STOSW, kX86pStringStos, 2);
    STR(ZYDIS_MNEMONIC_STOSD, kX86pStringStos, 4);
    STR(ZYDIS_MNEMONIC_LODSB, kX86pStringLods, 1);
    STR(ZYDIS_MNEMONIC_LODSW, kX86pStringLods, 2);
    STR(ZYDIS_MNEMONIC_LODSD, kX86pStringLods, 4);
    STR(ZYDIS_MNEMONIC_SCASB, kX86pStringScas, 1);
    STR(ZYDIS_MNEMONIC_SCASW, kX86pStringScas, 2);
    STR(ZYDIS_MNEMONIC_SCASD, kX86pStringScas, 4);
    STR(ZYDIS_MNEMONIC_CMPSB, kX86pStringCmps, 1);
    STR(ZYDIS_MNEMONIC_CMPSW, kX86pStringCmps, 2);
    STR(ZYDIS_MNEMONIC_CMPSD, kX86pStringCmps, 4);
#undef STR
/*
 * x87. The tables are keyed on the SUFFIX as much as the operation: the P is a
 * pop and the R is an operand swap, and both are part of what the instruction
 * does, not decoration on its name.
 */
#define FP(m, k)                                                                                                       \
  case m:                                                                                                              \
    out->op = kX86pInsnX87;                                                                                            \
    out->x87 = (uint8_t)(k);                                                                                           \
    return
#define FPP(m, k, pops)                                                                                                \
  case m:                                                                                                              \
    out->op = kX86pInsnX87;                                                                                            \
    out->x87 = (uint8_t)(k);                                                                                           \
    out->x87_pops = (uint8_t)(pops);                                                                                   \
    return
#define FPA(m, o, rev, pops, mem_int)                                                                                  \
  case m:                                                                                                              \
    out->op = kX86pInsnX87;                                                                                            \
    out->x87 = (uint8_t)kX86pX87InsnArith;                                                                             \
    out->x87_op = (uint8_t)(o);                                                                                        \
    out->x87_reverse = (uint8_t)(rev);                                                                                 \
    out->x87_pops = (uint8_t)(pops);                                                                                   \
    out->x87_mem_int = (uint8_t)(mem_int);                                                                             \
    return
    FP(ZYDIS_MNEMONIC_FLD, kX86pX87InsnLoad);
    FP(ZYDIS_MNEMONIC_FILD, kX86pX87InsnLoadInt);
    FPP(ZYDIS_MNEMONIC_FST, kX86pX87InsnStore, 0);
    FPP(ZYDIS_MNEMONIC_FSTP, kX86pX87InsnStore, 1);
    FPP(ZYDIS_MNEMONIC_FIST, kX86pX87InsnStoreInt, 0);
    FPP(ZYDIS_MNEMONIC_FISTP, kX86pX87InsnStoreInt, 1);
    FPA(ZYDIS_MNEMONIC_FADD, kX86pX87Add, 0, 0, 0);
    FPA(ZYDIS_MNEMONIC_FADDP, kX86pX87Add, 0, 1, 0);
    FPA(ZYDIS_MNEMONIC_FIADD, kX86pX87Add, 0, 0, 1);
    FPA(ZYDIS_MNEMONIC_FSUB, kX86pX87Sub, 0, 0, 0);
    FPA(ZYDIS_MNEMONIC_FSUBP, kX86pX87Sub, 0, 1, 0);
    FPA(ZYDIS_MNEMONIC_FSUBR, kX86pX87Sub, 1, 0, 0);
    FPA(ZYDIS_MNEMONIC_FSUBRP, kX86pX87Sub, 1, 1, 0);
    FPA(ZYDIS_MNEMONIC_FMUL, kX86pX87Mul, 0, 0, 0);
    FPA(ZYDIS_MNEMONIC_FMULP, kX86pX87Mul, 0, 1, 0);
    FPA(ZYDIS_MNEMONIC_FIMUL, kX86pX87Mul, 0, 0, 1);
    FPA(ZYDIS_MNEMONIC_FISUB, kX86pX87Sub, 0, 0, 1);
    FPA(ZYDIS_MNEMONIC_FISUBR, kX86pX87Sub, 1, 0, 1);
    FPA(ZYDIS_MNEMONIC_FIDIV, kX86pX87Div, 0, 0, 1);
    FPA(ZYDIS_MNEMONIC_FIDIVR, kX86pX87Div, 1, 0, 1);
    FPA(ZYDIS_MNEMONIC_FDIV, kX86pX87Div, 0, 0, 0);
    FPA(ZYDIS_MNEMONIC_FDIVP, kX86pX87Div, 0, 1, 0);
    FPA(ZYDIS_MNEMONIC_FDIVR, kX86pX87Div, 1, 0, 0);
    FPA(ZYDIS_MNEMONIC_FDIVRP, kX86pX87Div, 1, 1, 0);
    FPP(ZYDIS_MNEMONIC_FCOM, kX86pX87InsnCompare, 0);
#define FPI(m, pops)                                                                                                   \
  case m:                                                                                                              \
    out->op = kX86pInsnX87;                                                                                            \
    out->x87 = (uint8_t)kX86pX87InsnCompare;                                                                           \
    out->x87_pops = (uint8_t)(pops);                                                                                   \
    out->x87_mem_int = 1;                                                                                              \
    return
    FPI(ZYDIS_MNEMONIC_FICOM, 0);
    FPI(ZYDIS_MNEMONIC_FICOMP, 1);
#undef FPI
    FPP(ZYDIS_MNEMONIC_FCOMP, kX86pX87InsnCompare, 1);
    FPP(ZYDIS_MNEMONIC_FCOMPP, kX86pX87InsnCompare, 2); /* pops TWICE */
    FPP(ZYDIS_MNEMONIC_FUCOM, kX86pX87InsnCompare, 0);
    FPP(ZYDIS_MNEMONIC_FUCOMP, kX86pX87InsnCompare, 1);
    FPP(ZYDIS_MNEMONIC_FUCOMPP, kX86pX87InsnCompare, 2);
    FP(ZYDIS_MNEMONIC_FXCH, kX86pX87InsnExchange);
    FP(ZYDIS_MNEMONIC_FCHS, kX86pX87InsnChangeSign);
    FP(ZYDIS_MNEMONIC_FABS, kX86pX87InsnAbs);
    FP(ZYDIS_MNEMONIC_FLDZ, kX86pX87InsnConstZero);
    FP(ZYDIS_MNEMONIC_FLD1, kX86pX87InsnConstOne);
    FP(ZYDIS_MNEMONIC_FLDPI, kX86pX87InsnConstPi);
    FP(ZYDIS_MNEMONIC_FNSTSW, kX86pX87InsnStoreStatus);
    FP(ZYDIS_MNEMONIC_FLDCW, kX86pX87InsnLoadControl);
    FP(ZYDIS_MNEMONIC_FNSTCW, kX86pX87InsnStoreControl);
    FP(ZYDIS_MNEMONIC_FFREE, kX86pX87InsnFree);
    FP(ZYDIS_MNEMONIC_FNINIT, kX86pX87InsnInit);
    FP(ZYDIS_MNEMONIC_FWAIT, kX86pX87InsnWait);
    FP(ZYDIS_MNEMONIC_FNCLEX, kX86pX87InsnClearExc);
    FP(ZYDIS_MNEMONIC_FTST, kX86pX87InsnTest);
    FP(ZYDIS_MNEMONIC_FLDL2E, kX86pX87InsnConstLog2E);
    FP(ZYDIS_MNEMONIC_FLDL2T, kX86pX87InsnConstLog2T);
    FP(ZYDIS_MNEMONIC_FLDLN2, kX86pX87InsnConstLn2);
    FP(ZYDIS_MNEMONIC_FLDLG2, kX86pX87InsnConstLog102);
    /* The comparisons that write EFLAGS instead of the condition codes. The
       P suffix is a pop, exactly as it is on FCOM -- carried as a count, not
       folded into the name. */
    FPP(ZYDIS_MNEMONIC_FCOMI, kX86pX87InsnCompareInt, 0);
    FPP(ZYDIS_MNEMONIC_FUCOMI, kX86pX87InsnCompareInt, 0);
    FPP(ZYDIS_MNEMONIC_FCOMIP, kX86pX87InsnCompareInt, 1);
    FPP(ZYDIS_MNEMONIC_FUCOMIP, kX86pX87InsnCompareInt, 1);
#undef FP

/*
 * The functions evaluated on the host's own x87 unit. One instruction kind
 * and a function selector, because what the decoder has to say about them is
 * identical and only the opcode differs.
 */
#define FN(m, k)                                                                                                       \
  case m:                                                                                                              \
    out->op = kX86pInsnX87;                                                                                            \
    out->x87 = (uint8_t)kX86pX87InsnFn;                                                                                \
    out->x87_fn = (uint8_t)(k);                                                                                        \
    return
    FN(ZYDIS_MNEMONIC_FSQRT, kX86pX87FnSqrt);
    FN(ZYDIS_MNEMONIC_FSIN, kX86pX87FnSin);
    FN(ZYDIS_MNEMONIC_FCOS, kX86pX87FnCos);
    FN(ZYDIS_MNEMONIC_FSINCOS, kX86pX87FnSinCos);
    FN(ZYDIS_MNEMONIC_FPTAN, kX86pX87FnPtan);
    FN(ZYDIS_MNEMONIC_FPATAN, kX86pX87FnPatan);
    FN(ZYDIS_MNEMONIC_FYL2X, kX86pX87FnYl2x);
    FN(ZYDIS_MNEMONIC_FYL2XP1, kX86pX87FnYl2xp1);
    FN(ZYDIS_MNEMONIC_F2XM1, kX86pX87Fn2xm1);
    FN(ZYDIS_MNEMONIC_FSCALE, kX86pX87FnScale);
    FN(ZYDIS_MNEMONIC_FRNDINT, kX86pX87FnRndint);
    FN(ZYDIS_MNEMONIC_FXTRACT, kX86pX87FnXtract);
    FN(ZYDIS_MNEMONIC_FPREM, kX86pX87FnPrem);
    FN(ZYDIS_MNEMONIC_FPREM1, kX86pX87FnPrem1);
#undef FN
#undef FPP
#undef FPA
  default:
    break;
  }
  /* The conditional families. Zydis names each condition as its own mnemonic,
     so this is the one place the 16-way fan-out is written; everything
     downstream carries an X86pCond. */
#define CC(jm, sm, cm, c)                                                                                              \
  if (insn->mnemonic == (jm)) {                                                                                        \
    out->op = kX86pInsnJcc;                                                                                            \
    out->cond = (uint8_t)(c);                                                                                          \
    return;                                                                                                            \
  }                                                                                                                    \
  if (insn->mnemonic == (sm)) {                                                                                        \
    out->op = kX86pInsnSetcc;                                                                                          \
    out->cond = (uint8_t)(c);                                                                                          \
    return;                                                                                                            \
  }                                                                                                                    \
  if (insn->mnemonic == (cm)) {                                                                                        \
    out->op = kX86pInsnCmovcc;                                                                                         \
    out->cond = (uint8_t)(c);                                                                                          \
    return;                                                                                                            \
  }
  CC(ZYDIS_MNEMONIC_JO, ZYDIS_MNEMONIC_SETO, ZYDIS_MNEMONIC_CMOVO, kX86pCondO)
  CC(ZYDIS_MNEMONIC_JNO, ZYDIS_MNEMONIC_SETNO, ZYDIS_MNEMONIC_CMOVNO, kX86pCondNO)
  CC(ZYDIS_MNEMONIC_JB, ZYDIS_MNEMONIC_SETB, ZYDIS_MNEMONIC_CMOVB, kX86pCondB)
  CC(ZYDIS_MNEMONIC_JNB, ZYDIS_MNEMONIC_SETNB, ZYDIS_MNEMONIC_CMOVNB, kX86pCondNB)
  CC(ZYDIS_MNEMONIC_JZ, ZYDIS_MNEMONIC_SETZ, ZYDIS_MNEMONIC_CMOVZ, kX86pCondZ)
  CC(ZYDIS_MNEMONIC_JNZ, ZYDIS_MNEMONIC_SETNZ, ZYDIS_MNEMONIC_CMOVNZ, kX86pCondNZ)
  CC(ZYDIS_MNEMONIC_JBE, ZYDIS_MNEMONIC_SETBE, ZYDIS_MNEMONIC_CMOVBE, kX86pCondBE)
  CC(ZYDIS_MNEMONIC_JNBE, ZYDIS_MNEMONIC_SETNBE, ZYDIS_MNEMONIC_CMOVNBE, kX86pCondA)
  CC(ZYDIS_MNEMONIC_JS, ZYDIS_MNEMONIC_SETS, ZYDIS_MNEMONIC_CMOVS, kX86pCondS)
  CC(ZYDIS_MNEMONIC_JNS, ZYDIS_MNEMONIC_SETNS, ZYDIS_MNEMONIC_CMOVNS, kX86pCondNS)
  CC(ZYDIS_MNEMONIC_JP, ZYDIS_MNEMONIC_SETP, ZYDIS_MNEMONIC_CMOVP, kX86pCondP)
  CC(ZYDIS_MNEMONIC_JNP, ZYDIS_MNEMONIC_SETNP, ZYDIS_MNEMONIC_CMOVNP, kX86pCondNP)
  CC(ZYDIS_MNEMONIC_JL, ZYDIS_MNEMONIC_SETL, ZYDIS_MNEMONIC_CMOVL, kX86pCondL)
  CC(ZYDIS_MNEMONIC_JNL, ZYDIS_MNEMONIC_SETNL, ZYDIS_MNEMONIC_CMOVNL, kX86pCondGE)
  CC(ZYDIS_MNEMONIC_JLE, ZYDIS_MNEMONIC_SETLE, ZYDIS_MNEMONIC_CMOVLE, kX86pCondLE)
  CC(ZYDIS_MNEMONIC_JNLE, ZYDIS_MNEMONIC_SETNLE, ZYDIS_MNEMONIC_CMOVNLE, kX86pCondG)
#undef CC

  /*
   * MMX, SSE and 3DNow!, matched by NAME rather than by Zydis mnemonic
   * constant.
   *
   * Everything above pairs a Zydis enumerator with a meaning, which is the
   * right shape when there are thirty of them. There are a hundred and thirty
   * here, and the tables that give them meaning already exist in simd.c and
   * three_dnow.c keyed by the spelling a disassembler prints -- so pairing
   * them again with enumerators would be a second table to keep in step, whose
   * failure mode is one instruction quietly meaning another.
   *
   * The spellings are the ones the decode_diff run over 2.1M instructions
   * confirmed, including the two Zydis 4.1 renders a letter short (PFSQRT for
   * PFRSQRT, PFCPIT1 for PFRCPIT1) -- three_dnow.c accepts both because of
   * that run.
   */
  {
    const char *name = upper_mnemonic(insn->mnemonic);
    X86pSimdOp sop;
    X86pPfOp pfop;
    if (x86p_simd_parse(name, &sop)) {
      out->op = kX86pInsnSimd;
      out->simd = (uint8_t)sop;
      return;
    }
    if (x86p_3dnow_parse(name, &pfop)) {
      out->op = kX86pInsnSimd;
      out->simd = (uint8_t)kX86pSimdPf;
      out->pf = (uint8_t)pfop;
      return;
    }
  }

  out->op = kX86pInsnUnsupported;
}

uint32_t x86p_decode(const uint8_t *bytes, size_t len, X86pInsn *out) {
  static ZydisDecoder decoder;
  static int decoder_ready;
  ZydisDecodedInstruction insn;
  ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

  if (!bytes || !out || len == 0) {
    return 0;
  }
  if (len > X86P_MAX_INSN_LEN) {
    len = X86P_MAX_INSN_LEN;
  }

  if (!decoder_ready) {
    /* 32-bit protected mode with a 32-bit stack: the guest is a Win32
       process, and there is no 16-bit or long-mode code in it. A decoder
       configured for the wrong mode reads the same bytes as different
       instructions rather than failing, so this is stated once and never
       parameterised by a caller. */
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32))) {
      return 0;
    }
    decoder_ready = 1;
  }

  if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, (const void *)bytes, len, &insn, ops))) {
    return 0;
  }
  if (insn.length == 0) { /* cannot happen on success; a zero would loop a caller forever */
    return 0;
  }

  memset(out, 0, sizeof *out);
  out->length = insn.length;
  out->mnemonic = upper_mnemonic(insn.mnemonic);
  out->operands = insn.operand_count_visible;
  map_mnemonic(&insn, out);

  if (out->operands > X86P_MAX_OPERANDS) {
    /* More explicit operands than this model holds. Refused by name rather
       than truncated: an instruction executed without its third operand is
       a different instruction. */
    out->op = kX86pInsnUnsupported;
  } else {
    int i;
    for (i = 0; i < out->operands; i++) {
      if (!map_operand(&insn, &ops[i], &out->operand[i], out->op == kX86pInsnX87 || out->op == kX86pInsnSimd)) {
        /* One unmodelled operand makes the whole instruction unsupported.
           A half-filled operand is how an engine executes against a base
           register that was never there. */
        out->op = kX86pInsnUnsupported;
        break;
      }
    }
  }
  return insn.length;
}

const char *x86p_decoder_id(void) {
  static char id[64];
  if (id[0] == '\0') {
    snprintf(id,
             sizeof id,
             "zydis %d.%d.%d",
             (int)((ZYDIS_VERSION >> 48) & 0xFFFF),
             (int)((ZYDIS_VERSION >> 32) & 0xFFFF),
             (int)((ZYDIS_VERSION >> 16) & 0xFFFF));
  }
  return id;
}
