/* alu.c -- see alu.h for why result and flags are one call. */
#include "alu.h"
#include "diagnostic.h"

uint32_t x86p_sign_extend(uint32_t value, int from_bytes) {
  switch (from_bytes) {
  case 1:
    return (uint32_t)(int32_t)(int8_t)(value & 0xFFu);
  case 2:
    return (uint32_t)(int32_t)(int16_t)(value & 0xFFFFu);
  default:
    return value;
  }
}

static const char *kOpNames[] = {
    "ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP", "TEST", "SHL", "SHR", "SAR", "ROL", "ROR", "RCL", "RCR"};
_Static_assert((int)(sizeof kOpNames / sizeof kOpNames[0]) == (int)kX86pAluOpCount, "every X86pAluOp needs a name");

static const char *kUnNames[] = {"NOT", "NEG", "INC", "DEC"};
_Static_assert((int)(sizeof kUnNames / sizeof kUnNames[0]) == (int)kX86pAluUnOpCount, "every X86pAluUnOp needs a name");

const char *x86p_alu_name(X86pAluOp op) {
  if ((unsigned)op >= (unsigned)kX86pAluOpCount) {
    return "unknown";
  }
  return kOpNames[(int)op];
}

const char *x86p_alu_unary_name(X86pAluUnOp op) {
  if ((unsigned)op >= (unsigned)kX86pAluUnOpCount) {
    return "unknown";
  }
  return kUnNames[(int)op];
}

static void require_width(int w, const char *who) {
  if (w != 1 && w != 2 && w != 4) {
    x86p_diagnostic_fatalf("alu", "%s: operand width %d is not 1, 2 or 4", who, w);
  }
}

static uint32_t bits(int w) {
  return (uint32_t)(w * 8);
}

/*
 * The rotates write ONLY CF and OF; every other flag keeps whatever it had.
 * The lazy model has no kind for "two flags changed and four did not", and
 * inventing one would put a second flag representation in the file whose whole
 * job is to have one. So the current flags are materialised, the two bits are
 * replaced, and the word is stored explicitly -- which is exactly what the
 * model's Explicit kind exists for.
 */
static void set_cf_of_only(X86pFlags *f, int cf, int of) {
  uint32_t word = x86p_eflags(f);
  word = (word & ~(X86P_CF | X86P_OF)) | (cf ? X86P_CF : 0u) | (of ? X86P_OF : 0u);
  x86p_flags_set_explicit(f, word);
}

