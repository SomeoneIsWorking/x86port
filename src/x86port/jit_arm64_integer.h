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

void emit_cdq(X86pA64Emit *e);

void emit_div32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int signed_divide);

void emit_imul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);

int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip);

#endif
