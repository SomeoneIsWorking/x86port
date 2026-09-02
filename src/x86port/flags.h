/*
 * flags.h -- EFLAGS, lazily. The single authority on what an arithmetic result
 * did to the condition codes.
 *
 * WHY LAZY AND NOT EAGER. Almost every x86 instruction writes flags and almost
 * none are read: computing six flags per instruction would dominate an
 * interpreter's inner loop and, in a translator, dominate the emitted code. So
 * an operation records what it DID -- kind, operands, result, width -- and each
 * flag is derived when something asks. This is the model `pc/xmen2`'s shipping
 * substrate already uses (`x86rt.h`, FK_* and FLAG_*), and keeping the same
 * model is deliberate: the interpreter has to be comparable to the substrate
 * instruction by instruction, and two different flag representations would make
 * every divergence report start by asking which one was wrong.
 *
 * WHY IT IS AN AUTHORITY AND NOT A COPY. Three things are different here.
 *
 *  - AF EXISTS. The substrate has no auxiliary-carry flag at all. It gets away
 *    with it because the instructions that read AF (DAA, DAS, AAA, AAS) are in
 *    the class its own translator annotates as "embedded data decoded as code"
 *    and never executes. An interpreter cannot make that bet: it decodes what
 *    execution actually reaches, and a reference implementation that silently
 *    omits an architectural flag is not a reference. PUSHFD also round-trips
 *    the whole word, so an absent AF is observable without ever executing DAA.
 *  - CARRY-IN IS FIRST CLASS. `a - b - c` is not expressible in the lazy triple.
 *    Modelling it as a SUB of `b - c` gets the borrow wrong whenever a != b, and
 *    that shipped: it made MSVC's `sbb eax,eax; sbb eax,-1` sign idiom return
 *    "equal" whenever CF was set, so every binary search over a string table
 *    took a mismatch for a hit and ARK field names were never bound
 *    (`pc/xmen2` issue #16). ADC/SBB therefore compute their flags eagerly, at
 *    the instruction, and store the result as an explicit word.
 *  - A SHIFT OF ZERO WRITES NOTHING, and that is the INSTRUCTION's rule, not a
 *    derivation. A masked count of zero leaves every flag alone, so the caller
 *    must not record a flag update at all; x86p_flags_set refuses one. The
 *    substrate instead derives CF as `(a >> (count - 1)) & 1`, which shifts by
 *    -1 in that case -- undefined behaviour, whose value on one host is not a
 *    specification -- and derives ZF/SF/PF from the result, which disagreed
 *    with real hardware on every zero-count case measured.
 */
#ifndef X86PORT_FLAGS_H
#define X86PORT_FLAGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EFLAGS bit positions, named once. Every other file asks for a flag by
   accessor; these exist for PUSHFD/POPFD and for tests that speak in words. */
#define X86P_CF (1u << 0)
#define X86P_PF (1u << 2)
#define X86P_AF (1u << 4)
#define X86P_ZF (1u << 6)
#define X86P_SF (1u << 7)
#define X86P_OF (1u << 11)
/* DF is NOT one of the six this model derives -- nothing computes it, CLD and
   STD set it outright, and it survives every arithmetic operation. It lives in
   X86pCpu; the bit position is here so PUSHFD and POPFD have one place to
   agree with. */
#define X86P_DF (1u << 10)

/* Bit 1 reads as 1 on every x86, and IF is set in any user-mode process this
   engine will ever run. A PUSHFD that omitted them would differ from hardware
   in a way guest CPU-detection code notices. */
#define X86P_EFLAGS_FIXED 0x00000202u

/* The six flags this model derives, as one mask -- the denominator for "did we
   account for every flag", and what a differential compares under. */
#define X86P_ARITH_FLAGS (X86P_CF | X86P_PF | X86P_AF | X86P_ZF | X86P_SF | X86P_OF)

/*
 * What the last flag-writing operation was. The kind selects the derivation;
 * it is not a mnemonic, so several instructions legitimately share one.
 */
