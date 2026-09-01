/* three_dnow.c -- see three_dnow.h for why this is the first thing x86port owns. */
#include "three_dnow.h"

#include <string.h>
#include <strings.h> /* strcasecmp -- x86p_3dnow_parse */

/* One table, in enum order, so the name lookup and the parser cannot disagree
   about a spelling and a new opcode cannot acquire a route without a name. */
static const char *const kNames[kX86pPfCount] = {"PFADD",   "PFSUB", "PFSUBR",  "PFMUL",    "PFACC",    "PFNACC",
                                                 "PFPNACC", "PFMAX", "PFMIN",   "PFCMPEQ",  "PFCMPGE",  "PFCMPGT",
                                                 "PI2FD",   "PF2ID", "PI2FW",   "PF2IW",    "PSWAPD",   "PAVGUSB",
                                                 "PMULHRW", "PFRCP", "PFRSQRT", "PFRCPIT1", "PFRCPIT2", "PFRSQIT1"};

const char *x86p_3dnow_name(X86pPfOp op) {
  if ((unsigned)op >= (unsigned)kX86pPfCount) {
    return "unknown";
  }
  return kNames[op];
}

/*
 * Spellings that are not the canonical mnemonic but name the same operation.
 *
 * MEASURED, not anticipated. Running x86port's decoder over pc/xmen2's
 * 2,168,629-instruction corpus surfaced Zydis 4.1 reporting PFRSQRT as
 * "PFSQRT" (60 sites) and PFRCPIT1 as "PFCPIT1" (66) -- each a letter short of
 * the real 3DNow! mnemonic. Accepting them costs nothing and closes a hazard
 * that would otherwise appear as two opcodes the engine mysteriously cannot
 * name. x86p_3dnow_name still returns only the canonical spelling: this is an
 * input convenience, not a second source of truth.
 */
static const struct {
  const char *alias;
  X86pPfOp op;
} kAliases[] = {
    {"PFSQRT", kX86pPfRsqrt},   /* zydis 4.1's spelling of PFRSQRT  */
    {"PFCPIT1", kX86pPfRcpIt1}, /* zydis 4.1's spelling of PFRCPIT1 */
};

int x86p_3dnow_parse(const char *mnemonic, X86pPfOp *out) {
  int i;
  if (!mnemonic || !*mnemonic || !out) {
    return 0;
  }
  for (i = 0; i < (int)kX86pPfCount; i++) {
    if (strcasecmp(mnemonic, kNames[i]) == 0) {
      *out = (X86pPfOp)i;
      return 1;
    }
  }
  for (i = 0; i < (int)(sizeof kAliases / sizeof kAliases[0]); i++) {
    if (strcasecmp(mnemonic, kAliases[i].alias) == 0) {
      *out = kAliases[i].op;
      return 1;
    }
  }
  return 0;
}

int x86p_3dnow_implemented(X86pPfOp op) {
  switch (op) {
  case kX86pPfRcp:
  case kX86pPfRsqrt:
  case kX86pPfRcpIt1:
  case kX86pPfRcpIt2:
  case kX86pPfRsqIt1:
  case kX86pPfCount:
    return 0;
  default:
    break;
  }
  return (unsigned)op < (unsigned)kX86pPfCount;
}

/*
 * Denormal-to-zero. 3DNow! has no denormal support: an input whose biased
 * exponent field is zero reads as a zero of the same sign. Done on the BIT
 * PATTERN rather than by comparing against FLT_MIN, because the comparison
 * itself would be a float operation on the denormal we are trying not to have.
 */
float x86p_pf_daz(float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  if ((bits & 0x7F800000u) == 0u) {
    bits &= 0x80000000u; /* keep the sign, clear mantissa: signed zero */
    memcpy(&v, &bits, sizeof v);
  }
  return v;
}

/* All-ones / all-zeros lane mask, which is what the PFCMP* family writes --
   they produce a mask, not a float. */
static uint32_t mask_of(int predicate) {
  return predicate ? 0xFFFFFFFFu : 0x00000000u;
}

/* float -> signed dword, truncating toward zero. Out-of-range and non-finite
   inputs are clamped rather than left to C's undefined conversion: an undefined
   conversion here is a host-dependent result, and a guest instruction whose
   answer depends on which host ran it is not a semantics definition. */
static int32_t f_to_i32_trunc(float v) {
  if (!(v == v)) { /* NaN */
    return 0;
  }
  if (v >= 2147483647.0f) {
    return 2147483647;
  }
  if (v <= -2147483648.0f) {
    return (-2147483647 - 1);
  }
  return (int32_t)v;
}

static int16_t sat_to_i16(int32_t v) {
  if (v > 32767) {
    return 32767;
  }
  if (v < -32768) {
    return -32768;
  }
  return (int16_t)v;
}

