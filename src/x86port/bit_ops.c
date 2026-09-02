/* bit_ops.c -- see bit_ops.h. */
#include "bit_ops.h"

#include "flags.h"

static const char *kNames[] = {"BT", "BTS", "BTR", "BTC"};
_Static_assert((int)(sizeof kNames / sizeof kNames[0]) == (int)kX86pBitOpCount, "every X86pBitOp needs a name");

const char *x86p_bit_op_name(X86pBitOp op) {
  if ((unsigned)op >= (unsigned)kX86pBitOpCount) {
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

static uint32_t width_mask(int w) {
  return (w >= 4) ? 0xFFFFFFFFu : ((1u << (unsigned)(w * 8)) - 1u);
}

uint32_t x86p_bit_apply(X86pBitOp op, uint32_t value, unsigned bit, uint32_t *eflags) {
  const uint32_t sel = 1u << (bit & 31u);
  set_bit(eflags, X86P_CF, (value & sel) != 0u);
  switch (op) {
  case kX86pBitSet:
    return value | sel;
  case kX86pBitReset:
    return value & ~sel;
  case kX86pBitComp:
    return value ^ sel;
  case kX86pBitTest:
  case kX86pBitOpCount:
  default:
    return value;
  }
}

int x86p_double_shift(
    int left, uint32_t dst, uint32_t src, unsigned count, int w, uint32_t *result, uint32_t *eflags, int *defined) {
  const unsigned bits = (unsigned)(w * 8);
  const uint32_t mask = width_mask(w);
  uint32_t f = *eflags;
  uint32_t r;
  uint32_t prev; /* the result one shift earlier -- see the OF comment below */
  int cf;
  unsigned parity = 0u;
  unsigned i;

  /* The mask is 0x1F even at 16-bit width -- it comes from the operand SIZE
     the CPU uses for the count, which is always 32 bits in 32-bit mode. This
     is why a 16-bit double shift can be handed a count of 20 at all. */
  count &= 31u;
  /*
   * A count of the operand width or more is outside the defined domain: Intel
   * says "greater than the operand size", AMD says "greater than or equal to",
   * and the silicon settles it -- a 16-bit SHLD by exactly 16 produces an OF
   * that no reading of the result explains, so the domain is AMD's. This only
   * ever arises at 16-bit width, since a 32-bit count is masked to 31.
   */
  *defined = (count < bits);
  if (count == 0u) {
    return 0; /* writes nothing, flags included */
  }
  if (!*defined) {
    /* Undefined. Report it and leave the caller to decide; producing a value
       here would be a guess wearing the costume of a result. */
    return 0;
  }

  dst &= mask;
  src &= mask;
  if (left) {
    r = (dst << count) | (src >> (bits - count));
    prev = (count == 1u) ? dst : ((dst << (count - 1u)) | (src >> (bits - count + 1u)));
    cf = (int)((dst >> (bits - count)) & 1u);
  } else {
    r = (dst >> count) | (src << (bits - count));
    prev = (count == 1u) ? dst : ((dst >> (count - 1u)) | (src << (bits - count + 1u)));
    cf = (int)((dst >> (count - 1u)) & 1u);
  }
  r &= mask;
  prev &= mask;

  set_bit(&f, X86P_CF, cf);
  set_bit(&f, X86P_SF, (r >> (bits - 1u)) & 1u);
  set_bit(&f, X86P_ZF, r == 0u);
  for (i = 0; i < 8u; i++) {
    parity ^= (unsigned)((r >> i) & 1u);
  }
  set_bit(&f, X86P_PF, parity == 0u);
  /* AF is undefined for the double shifts, and this CPU SETS it -- the same
     behaviour flags.c measured for the ordinary shifts, and the same reason
     for modelling it: a guest that pushes EFLAGS can see the bit. */
  set_bit(&f, X86P_AF, 1);
  /*
   * OF, which the SDM defines only for a count of one and the hardware writes
   * for every count. The rule is the LAST SINGLE-BIT STEP: these are microcoded
   * as a sequence of one-bit shifts, and OF says the sign changed on the final
   * one. For a count of one that reduces to "sign(result) differs from
   * sign(destination)", which is exactly what the manual documents.
   *
   * The manual's rule extended naively to every count -- comparing the result
   * against the ORIGINAL destination -- agrees on a great many inputs and is
   * wrong on the rest: SHRD of 0 by 11 with all-ones filling in produces a
   * negative result from a positive destination, and the hardware reports no
   * overflow because the sign did not change on the last step.
   */
  set_bit(&f, X86P_OF, (((r ^ prev) >> (bits - 1u)) & 1u) != 0u);

  *result = r;
  *eflags = f;
  return 1;
}
