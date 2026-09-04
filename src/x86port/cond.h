/*
 * cond.h -- the 16 condition codes, evaluated from the flag state.
 *
 * WHY THIS IS ITS OWN MODULE. The conditions are shared by three instruction
 * families that arrive at different times -- Jcc (8.0% of the corpus), SETcc,
 * and CMOVcc. Writing the condition three times is how JGE and JNL, which are
 * the same condition, come to disagree.
 *
 * The low four bits of a Jcc, SETcc or CMOVcc opcode ARE the condition number,
 * so this enum is indexed by that nibble directly rather than through a table
 * that can drift from the encoding.
 *
 * The signed/unsigned pairs are the trap. JB and JL both read "less than" in
 * English and test entirely different flags -- CF for the unsigned one, SF!=OF
 * for the signed. Choosing wrong is a comparison that works for small positive
 * values and fails across the sign boundary, which is exactly the input a test
 * written by hand tends not to have. So this is checked exhaustively against
 * hardware instead: 64 flag states x 16 conditions, every one of them.
 */
#ifndef X86PORT_COND_H
#define X86PORT_COND_H

#include "flags.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Condition numbers as the encoding numbers them: opcode 0x70+cc, 0x0F 0x80+cc,
   0x0F 0x90+cc, 0x0F 0x40+cc. Do not reorder. */
typedef enum X86pCond {
  kX86pCondO = 0x0,  /* OF */
  kX86pCondNO = 0x1, /* !OF */
  kX86pCondB = 0x2,  /* CF          -- unsigned below (JB/JC/JNAE) */
  kX86pCondNB = 0x3, /* !CF         -- unsigned above or equal */
  kX86pCondZ = 0x4,  /* ZF */
  kX86pCondNZ = 0x5, /* !ZF */
  kX86pCondBE = 0x6, /* CF | ZF     -- unsigned below or equal */
  kX86pCondA = 0x7,  /* !CF & !ZF   -- unsigned above */
  kX86pCondS = 0x8,  /* SF */
  kX86pCondNS = 0x9, /* !SF */
  kX86pCondP = 0xA,  /* PF */
  kX86pCondNP = 0xB, /* !PF */
  kX86pCondL = 0xC,  /* SF != OF    -- SIGNED less */
  kX86pCondGE = 0xD, /* SF == OF    -- signed greater or equal */
  kX86pCondLE = 0xE, /* ZF | (SF != OF) -- signed less or equal */
  kX86pCondG = 0xF,  /* !ZF & (SF == OF) -- signed greater */
  kX86pCondCount     /* MUST stay last: 16, and the denominator */
} X86pCond;

/* Evaluate a condition against the flag state. Returns 0 or 1 for every input;
   a condition number outside the enum returns 0 and is reported by name as
   "??" rather than indexing off the end of a table. */
int x86p_cond(X86pCond cc, const X86pFlags *f);

/* The same, from a materialised EFLAGS word, for a caller that has one (a
   divergence report comparing against hardware, say). */
int x86p_cond_eflags(X86pCond cc, uint32_t eflags);

/* The canonical mnemonic suffix -- "Z", "NZ", "GE" -- so a trace says `JGE`
   rather than `Jcc(0xD)`. Never null. */
const char *x86p_cond_name(X86pCond cc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_COND_H */
