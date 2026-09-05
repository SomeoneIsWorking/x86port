#ifndef X86PORT_JIT_X87_PREDICATES_H
#define X86PORT_JIT_X87_PREDICATES_H

#include "decode.h"

/* Execution admission is distinct from exact extended-precision fidelity. */
int x87_values_are_emittable(void);
int x87_register_is_emittable(const X86pInsn *insn);
int x87_fn_is_emittable(const X86pInsn *insn);
int x87_control_is_emittable(const X86pInsn *insn);

/* Can this X87 instruction be emitted natively? Exactly one predicate is true
   for an emittable instruction; both backend dispatchers use this same admission policy. */
int x87_load_is_emittable(const X86pInsn *insn);
int x87_arith_is_emittable(const X86pInsn *insn);
int x87_compare_mem_is_emittable(const X86pInsn *insn);
int x87_store_reg_is_emittable(const X86pInsn *insn);
int x87_store_mem_is_emittable(const X86pInsn *insn);
int x87_constant_is_emittable(const X86pInsn *insn);
int x87_status_ax_is_emittable(const X86pInsn *insn);
int x87_clear_exceptions_is_emittable(const X86pInsn *insn);

#endif
