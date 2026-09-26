#ifndef X86PORT_JIT_ARM64_COND_H
#define X86PORT_JIT_ARM64_COND_H

#include "emit_arm64.h"

#include <stdint.h>

/*
 * Where the flag writer immediately before a condition left the tuple it just
 * recorded: the host W register holding each of a, b and r, or
 * X86P_A64_FLAG_IN_MEMORY for a field that is only in the CPU state. Reading
 * the registers spares the condition reloading the stores it follows.
 */
#define X86P_A64_FLAG_IN_MEMORY (-1)
typedef struct X86pA64FlagRegs {
  int a;
  int b;
  int r;
} X86pA64FlagRegs;

static inline X86pA64FlagRegs x86p_a64_flags_in_memory(void) {
  const X86pA64FlagRegs none = {X86P_A64_FLAG_IN_MEMORY, X86P_A64_FLAG_IN_MEMORY, X86P_A64_FLAG_IN_MEMORY};
  return none;
}

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
 * only to test it again. `regs` says which tuple fields are still in host
 * registers (NULL: none are).
 */
int x86p_a64_emit_condition_flags(X86pA64Emit *e,
                                  uint8_t cond,
                                  int last_kind,
                                  int last_w,
                                  const X86pA64FlagRegs *regs,
                                  X86pA64Cond *cc,
                                  int *constant);

#endif /* X86PORT_JIT_ARM64_COND_H */