uint32_t x86p_alu(X86pAluOp op, uint32_t a, uint32_t b, int w, X86pFlags *f) {
  uint32_t m, r, count, nbits;
  require_width(w, "x86p_alu");
  if (!f) {
    x86p_diagnostic_fatalf("alu", "x86p_alu: no flag state; every operation here writes flags");
  }
  m = x86p_width_mask(w);
  nbits = bits(w);
  a &= m;
  b &= m;

  switch (op) {
  case kX86pAluAdd:
    r = (a + b) & m;
    x86p_flags_set(f, kX86pFlagsAdd, a, b, r, w);
    return r;
  case kX86pAluSub:
  case kX86pAluCmp:
    r = (a - b) & m;
    x86p_flags_set(f, kX86pFlagsSub, a, b, r, w);
    return r;
  case kX86pAluOr:
    r = (a | b) & m;
    x86p_flags_set(f, kX86pFlagsLogic, a, b, r, w);
    return r;
  case kX86pAluAnd:
  case kX86pAluTest:
    r = (a & b) & m;
    x86p_flags_set(f, kX86pFlagsLogic, a, b, r, w);
    return r;
  case kX86pAluXor:
    r = (a ^ b) & m;
    x86p_flags_set(f, kX86pFlagsLogic, a, b, r, w);
    return r;
  case kX86pAluAdc: {
    /* Eager, because the lazy triple cannot carry a carry-in -- flags.h says
       what that cost when it was modelled as a plain SUB/ADD of an adjusted
       operand. The carry is read BEFORE the flags are overwritten. */
    uint32_t cin = (uint32_t)(x86p_flag_cf(f) ? 1 : 0);
    r = (a + b + cin) & m;
    x86p_flags_set_explicit(f, x86p_flags_adc(a, b, cin, r, w));
    return r;
  }
  case kX86pAluSbb: {
    uint32_t cin = (uint32_t)(x86p_flag_cf(f) ? 1 : 0);
    r = (a - b - cin) & m;
    x86p_flags_set_explicit(f, x86p_flags_sbb(a, b, cin, r, w));
    return r;
  }
  case kX86pAluShl:
  case kX86pAluShr:
  case kX86pAluSar:
    /* Masked to 5 bits here, ONCE. It is architectural (386 and later), and a
       caller that forgets produces a shift by 33, which C leaves undefined. */
    count = b & 0x1Fu;
    if (count == 0) {
      /* No flags are written AT ALL -- not preserved-by-derivation, not
         written. flags.c refuses to record such an update, so this returns
         before touching `f`. */
      return a;
    }
    if (op == kX86pAluShl) {
      r = (count >= nbits) ? 0u : ((a << count) & m);
      x86p_flags_set(f, kX86pFlagsShl, a, count, r, w);
    } else if (op == kX86pAluShr) {
      r = (count >= nbits) ? 0u : ((a >> count) & m);
      x86p_flags_set(f, kX86pFlagsShr, a, count, r, w);
    } else {
      /* Arithmetic: replicate the sign bit. Done by hand rather than with a
         signed >>, whose behaviour on negative values C leaves
         implementation-defined. */
      uint32_t sign = (a >> (nbits - 1)) & 1u;
      if (count >= nbits) {
        r = sign ? m : 0u;
      } else {
        r = (a >> count) & m;
        if (sign) {
          r |= (m << (nbits - count)) & m;
        }
      }
      x86p_flags_set(f, kX86pFlagsSar, a, count, r, w);
    }
    return r;
  case kX86pAluRol:
  case kX86pAluRor:
    count = b & 0x1Fu;
    if (count == 0) {
      return a; /* nothing happens, flags included */
    }
    count %= nbits;
    if (count == 0) {
      /* A count that is a nonzero MULTIPLE of the width leaves the value alone
         but STILL writes CF, from the bit that ended up in position. This is
         the case a naive `if (count == 0) return` after the modulo silently
         gets wrong. */
      r = a;
    } else if (op == kX86pAluRol) {
      r = ((a << count) | (a >> (nbits - count))) & m;
    } else {
      r = ((a >> count) | (a << (nbits - count))) & m;
    }
    {
      int cf = (op == kX86pAluRol) ? (int)(r & 1u) : (int)((r >> (nbits - 1)) & 1u);
      /* OF is defined only for a count of 1: the sign change the rotate caused. */
      int of = (int)((r >> (nbits - 1)) & 1u) ^ ((op == kX86pAluRol) ? cf : (int)((r >> (nbits - 2)) & 1u));
      set_cf_of_only(f, cf, of);
    }
    return r;
  case kX86pAluRcl:
  case kX86pAluRcr: {
    /* Rotating through carry is a rotate of a quantity one bit WIDER than the
       operand, which is why it cannot be expressed as a pair of shifts. The
       count is reduced modulo width+1, not width. */
    uint32_t i, cf = (uint32_t)(x86p_flag_cf(f) ? 1 : 0);
    count = b & 0x1Fu;
    if (count == 0) {
      return a;
    }
    count %= (nbits + 1u);
    r = a;
    for (i = 0; i < count; i++) {
      if (op == kX86pAluRcl) {
        uint32_t out = (r >> (nbits - 1)) & 1u;
        r = ((r << 1) | cf) & m;
        cf = out;
      } else {
        uint32_t out = r & 1u;
        r = ((r >> 1) | (cf << (nbits - 1))) & m;
        cf = out;
      }
    }
    {
      int of = (op == kX86pAluRcl) ? (int)(((r >> (nbits - 1)) & 1u) ^ cf)
                                   : (int)(((r >> (nbits - 1)) & 1u) ^ ((r >> (nbits - 2)) & 1u));
      set_cf_of_only(f, (int)cf, of);
    }
    return r;
  }
  case kX86pAluOpCount:
    break;
  }
  x86p_diagnostic_fatalf(
      "alu", "x86p_alu: operation %u is not one of the %u this build knows", (unsigned)op, (unsigned)kX86pAluOpCount);
}

