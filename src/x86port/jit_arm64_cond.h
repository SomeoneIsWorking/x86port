#ifndef X86PORT_JIT_ARM64_COND_H
#define X86PORT_JIT_ARM64_COND_H

#include "emit_arm64.h"

#include <stdint.h>

/*
 * Materialise x86 condition `cond` as 0 or 1 in X0 using the host's own
 * condition codes, given the flag kind and operand width that the block knows
 * wrote the flags (`last_kind` is an X86pFlagKind, or -1 when the producer is
 * outside the block; `last_w` is 1, 2 or 4, or -1).
 *
 * Returns 1 having emitted the evaluation, or 0 having emitted NOTHING, in
 * which case the caller must fall back to calling x86p_cond. Refusing is
 * always safe and is the answer for every kind, width and condition whose
 * derivation is not exactly a host condition code.
 */
int x86p_a64_emit_condition_inline(X86pA64Emit *e, uint8_t cond, int last_kind, int last_w);

#endif /* X86PORT_JIT_ARM64_COND_H */
