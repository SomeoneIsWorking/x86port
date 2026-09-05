/*
 * jit_x64_x87.h -- native x87 emission for the x86-64 JIT backend.
 *
 * x87 is ~38% of in-game guest CPU on pc/xmen2, and it is the single largest
 * broad-codegen lever left. This unit emits the translatable subset -- FLD, the
 * FADD/FSUB/FMUL/FDIV family, FCOM/FCOMP against m32/m64, and FST/FSTP (to a
 * register or to m32/m64) -- with inline operand widening and direct calls to
 * the shared x86p_x87_* semantic helpers, so the status word, tags and TOP stay
 * that module's business and the test oracle can check operand plumbing
 * independently. Integer-source forms (FILD/FIST/FICOM...), FLD m80,
 * register/implicit compares, FLDCW and memory-form FNSTSW are refused until
 * they have native emitters. FNSTSW AX and FNCLEX are emitted through their
 * canonical status owners.
 */
#ifndef X86PORT_JIT_X64_X87_H
#define X86PORT_JIT_X64_X87_H

#include "decode.h"
#include "jit_x64_internal.h"

/* Can this X87 instruction be emitted natively? Exactly one predicate is true
   for an emittable instruction; the dispatch order in jit_x64.c mirrors this. */
int x87_load_is_emittable(const X86pInsn *insn);
int x87_arith_is_emittable(const X86pInsn *insn);
int x87_compare_mem_is_emittable(const X86pInsn *insn);
int x87_store_reg_is_emittable(const X86pInsn *insn);
int x87_store_mem_is_emittable(const X86pInsn *insn);
int x87_constant_is_emittable(const X86pInsn *insn);
int x87_status_ax_is_emittable(const X86pInsn *insn);
int x87_clear_exceptions_is_emittable(const X86pInsn *insn);

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

#endif /* X86PORT_JIT_X64_X87_H */
