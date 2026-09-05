/* bcd.c -- see bcd.h. The pseudocode is the SDM's, transcribed rather than
   reasoned about, because these are the instructions where a plausible-looking
   simplification is wrong: DAA clears CF when its second test fails and DAS
   does not, and that asymmetry is real. */
#include "bcd.h"

#include "flags.h"

static const char *kNames[] = {"DAA", "DAS", "AAA", "AAS", "AAM", "AAD"};
_Static_assert((int)(sizeof kNames / sizeof kNames[0]) == (int)kX86pBcdOpCount, "every X86pBcdOp needs a name");

const char *x86p_bcd_op_name(X86pBcdOp op) {
  if ((unsigned)op >= (unsigned)kX86pBcdOpCount) {
    return "unknown";
  }
  return kNames[(int)op];
}

static void set_bit(uint32_t *w, uint32_t bit, int on) {
  if (on) {
    *w |= bit;
  } else {
    *w &= ~bit;
  }
}

/*
 * OF is undefined for all six forms. The deterministic policy derived from the
 * original reference host models it as signed overflow of the last adjustment
 * actually performed, or clear when no adjustment happened.
 *
 * The obvious reading, "OF says
 * the sign changed", fits every case in the first sweep except DAA on 0x9A,
 * where the value wraps from 0x9A to 0x00 across two adjustments and the sign
 * plainly changes while the hardware reports no overflow. See
 * tests/test_integer_tail.c, which runs these in a 32-bit code segment because
 * long mode dropped the opcodes.
 */
typedef struct LastAdjust {
  uint8_t a, b, r;
  int subtract;
  int happened;
} LastAdjust;

static void apply_of(uint32_t *w, const LastAdjust *l) {
  int of;
  if (!l->happened) {
    set_bit(w, X86P_OF, 0); /* measured: no adjustment, no overflow */
    return;
  }
  of = l->subtract ? (((l->a ^ l->b) & (l->a ^ l->r) & 0x80u) != 0u)
                   : (((uint8_t)(l->a ^ l->r) & (uint8_t)(l->b ^ l->r) & 0x80u) != 0u);
  set_bit(w, X86P_OF, of);
}

/* SF, ZF and PF, which all six define over the resulting AL. */
static void set_result_flags(uint32_t *w, uint8_t al) {
  unsigned parity = 0u;
  unsigned i;
  for (i = 0; i < 8u; i++) {
    parity ^= (unsigned)((al >> i) & 1u);
  }
  set_bit(w, X86P_SF, (al & 0x80u) != 0u);
  set_bit(w, X86P_ZF, al == 0u);
  set_bit(w, X86P_PF, parity == 0u); /* PF is EVEN parity of the low byte */
}