uint32_t x86p_alu_unary(X86pAluUnOp op, uint32_t a, int w, X86pFlags *f) {
  uint32_t m, r;
  require_width(w, "x86p_alu_unary");
  if (!f) {
    x86p_diagnostic_fatalf("alu", "x86p_alu_unary: no flag state");
  }
  m = x86p_width_mask(w);
  a &= m;
  switch (op) {
  case kX86pAluNot:
    /* NOT writes NO flags. That is why it is here and not expressed as
       `XOR a, -1`, which would clear CF and OF. */
    return (~a) & m;
  case kX86pAluNeg:
    /* 0 - a, with SUB's flags. CF ends up set exactly when a was nonzero,
       which falls out of the borrow rather than being special-cased. */
    r = (0u - a) & m;
    x86p_flags_set(f, kX86pFlagsSub, 0u, a, r, w);
    return r;
  case kX86pAluInc:
    r = (a + 1u) & m;
    x86p_flags_set(f, kX86pFlagsInc, a, 1u, r, w);
    return r;
  case kX86pAluDec:
    r = (a - 1u) & m;
    x86p_flags_set(f, kX86pFlagsDec, a, 1u, r, w);
    return r;
  case kX86pAluUnOpCount:
    break;
  }
  x86p_diagnostic_fatalf("alu",
                         "x86p_alu_unary: operation %u is not one of the %u this build knows",
                         (unsigned)op,
                         (unsigned)kX86pAluUnOpCount);
}

/*
 * A multiply writes CF and OF and NOTHING ELSE.
 *
 * ZF, SF, PF and AF are architecturally undefined here. The deterministic
 * policy preserves them, matching the original reference host. Hardware-oracle
 * tests report variation in those bits across CPUs without promoting it to an
 * architectural correctness failure.
 *
 * Which makes a multiply the same SHAPE as a rotate: two flags written, four
 * untouched. It reuses that helper rather than growing a second way to say so.
 */
static void mul_flags(X86pFlags *f, int overflow) {
  set_cf_of_only(f, overflow, overflow);
}

void x86p_alu_mul(uint32_t a, uint32_t b, int w, uint32_t *lo, uint32_t *hi, X86pFlags *f) {
  uint64_t full;
  uint32_t m;
  require_width(w, "x86p_alu_mul");
  if (!lo || !hi || !f) {
    x86p_diagnostic_fatalf("alu", "x86p_alu_mul: lo, hi and flags are all required outputs");
  }
  m = x86p_width_mask(w);
  full = (uint64_t)(a & m) * (uint64_t)(b & m);
  /* BOTH halves are written on every call, so nothing can read a stale upper
     half after a multiply that "did not need" it. */
  *lo = (uint32_t)(full & m);
  *hi = (uint32_t)((full >> bits(w)) & m);
  mul_flags(f, *hi != 0);
}

