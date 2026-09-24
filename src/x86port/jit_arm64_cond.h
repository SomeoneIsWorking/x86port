#ifndef X86PORT_JIT_ARM64_COND_H
#define X86PORT_JIT_ARM64_COND_H

#include "emit_arm64.h"

#include <stdint.h>

/*
 * Leave x86 condition `cond` in the host's own NZCV, given the flag kind and
 * operand width that the block knows wrote the flags (`last_kind` is an
 * X86pFlagKind, or -1 when the producer is outside the block; `last_w` is 1, 2
 * or 4, or -1).
 *
 * Returns 1 having emitted the comparison and set `*cc` to the host condition
 * to read it with -- or, when the condition is a compile-time constant for
 * that kind, set `*constant` to 0 or 1 and emitted nothing. Returns 0 having
 * emitted NOTHING, in which case the caller must fall back to calling
 * x86p_cond. Refusing is always safe and is the answer for every kind, width
 * and condition whose derivation is not exactly a host condition code.
 *
 * The caller chooses what to do with the condition -- a `cset` for SETcc, a
 * `b.cc` between a Jcc's two exits -- so a branch never materialises a 0/1
 * only to test it again.
 */
int x86p_a64_emit_condition_flags(
    X86pA64Emit *e, uint8_t cond, int last_kind, int last_w, X86pA64Cond *cc, int *constant);

#endif /* X86PORT_JIT_ARM64_COND_H */
