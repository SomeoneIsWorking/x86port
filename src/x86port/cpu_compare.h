/*
 * cpu_compare.h -- "do these two machines agree on everything architectural?"
 *
 * The one place that question is answered. A differential test compares the
 * interpreter against the JIT after every block; the JIT engine's verify mode
 * compares them after every block AT RUN TIME against a real game. Both need
 * the same predicate, and a second copy of it is a second thing that can be
 * wrong about which fields matter -- the padding bytes of a long double, the
 * lazy-flag tuple versus its six derived bits, ST(i) versus physical register i.
 *
 * Every field both engines are supposed to agree on is visited: the eight GPRs,
 * EIP, the six segment selectors and the two segment bases, DF, the eight XMM
 * registers, MXCSR, the lazy-flag tuple AND each of the six flags it derives
 * (redundant only if the derivation is right -- comparing both fails a backend
 * that stored a plausible tuple with the wrong kind, and names the flag), and
 * the x87 stack compared by ST position with its control and status words.
 */
#ifndef X86PORT_CPU_COMPARE_H
#define X86PORT_CPU_COMPARE_H

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called once per diverging field. `field` names it ("EAX", "ZF", "ST(2)",
 * "lazy flags", ...); `a_text` and `b_text` are that field formatted for the
 * two machines, in the order they were passed to x86p_cpu_diff. All three
 * strings are valid only for the duration of the call.
 */
typedef void (*X86pCpuDiffFn)(const char *field, const char *a_text, const char *b_text, void *user);

/*
 * Visit every architectural difference between `a` and `b`. Returns the number
 * of differing fields; 0 means the two machines are architecturally identical.
 * `on_diff` may be null to only count.
 */
unsigned x86p_cpu_diff(const X86pCpu *a, const X86pCpu *b, X86pCpuDiffFn on_diff, void *user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_CPU_COMPARE_H */
