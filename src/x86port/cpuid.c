/* cpuid.c -- see cpuid.h for why these bits are chosen rather than copied. */
#include "cpuid.h"

#include <string.h>

/*
 * Leaf 1, EDX. Each bit is set only because something in this repository
 * implements the WHOLE feature it advertises; the owning file is named so that
 * removing an implementation and leaving the bit set is a visible
 * inconsistency. Writing this list is what surfaced that the first draft
 * claimed three features this build does not have.
 */
#define F_FPU (1u << 0)   /* x87.c, x87_exec.c, x87_transcendental.c */
#define F_TSC (1u << 4)   /* RDTSC, below */
#define F_CMOV (1u << 15) /* cond.c, and the CMOVcc emitter in jit_x64.c */
#define F_MMX (1u << 23)  /* simd_int.c, aliased onto the x87 file by x87.c */
#define F_SSE (1u << 25)  /* simd_float.c: the single-precision set, complete */

/*
 * WHAT IS DELIBERATELY NOT CLAIMED, and why each one would be a lie that
 * surfaced far from here:
 *
 *  - SSE2 (bit 26). simd_float.c implements no double-precision operation at
 *    all -- no ADDPD, no CVTSD2SI. Several SSE2 *integer* operations are
 *    implemented (PADDQ, PSHUFD, the 128-bit forms), but the bit gates the
 *    whole set, and a guest told SSE2 exists would reach the double-precision
 *    half and be refused thousands of instructions after the CPUID that let it.
 *  - CX8 (bit 8). CMPXCHG8B is not implemented. It is what lock-free guest
 *    code compiles to, so claiming it would fail exactly where failure is
 *    least debuggable.
 *  - FXSR (bit 24). FXSAVE and FXRSTOR are not implemented. On real hardware
 *    the SSE bit implies this one, and the gap is tolerable for the specific
 *    reason that the facility exists for saving state ACROSS CONTEXT SWITCHES:
 *    the operating system is emulated, the guest never performs its own, and
 *    nothing in a game's own code calls it. That is an argument about this
 *    framework's shape, not a general one, so it is written down rather than
 *    assumed.
 *  - MSR (bit 5). RDMSR and WRMSR are privileged; a ring-3 guest could not use
 *    them if they existed.
 */
#define F_ECX 0u /* SSE3 and later: none of it is implemented */

/*
 * Extended leaf 0x80000001, EDX: 3DNow! and MMX-ext, ALSO NOT CLAIMED, and
 * this one is worth more than honesty.
 *
 * three_dnow.c implements the arithmetic but REFUSES the reciprocal family --
 * PFRCP, PFRSQRT, PFRCPIT1, PFRCPIT2, PFRSQIT1 -- because those come from
 * hardware tables to about twelve mantissa bits and cannot be stated from a
 * specification. Approximating them is the one thing this repository will not
 * do (see "approximate is not a synonym for implemented").
 *
 * A guest picks its 3DNow! path by ASKING. X-Men Legends II's binary contains
 * 447 of those reciprocal instructions, and every one of them sits behind a
 * runtime feature check. Declining the bit means the guest takes its generic
 * path and never executes one -- so the largest remaining block of refusals in
 * the corpus becomes unreachable code rather than a wall. Claiming the feature
 * and then refusing five of its instructions would be strictly worse than
 * saying no.
 */
#define F_EXT 0u

/* "x86port CPU " as the twelve-byte vendor string, in the EBX:EDX:ECX order
   CPUID reports it. Honest: a guest that keys off "GenuineIntel" is asking
   whether it may use undocumented Intel behaviour, and the answer is no. */
static void vendor(X86pCpuidResult *out) {
  static const char kVendor[12] = {'x', '8', '6', 'p', 'o', 'r', 't', ' ', 'C', 'P', 'U', ' '};
  memcpy(&out->ebx, kVendor + 0, 4);
  memcpy(&out->edx, kVendor + 4, 4);
  memcpy(&out->ecx, kVendor + 8, 4);
}

static void brand(X86pCpuidResult *out, unsigned index) {
  static const char kBrand[48] = "x86port dynamic recompiler";
  memcpy(&out->eax, kBrand + index * 16u + 0u, 4);
  memcpy(&out->ebx, kBrand + index * 16u + 4u, 4);
  memcpy(&out->ecx, kBrand + index * 16u + 8u, 4);
  memcpy(&out->edx, kBrand + index * 16u + 12u, 4);
}

void x86p_cpuid(uint32_t leaf, uint32_t subleaf, X86pCpuidResult *out) {
  (void)subleaf; /* no implemented leaf is subleaf-dependent */
  memset(out, 0, sizeof *out);

  switch (leaf) {
  case 0u:
    out->eax = 1u; /* the highest basic leaf that reports anything */
    vendor(out);
    return;
  case 1u:
    /* Family 6, model 8, stepping 1 -- a plain 686. The value matters because
       guest code branches on family for instruction availability, and a
       family low enough to predate what is not implemented here is the safe
       claim. */
    out->eax = 0x00000681u;
    out->ebx = 0x00000000u;
    out->ecx = F_ECX;
    out->edx = F_FPU | F_TSC | F_CMOV | F_MMX | F_SSE;
    return;
  case 0x80000000u:
    out->eax = 0x80000004u; /* the brand string leaves, and no further */
    return;
  case 0x80000001u:
    out->edx = F_EXT;
    return;
  case 0x80000002u:
  case 0x80000003u:
  case 0x80000004u:
    brand(out, (unsigned)(leaf - 0x80000002u));
    return;
  default:
    /* Zeros, which is what a real CPU returns above its maximum leaf. Not the
       host's answer: a guest must not discover a feature by accident. */
    return;
  }
}

uint64_t x86p_rdtsc_next(uint64_t *counter) {
  /*
   * A fixed step per read. Large enough that a guest measuring a short
   * interval sees a non-zero difference, and constant so two runs of the same
   * code agree -- which the differential testing this framework rests on
   * requires, and which the host's own TSC could never provide.
   */
  *counter += 1000u;
  return *counter;
}
