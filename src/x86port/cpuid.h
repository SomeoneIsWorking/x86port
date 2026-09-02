/*
 * cpuid.h -- the processor this framework claims to be, and the timestamp
 * counter.
 *
 * CPUID IS A CONTRACT, NOT A LOOKUP. What it reports decides which code paths
 * the guest takes for the rest of the run: a game that is told SSE2 exists will
 * call the SSE2 path forever after, and if this framework does not actually
 * implement one of those instructions the failure surfaces thousands of
 * instructions later, in a routine that never mentions CPUID. So the feature
 * bits here are not copied from the host and not set to "everything" -- they
 * are exactly the features this build implements, and the file that owns the
 * implementations is named beside each one.
 *
 * REPORTING THE HOST'S OWN CPUID WOULD BE WORSE THAN USELESS. It advertises
 * AVX-512 on this machine and nothing here decodes a single VEX prefix; a guest
 * would take the widest path available and meet a refusal immediately. The
 * identity is ours to state, and stating it conservatively is what makes the
 * guest's choices land inside what is implemented.
 */
#ifndef X86PORT_CPUID_H
#define X86PORT_CPUID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct X86pCpuidResult {
  uint32_t eax, ebx, ecx, edx;
} X86pCpuidResult;

/*
 * The leaf `leaf` (EAX) with subleaf `subleaf` (ECX).
 *
 * Every unknown leaf returns zeros rather than whatever the host would say,
 * which is what a real CPU does for a leaf above its maximum and what keeps a
 * guest from discovering a feature by accident.
 */
void x86p_cpuid(uint32_t leaf, uint32_t subleaf, X86pCpuidResult *out);

/*
 * The timestamp counter, as EDX:EAX.
 *
 * MONOTONIC AND OURS. Passing the host's real TSC through would tie guest
 * timing to the host's clock speed and make a run non-reproducible, which
 * defeats the differential testing this whole framework rests on -- two runs of
 * the same block would legitimately disagree. This counter advances by a fixed
 * amount per read, so a guest that measures elapsed time sees time pass and two
 * runs see the same thing.
 */
uint64_t x86p_rdtsc_next(uint64_t *counter);

#ifdef __cplusplus
}
#endif

#endif