void x86p_alu_imul(uint32_t a, uint32_t b, int w, uint32_t *lo, uint32_t *hi, X86pFlags *f) {
  int64_t full;
  uint32_t m, nb;
  require_width(w, "x86p_alu_imul");
  if (!lo || !hi || !f) {
    x86p_diagnostic_fatalf("alu", "x86p_alu_imul: lo, hi and flags are all required outputs");
  }
  m = x86p_width_mask(w);
  nb = bits(w);
  /* Sign-extend both operands from the operand width to 64 bits before
     multiplying. Widening first and signing later gets every negative operand
     wrong. */
  {
    int64_t sa = (int64_t)(int32_t)((a & m) << (32 - nb)) >> (32 - nb);
    int64_t sb = (int64_t)(int32_t)((b & m) << (32 - nb)) >> (32 - nb);
    full = sa * sb;
  }
  *lo = (uint32_t)((uint64_t)full & m);
  *hi = (uint32_t)(((uint64_t)full >> nb) & m);
  /* CF and OF say the result did not fit in the LOW half as a signed value --
     which is not the same test as the unsigned one, so it is written out. */
  {
    int64_t low_as_signed = (int64_t)(int32_t)((*lo & m) << (32 - nb)) >> (32 - nb);
    mul_flags(f, low_as_signed != full);
  }
}

int x86p_alu_div(uint32_t hi, uint32_t lo, uint32_t d, int w, uint32_t *quot, uint32_t *rem, X86pFlags *f) {
  uint32_t m;
  uint64_t num, q;
  require_width(w, "x86p_alu_div");
  if (!quot || !rem || !f) {
    x86p_diagnostic_fatalf("alu", "x86p_alu_div: quot, rem and flags are all required outputs");
  }
  m = x86p_width_mask(w);
  d &= m;
  if (d == 0) {
    return 0; /* #DE, and nothing written */
  }
  num = ((uint64_t)(hi & m) << bits(w)) | (uint64_t)(lo & m);
  q = num / d;
  if (q > (uint64_t)m) {
    /* Quotient overflow is the SAME guest exception as divide-by-zero, and it
       is the one people forget: `hi` larger than the divisor always produces
       it, and returning a truncated quotient instead would be silently wrong
       arithmetic rather than a fault. */
    return 0;
  }
  *quot = (uint32_t)q;
  *rem = (uint32_t)(num % d);
  /* Every flag is architecturally undefined after a divide. Left untouched:
     inventing values would make a divergence report blame the divide for
     flags the guest had already set. */
  (void)f;
  return 1;
}

int x86p_alu_idiv(uint32_t hi, uint32_t lo, uint32_t d, int w, uint32_t *quot, uint32_t *rem, X86pFlags *f) {
  uint32_t m, nb;
  int64_t num, sd, q, r;
  int64_t qmin, qmax;
  require_width(w, "x86p_alu_idiv");
  if (!quot || !rem || !f) {
    x86p_diagnostic_fatalf("alu", "x86p_alu_idiv: quot, rem and flags are all required outputs");
  }
  m = x86p_width_mask(w);
  nb = bits(w);
  sd = (int64_t)(int32_t)((d & m) << (32 - nb)) >> (32 - nb);
  if (sd == 0) {
    return 0;
  }
  /* The dividend is 2*w bytes wide and SIGNED, so it is sign-extended from
     2*nb bits, not from nb. */
  num = (int64_t)(((uint64_t)(hi & m) << nb) | (uint64_t)(lo & m));
  if (nb * 2 < 64) {
    int64_t sign_bit = (int64_t)1 << (nb * 2 - 1);
    if (num & sign_bit) {
      num -= (int64_t)1 << (nb * 2);
    }
  }
  /* C cannot represent INT64_MIN / -1 and leaves it undefined, while the
     guest architecture requires a precise #DE. Refuse before host division. */
  if (num == INT64_MIN && sd == -1) {
    return 0;
  }
  q = num / sd; /* C truncates toward zero, which is what IDIV does */
  r = num % sd;
  qmin = -((int64_t)1 << (nb - 1));
  qmax = ((int64_t)1 << (nb - 1)) - 1;
  if (q < qmin || q > qmax) {
    return 0; /* #DE: includes INT_MIN / -1 */
  }
  *quot = (uint32_t)((uint64_t)q & m);
  *rem = (uint32_t)((uint64_t)r & m);
  (void)f; /* undefined, and left alone -- see x86p_alu_div */
  return 1;
}
