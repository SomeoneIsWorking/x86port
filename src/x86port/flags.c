/* flags.c -- see flags.h for the model and its measured edge cases. */
#include "flags.h"
#include "diagnostic.h"

uint32_t x86p_width_mask(int w) {
  switch (w) {
  case 1:
    return 0xFFu;
  case 2:
    return 0xFFFFu;
  case 4:
    return 0xFFFFFFFFu;
  default:
    /* Not 0xFFFFFFFF. A bad width that silently becomes "32 bits" produces
       flags that are plausible for the wrong operand size, which is the kind
       of wrong that survives review; zero produces flags that are obviously
       broken at the first check. */
    return 0u;
  }
}

static int msb(uint32_t v, int w) {
  if (w < 1 || w > 4) {
    return 0;
  }
  return (int)((v >> (w * 8 - 1)) & 1u);
}

void x86p_flags_set(X86pFlags *f, X86pFlagKind kind, uint32_t a, uint32_t b, uint32_t r, int w) {
  if (!f) {
    return;
  }
  if (w != 1 && w != 2 && w != 4) {
    /* A width outside {1,2,4} is a caller defect with no correct
       interpretation. Guessing one produces flags that look computed. */
    x86p_diagnostic_fatalf("flags", "x86p_flags_set: operand width %d is not 1, 2 or 4", w);
  }
  if ((unsigned)kind >= (unsigned)kX86pFlagsKindCount) {
    x86p_diagnostic_fatalf("flags",
                           "x86p_flags_set: flag kind %u is not one of the %u this build knows",
                           (unsigned)kind,
                           (unsigned)kX86pFlagsKindCount);
  }
  if ((kind == kX86pFlagsShl || kind == kX86pFlagsShr || kind == kX86pFlagsSar) && b == 0) {
    /*
     * A shift by a masked count of zero writes NO FLAGS AT ALL -- not "the same
     * flags", not "CF preserved and the rest derived". Measured against
     * hardware: deriving ZF/SF/PF from the result here disagreed with a real
     * CPU on 512 of 4608 cases, every one of them a zero count.
     *
     * The owner of that rule is the INSTRUCTION, which simply does not update
     * the flag state, so there is nothing correct for this function to record
     * and it says so instead of inventing a preserving case for four flags.
     */
    x86p_diagnostic_fatalf("flags", "x86p_flags_set: a shift by zero writes no flags; the caller must not record one");
  }
  /* Carried across, because the NEXT operation may be one that preserves them.
     Read before the write: `f` is both source and destination. */
  f->carry_in = (uint8_t)x86p_flag_cf(f);
  f->kind = (uint8_t)kind;
  f->a = a;
  f->b = b;
  f->r = r;
  f->w = (uint8_t)w;
}

void x86p_flags_set_explicit(X86pFlags *f, uint32_t eflags) {
  if (!f) {
    return;
  }
  f->kind = (uint8_t)kX86pFlagsExplicit;
  f->a = eflags;
  f->b = 0;
  f->r = 0;
  f->w = 4;
  /* An explicit word states CF and AF outright, so the preserved copies must
     agree with it -- otherwise a later INC would restore the carry the POPFD
     just overwrote. */
  f->carry_in = (uint8_t)((eflags & X86P_CF) != 0);
}

int x86p_flag_zf(const X86pFlags *f) {
  if (!f || f->kind == kX86pFlagsNone) {
    return 0;
  }
  if (f->kind == kX86pFlagsExplicit) {
    return (f->a & X86P_ZF) != 0;
  }
  return (f->w == 4) ? (f->r == 0) : ((f->r & x86p_width_mask(f->w)) == 0);
}

int x86p_flag_sf(const X86pFlags *f) {
  if (!f || f->kind == kX86pFlagsNone) {
    return 0;
  }
  if (f->kind == kX86pFlagsExplicit) {
    return (f->a & X86P_SF) != 0;
  }
  return (f->w == 4) ? (int)((f->r >> 31) & 1u) : msb(f->r, f->w);
}

int x86p_flag_pf(const X86pFlags *f) {
  uint32_t v;
  if (!f || f->kind == kX86pFlagsNone) {
    return 0;
  }
  if (f->kind == kX86pFlagsExplicit) {
    return (f->a & X86P_PF) != 0;
  }
  /* Parity of the LOW BYTE only, whatever the operand width. This is one of
     the details that reads like a bug and is architectural. */
  v = f->r & 0xFFu;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return (int)(~v & 1u);
}