int x86p_3dnow_eval(X86pPfOp op, X86pMm a, X86pMm b, X86pMm *out) {
  X86pMm r;
  int lane;

  if (!out || !x86p_3dnow_implemented(op)) {
    return 0;
  }

  r.q = 0;

  /* The float-domain operations share one input flush, so the numeric model
     is applied once rather than restated per opcode. The integer-domain
     operations below take the raw operands and skip it. */
  switch (op) {
  case kX86pPfAdd:
  case kX86pPfSub:
  case kX86pPfSubR:
  case kX86pPfMul:
  case kX86pPfAcc:
  case kX86pPfNAcc:
  case kX86pPfPNAcc:
  case kX86pPfMax:
  case kX86pPfMin:
  case kX86pPfCmpEq:
  case kX86pPfCmpGe:
  case kX86pPfCmpGt:
  case kX86pPf2Id:
  case kX86pPf2Iw:
    for (lane = 0; lane < 2; lane++) {
      a.f[lane] = x86p_pf_daz(a.f[lane]);
      b.f[lane] = x86p_pf_daz(b.f[lane]);
    }
    break;
  default:
    break;
  }

  switch (op) {
  case kX86pPfAdd:
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = a.f[lane] + b.f[lane];
    }
    break;
  case kX86pPfSub:
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = a.f[lane] - b.f[lane];
    }
    break;
  case kX86pPfSubR: /* reversed: b - a. The whole reason the opcode exists is
                       that the destination is the subtrahend. */
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = b.f[lane] - a.f[lane];
    }
    break;
  case kX86pPfMul:
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = a.f[lane] * b.f[lane];
    }
    break;
  case kX86pPfAcc: /* horizontal, and it crosses operands: low lane from a,
                      high lane from b. */
    r.f[0] = a.f[0] + a.f[1];
    r.f[1] = b.f[0] + b.f[1];
    break;
  case kX86pPfNAcc:
    r.f[0] = a.f[0] - a.f[1];
    r.f[1] = b.f[0] - b.f[1];
    break;
  case kX86pPfPNAcc: /* positive-negative: subtract in the low lane, add in
                        the high one. */
    r.f[0] = a.f[0] - a.f[1];
    r.f[1] = b.f[0] + b.f[1];
    break;
  case kX86pPfMax:
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = a.f[lane] > b.f[lane] ? a.f[lane] : b.f[lane];
    }
    break;
  case kX86pPfMin:
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = a.f[lane] < b.f[lane] ? a.f[lane] : b.f[lane];
    }
    break;
  case kX86pPfCmpEq:
    for (lane = 0; lane < 2; lane++) {
      r.u[lane] = mask_of(a.f[lane] == b.f[lane]);
    }
    break;
  case kX86pPfCmpGe:
    for (lane = 0; lane < 2; lane++) {
      r.u[lane] = mask_of(a.f[lane] >= b.f[lane]);
    }
    break;
  case kX86pPfCmpGt:
    for (lane = 0; lane < 2; lane++) {
      r.u[lane] = mask_of(a.f[lane] > b.f[lane]);
    }
    break;
  case kX86pPi2Fd: /* source is b, the operand being converted */
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = (float)b.i[lane];
    }
    break;
  case kX86pPf2Id:
    for (lane = 0; lane < 2; lane++) {
      r.i[lane] = f_to_i32_trunc(b.f[lane]);
    }
    break;
  case kX86pPi2Fw: /* the LOW word of each dword is the signed source */
    for (lane = 0; lane < 2; lane++) {
      r.f[lane] = (float)(int16_t)(uint16_t)(b.u[lane] & 0xFFFFu);
    }
    break;
  case kX86pPf2Iw: /* saturate to a word, then sign-extend back to the dword */
    for (lane = 0; lane < 2; lane++) {
      r.i[lane] = sat_to_i16(f_to_i32_trunc(b.f[lane]));
    }
    break;
  case kX86pPSwapD:
    r.u[0] = b.u[1];
    r.u[1] = b.u[0];
    break;
  case kX86pPAvgUsb: { /* eight unsigned bytes, average rounded UP */
    int i;
    for (i = 0; i < 8; i++) {
      unsigned x = (unsigned)((a.q >> (i * 8)) & 0xFFu);
      unsigned y = (unsigned)((b.q >> (i * 8)) & 0xFFu);
      uint64_t avg = (x + y + 1u) >> 1;
      r.q |= avg << (i * 8);
    }
    break;
  }
  case kX86pPMulHrw: { /* four signed words: (a*b + 0x8000) >> 16 */
    int i;
    for (i = 0; i < 4; i++) {
      int32_t x = (int16_t)(uint16_t)((a.q >> (i * 16)) & 0xFFFFu);
      int32_t y = (int16_t)(uint16_t)((b.q >> (i * 16)) & 0xFFFFu);
      uint16_t hi = (uint16_t)(((x * y) + 0x8000) >> 16);
      r.q |= (uint64_t)hi << (i * 16);
    }
    break;
  }
  default:
    /* Unreachable: x86p_3dnow_implemented() already rejected everything
       this switch does not name. Refuse rather than fall through to a
       zeroed result, which would be an answer. */
    return 0;
  }

  *out = r;
  return 1;
}
