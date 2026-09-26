#ifndef X86PORT_JIT_ARM64_INTEGER_H
#define X86PORT_JIT_ARM64_INTEGER_H

#include "jit_arm64_internal.h"

/* Integer arithmetic lowering and its lazy-flag updates; memory safety is
 * supplied by the block owner's emit_mem_prepare_w contract. */
int inline_alu_shape(uint8_t alu, X86pA64Alu *host, X86pFlagKind *kind, int *writes_dest);

void emit_alu_inline(BlockCtx *c,
                     const X86pInsn *insn,
                     X86pA64Alu host,
                     X86pFlagKind kind,
                     int writes_dest,
                     int last_kind,
                     int flags_dead,
                     uint32_t insn_eip);

/* What emit_shift_inline says the flag state holds afterwards, besides a kind
   it recorded: a count known to be zero writes no flags, and a CL count is not
   known until the block runs. */
#define SHIFT_FLAGS_UNKNOWN (-1)
#define SHIFT_FLAGS_UNCHANGED (-2)

int emit_shift_inline(BlockCtx *c, const X86pInsn *insn, int flags_dead, uint32_t insn_eip);

void emit_cdq(X86pA64Emit *e);

void emit_div32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int signed_divide);

void emit_imul_to_register(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int flags_dead);

int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip);

#endif