int x86p_flag_cf(const X86pFlags *f) {
  if (__builtin_expect(!f, 0)) {
    return 0;
  }
  switch (f->kind) {
  case kX86pFlagsSub:
    if (__builtin_expect(f->w == 4, 1)) {
      return f->a < f->b;
    }
    return (f->a & x86p_width_mask(f->w)) < (f->b & x86p_width_mask(f->w));
  case kX86pFlagsLogic:
  case kX86pFlagsNone:
    return 0;
  case kX86pFlagsAdd:
    if (__builtin_expect(f->w == 4, 1)) {
      return f->r < f->a;
    }
    return (f->r & x86p_width_mask(f->w)) < (f->a & x86p_width_mask(f->w));
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* PRESERVED, not cleared. `inc` between an `add` and an `adc` does not
       clear the carry on hardware, and code really is written that way. */
    return f->carry_in != 0;
  case kX86pFlagsExplicit:
    return (f->a & X86P_CF) != 0;
  case kX86pFlagsShl:
  case kX86pFlagsShr:
  case kX86pFlagsSar: {
    /* Zero cannot arrive here: x86p_flags_set refuses to record a shift by
       zero, because such a shift writes no flags at all. */
    unsigned count = (unsigned)f->b;
    if (count > (unsigned)(f->w * 8)) {
      /*
       * Shifted entirely out, and the three directions do NOT agree here.
       * SHL and SHR have run out of operand bits, so the last bit out is 0.
       * SAR never runs out: it keeps feeding in the sign, so CF is the sign
       * bit however large the count. Measured -- returning 0 for all three
       * disagreed with hardware on 11,776 of 34,816 SAR cases, every one of
       * them a count past the width.
       */
      return (f->kind == kX86pFlagsSar) ? msb(f->a, f->w) : 0;
    }
    if (f->kind == kX86pFlagsShl) {
      return (int)((f->a >> ((unsigned)(f->w * 8) - count)) & 1u);
    }
    return (int)((f->a >> (count - 1)) & 1u);
  }
  default:
    return 0;
  }
}

int x86p_flag_of(const X86pFlags *f) {
  int sa, sb, sr;
  uint32_t m, sign_bit;
  if (!f || f->kind == kX86pFlagsNone) {
    return 0;
  }
  if (f->kind == kX86pFlagsExplicit) {
    return (f->a & X86P_OF) != 0;
  }
  if (f->kind == kX86pFlagsLogic || f->kind == kX86pFlagsSar) {
    return 0;
  }
  if (f->w != 1 && f->w != 2 && f->w != 4) {
    x86p_diagnostic_fatalf("flags", "x86p_flag_of: operand width %u is not 1, 2 or 4", (unsigned)f->w);
  }
  sa = (f->w == 4) ? (int)((f->a >> 31) & 1u) : msb(f->a, f->w);
  sb = (f->w == 4) ? (int)((f->b >> 31) & 1u) : msb(f->b, f->w);
  sr = (f->w == 4) ? (int)((f->r >> 31) & 1u) : msb(f->r, f->w);
  m = (f->w == 4) ? 0xFFFFFFFFu : x86p_width_mask(f->w);
  sign_bit = (uint32_t)1u << (f->w * 8 - 1);
  switch (f->kind) {
  case kX86pFlagsAdd:
    /* Two like signs that produced the other sign. */
    return (sa == sb) && (sr != sa);
  case kX86pFlagsSub:
    return (sa != sb) && (sr != sa);
  case kX86pFlagsInc:
    return (f->r & m) == sign_bit; /* wrapped to the most negative value */
  case kX86pFlagsDec:
    return (f->a & m) == sign_bit; /* was the most negative value */
  case kX86pFlagsShl:
    /* Architecturally defined only for a count of 1. Measured: this CPU
       computes msb(result) ^ CF for EVERY count -- 1792 of 1792 cases at
       counts 2..8 -- so one formula matches hardware everywhere. */
    return sr ^ x86p_flag_cf(f);
  case kX86pFlagsShr:
    /* At a count of 1, the bit that was shifted away from the top. Beyond
       that the ISA leaves OF undefined and this CPU returns 0 (measured:
       1792 of 1792), which is what an interpreter should reproduce -- not
       msb(a), which was the reading before hardware was consulted. */
    return (f->b == 1) ? sa : 0;
  default:
    return 0;
  }
}