int x86p_bcd_apply(X86pBcdOp op, uint16_t *ax, uint32_t *eflags, uint8_t imm) {
  uint8_t al = (uint8_t)(*ax & 0xFFu);
  uint8_t ah = (uint8_t)(*ax >> 8);
  uint32_t f = *eflags;
  const int cf_in = (f & X86P_CF) != 0u;
  const int af_in = (f & X86P_AF) != 0u;

  switch (op) {
  case kX86pBcdDaa: {
    const uint8_t old_al = al;
    LastAdjust last = {0u, 0u, 0u, 0, 0};
    if ((al & 0x0Fu) > 9u || af_in) {
      const int carry = ((unsigned)al + 6u) > 0xFFu;
      last.a = al;
      last.b = 6u;
      al = (uint8_t)(al + 6u);
      last.r = al;
      last.happened = 1;
      set_bit(&f, X86P_CF, cf_in || carry);
      set_bit(&f, X86P_AF, 1);
    } else {
      set_bit(&f, X86P_AF, 0);
    }
    if (old_al > 0x99u || cf_in) {
      last.a = al;
      last.b = 0x60u;
      al = (uint8_t)(al + 0x60u);
      last.r = al;
      last.happened = 1;
      set_bit(&f, X86P_CF, 1);
    } else {
      set_bit(&f, X86P_CF, 0);
    }
    set_result_flags(&f, al);
    apply_of(&f, &last);
    break;
  }
  case kX86pBcdDas: {
    const uint8_t old_al = al;
    LastAdjust last = {0u, 0u, 0u, 1, 0};
    if ((al & 0x0Fu) > 9u || af_in) {
      const int borrow = al < 6u;
      last.a = al;
      last.b = 6u;
      al = (uint8_t)(al - 6u);
      last.r = al;
      last.happened = 1;
      set_bit(&f, X86P_CF, cf_in || borrow);
      set_bit(&f, X86P_AF, 1);
    } else {
      set_bit(&f, X86P_AF, 0);
    }
    /* No else-clause here, unlike DAA: DAS leaves CF as the first block left
       it. Transcribed from the SDM; the two are not symmetrical. */
    if (old_al > 0x99u || cf_in) {
      last.a = al;
      last.b = 0x60u;
      al = (uint8_t)(al - 0x60u);
      last.r = al;
      last.happened = 1;
      set_bit(&f, X86P_CF, 1);
    }
    set_result_flags(&f, al);
    apply_of(&f, &last);
    break;
  }
  case kX86pBcdAaa:
  case kX86pBcdAas: {
    const int subtract = (op == kX86pBcdAas);
    if ((al & 0x0Fu) > 9u || af_in) {
      /*
       * The adjustment is on AX, not on AL. Six subtracted from AL alone loses
       * the borrow into AH, and AAS on AX = 0x0000 then leaves 0xFF0A where
       * the hardware leaves 0xFE0A -- AH decremented once by the borrow and
       * once by the instruction. Measured; the byte-wise version passed every
       * case that did not borrow.
       */
      uint16_t ax16 = (uint16_t)(((uint16_t)ah << 8) | al);
      ax16 = (uint16_t)(subtract ? (ax16 - 6u) : (ax16 + 6u));
      ah = (uint8_t)((ax16 >> 8) + (subtract ? 0xFFu : 1u));
      al = (uint8_t)(ax16 & 0xFFu);
      set_bit(&f, X86P_AF, 1);
      set_bit(&f, X86P_CF, 1);
    } else {
      set_bit(&f, X86P_AF, 0);
      set_bit(&f, X86P_CF, 0);
    }
    /* SF, ZF and PF are undefined. The deterministic policy derives them from
       AL before the low-nibble mask, matching the original reference host. */
    set_result_flags(&f, al);
    /* ... except SF, which is bit 15 of the WHOLE AX, not the sign of AL.
       AAS on AX = 0x0009 with AF set leaves AX = 0xFF03: AL is positive and
       the hardware reports negative, because the adjustment borrowed into AH
       and the sign it reports is the sixteen-bit one. */
    set_bit(&f, X86P_SF, ((((uint16_t)ah << 8) | al) & 0x8000u) != 0u);
    set_bit(&f, X86P_OF, 0);
    al &= 0x0Fu;
    break;
  }
  case kX86pBcdAam:
    if (imm == 0u) {
      return 0; /* #DE: the guest must receive it */
    }
    ah = (uint8_t)(al / imm);
    al = (uint8_t)(al % imm);
    set_result_flags(&f, al);
    /* CF, AF and OF are undefined. The deterministic reference-host policy
       clears all three for AAM. */
    f &= ~(uint32_t)(X86P_CF | X86P_AF | X86P_OF);
    break;
  case kX86pBcdAad: {
    /* CF and AF are undefined for AAD. The deterministic policy derives them
       from the internal addition. */
    const unsigned addend = (unsigned)((uint8_t)(ah * imm));
    const unsigned sum = (unsigned)al + addend;
    set_bit(&f, X86P_CF, sum > 0xFFu);
    set_bit(&f, X86P_AF, ((al & 0x0Fu) + (addend & 0x0Fu)) > 0x0Fu);
    /* OF follows signed overflow of that same addition. */
    set_bit(&f, X86P_OF, ((al ^ (uint8_t)sum) & (addend ^ (uint8_t)sum) & 0x80u) != 0u);
    al = (uint8_t)sum;
    ah = 0u;
    set_result_flags(&f, al);
    break;
  }
  case kX86pBcdOpCount:
  default:
    return 0;
  }

  *ax = (uint16_t)(((uint16_t)ah << 8) | al);
  *eflags = f;
  return 1;
}
