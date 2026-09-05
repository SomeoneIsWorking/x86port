/*
 * jit_arm64_x87.h -- native x87 emission for the AArch64 JIT backend.
 *
 * The AArch64 counterpart of jit_x64_x87.h: same predicates, same dispatch
 * contract, mirrored to jit_arm64_internal.h's BlockCtx and emit_arm64.h.
 */
#ifndef X86PORT_JIT_ARM64_X87_H
#define X86PORT_JIT_ARM64_X87_H

#include "decode.h"
#include "jit_arm64_internal.h"

#include "jit_x87_predicates.h"

/* Emit one X87 instruction. The caller has already checked the matching
   predicate. None of these write EFLAGS. */
void emit_x87_load(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);
void emit_x87_arith(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);
void emit_x87_compare_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);
void emit_x87_store_reg(BlockCtx *c, const X86pInsn *insn);
void emit_x87_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);
void emit_x87_constant(BlockCtx *c, const X86pInsn *insn);
void emit_x87_status_ax(BlockCtx *c);
void emit_x87_clear_exceptions(BlockCtx *c);

int x87_control_is_emittable(const X86pInsn *insn);
void emit_x87_control(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);

int x87_fn_is_emittable(const X86pInsn *insn);
void emit_x87_fn(BlockCtx *c, const X86pInsn *insn);

#endif /* X86PORT_JIT_ARM64_X87_H */