int x86p_flag_af(const X86pFlags *f) {
  if (!f || f->kind == kX86pFlagsNone) {
    return 0;
  }
  if (f->kind == kX86pFlagsExplicit) {
    return (f->a & X86P_AF) != 0;
  }
  switch (f->kind) {
  case kX86pFlagsAdd:
  case kX86pFlagsSub:
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* The carry out of bit 3. One formula covers add and subtract because
       a ^ b ^ r isolates exactly the bits where a borrow or carry crossed. */
    return (int)(((f->a ^ f->b ^ f->r) >> 4) & 1u);
  case kX86pFlagsLogic:
    /*
     * Architecturally UNDEFINED, and "undefined" is not permission to guess:
     * real hardware puts something there, and PUSHFD can observe it. Measured
     * on this CPU across all 65,536 operand pairs for AND, OR, XOR and TEST:
     * AF is CLEARED, whether it went in set or clear. This model preserved it
     * until that measurement, and the first sweep could not catch the error
     * because it never varied the incoming AF.
     */
    return 0;
  case kX86pFlagsShl:
  case kX86pFlagsShr:
  case kX86pFlagsSar:
    /* Also undefined, and measured the other way: SET after every shift with a
       nonzero count -- 4096 of 4096 cases for each of SHL, SHR and SAR,
       regardless of the incoming AF. */
    return 1;
  default:
    return 0;
  }
}

uint32_t x86p_eflags(const X86pFlags *f) {
  if (!f || f->kind == kX86pFlagsNone) {
    return X86P_EFLAGS_FIXED;
  }
  if (f->kind == kX86pFlagsExplicit) {
    return (f->a & X86P_ARITH_FLAGS) | X86P_EFLAGS_FIXED;
  }
  uint32_t v = X86P_EFLAGS_FIXED;
  if (f->kind == kX86pFlagsLogic) {
    if (x86p_flag_zf(f)) {
      v |= X86P_ZF;
    }
    if (x86p_flag_sf(f)) {
      v |= X86P_SF;
    }
    if (x86p_flag_pf(f)) {
      v |= X86P_PF;
    }
    return v;
  }
  if (x86p_flag_cf(f)) {
    v |= X86P_CF;
  }
  if (x86p_flag_pf(f)) {
    v |= X86P_PF;
  }
  if (x86p_flag_af(f)) {
    v |= X86P_AF;
  }
  if (x86p_flag_zf(f)) {
    v |= X86P_ZF;
  }
  if (x86p_flag_sf(f)) {
    v |= X86P_SF;
  }
  if (x86p_flag_of(f)) {
    v |= X86P_OF;
  }
  return v;
}

uint32_t x86p_flags_adc(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t r, int w) {
  uint32_t m = x86p_width_mask(w), v = 0;
  uint64_t full;
  carry_in = carry_in ? 1u : 0u;
  full = (uint64_t)(a & m) + (uint64_t)(b & m) + (uint64_t)carry_in;
  a &= m;
  b &= m;
  r &= m;
  if ((full >> (w * 8)) & 1u) {
    v |= X86P_CF;
  }
  if (r == 0) {
    v |= X86P_ZF;
  }
  if (msb(r, w)) {
    v |= X86P_SF;
  }
  if (((~(a ^ b) & (a ^ r)) >> (w * 8 - 1)) & 1u) {
    v |= X86P_OF;
  }
  if (((a ^ b ^ r) >> 4) & 1u) {
    v |= X86P_AF;
  }
  {
    uint32_t p = r & 0xFFu;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    if (~p & 1u) {
      v |= X86P_PF;
    }
  }
  return v | X86P_EFLAGS_FIXED;
}

uint32_t x86p_flags_sbb(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t r, int w) {
  uint32_t m = x86p_width_mask(w), v = 0;
  uint64_t full;
  carry_in = carry_in ? 1u : 0u;
  full = (uint64_t)(a & m) - (uint64_t)(b & m) - (uint64_t)carry_in;
  a &= m;
  b &= m;
  r &= m;
  if ((full >> (w * 8)) & 1u) {
    v |= X86P_CF; /* borrow */
  }
  if (r == 0) {
    v |= X86P_ZF;
  }
  if (msb(r, w)) {
    v |= X86P_SF;
  }
  if ((((a ^ b) & (a ^ r)) >> (w * 8 - 1)) & 1u) {
    v |= X86P_OF;
  }
  if (((a ^ b ^ r) >> 4) & 1u) {
    v |= X86P_AF;
  }
  {
    uint32_t p = r & 0xFFu;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    if (~p & 1u) {
      v |= X86P_PF;
    }
  }
  return v | X86P_EFLAGS_FIXED;
}