typedef enum X86pFlagKind {
  kX86pFlagsNone = 0, /* nothing has written flags; every flag reads 0 */
  kX86pFlagsAdd,      /* ADD, and the address arithmetic that reuses it */
  kX86pFlagsSub,      /* SUB and CMP */
  kX86pFlagsLogic,    /* AND/OR/XOR/TEST: CF and OF cleared; AF cleared (measured) */
  kX86pFlagsInc,      /* like Add but CF is PRESERVED, so it is a distinct kind */
  kX86pFlagsDec,      /* like Sub but CF is PRESERVED */
  /*
   * Shifts, split by direction because CF is a DIFFERENT BIT for each and the
   * substrate expresses all three with one formula, `(a >> (count - 1)) & 1`.
   * That is the SHR reading; it is only right for SHL because the caller hands
   * it a pre-arranged value, which is an unstated convention between two files
   * -- exactly the kind an authority must not have. Here `a` is always the
   * value BEFORE the shift and `b` the masked count, and the kind says which
   * bit to take.
   */
  kX86pFlagsShl,      /* CF = bit (width*8 - count) of a; OF = msb(r) ^ CF */
  kX86pFlagsShr,      /* CF = bit (count - 1) of a;       OF = msb(a) */
  kX86pFlagsSar,      /* CF = bit (count - 1) of a;       OF = 0 */
  kX86pFlagsExplicit, /* a real EFLAGS word: POPFD, and eagerly-computed ADC/SBB */
  kX86pFlagsKindCount /* MUST stay last: the denominator of exhaustive checks */
} X86pFlagKind;

/*
 * The lazy state. `w` is the operand width in BYTES (1, 2 or 4) -- flags depend
 * on it, and a byte operation whose width was recorded as 4 gets sign and zero
 * wrong for every value above 0x7F.
 *
 * `carry_in` holds the CF that must survive INC and DEC, which do not write it.
 * The substrate returns 0 for those, which is wrong inside a carry-carrying
 * loop: `inc` between an `add` and an `adc` does not clear the carry on
 * hardware. Nothing analogous is needed for AF, because every kind here states
 * a value for it -- measured, not assumed; see flags.c.
 */
typedef struct X86pFlags {
  uint32_t a; /* first operand, or the EFLAGS word when kind is Explicit */
  uint32_t b; /* second operand, or the shift count */
  uint32_t r; /* the result */
  uint8_t kind;
  uint8_t w;
  uint8_t carry_in; /* CF before an operation that preserves it (INC, DEC) */
} X86pFlags;

/* Record an operation. `w` must be 1, 2 or 4; anything else is a caller defect
   and is reported rather than silently treated as 4. */
void x86p_flags_set(X86pFlags *f, X86pFlagKind kind, uint32_t a, uint32_t b, uint32_t r, int w);

/* Record a real EFLAGS word (POPFD, and the eager ADC/SBB results below). */
void x86p_flags_set_explicit(X86pFlags *f, uint32_t eflags);

/* Individual flags. Each returns 0 or 1, for every input, always. */
int x86p_flag_cf(const X86pFlags *f);
int x86p_flag_pf(const X86pFlags *f);
int x86p_flag_af(const X86pFlags *f);
int x86p_flag_zf(const X86pFlags *f);
int x86p_flag_sf(const X86pFlags *f);
int x86p_flag_of(const X86pFlags *f);

/* The materialised word, including the fixed bits. This is what PUSHFD stores
   and what a differential against hardware compares. */
uint32_t x86p_eflags(const X86pFlags *f);

/*
 * ADC and SBB, computed eagerly, because the lazy triple cannot express a
 * carry-in without getting the borrow wrong -- see the header comment. Both
 * return an EFLAGS word for x86p_flags_set_explicit().
 */
uint32_t x86p_flags_adc(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t r, int w);
uint32_t x86p_flags_sbb(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t r, int w);

/* The value mask for a width: 0xFF, 0xFFFF or 0xFFFFFFFF. Widths outside
   {1,2,4} return 0, which makes a bad width produce visibly wrong values
   rather than plausible ones. */
uint32_t x86p_width_mask(int w);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_FLAGS_H */
