/*
 * jit_wasm_branch.c -- the control transfers a block ends with.
 *
 * A branch ENDS the block -- that is what makes it a basic block -- so every
 * lowering here writes the guest EIP and returns rather than falling through
 * to the block's own epilogue.
 *
 * NO SHARED EXIT STUB, unlike the machine-code backends. WebAssembly's control
 * flow is structured: there are no forward jumps to bind, an early `return`
 * costs what a branch to a stub costs, and a stub would need a `block`
 * wrapping the whole body just to have something to branch to. The conditional
 * forms are an `if` around a `return`, which is the shape the format actually
 * offers.
 *
 * CALL and RET are transfers the block COMPLETES rather than refuses: ending
 * with kX86pJitExitBlockEnd and the right EIP leaves the dispatcher a plain
 * address to look up, where refusing the block would make the run fall back.
 * The indirect forms end the block too -- their target is not known until the
 * block runs -- but they are still lowered, because reading a register or a
 * memory word is something emitted code can do.
 */
#include "jit_wasm_cond.h"
#include "jit_wasm_internal.h"

#include <stddef.h>

static int is_relative_immediate(const X86pInsn *insn) {
  return insn->operands == 1 && insn->operand[0].kind == kX86pOperandImm && insn->operand[0].relative;
}

static int is_indirect_target(const X86pOperand *o) {
  if (o->kind == kX86pOperandReg) {
    return o->size == 4;
  }
  return o->kind == kX86pOperandMem && o->size == 4 && !o->addr16;
}

/* The address a relative branch names: the displacement is relative to the
   NEXT instruction, and the sum wraps at 32 bits exactly as the guest's
   does. */
static uint32_t relative_target(const X86pInsn *insn, uint32_t pc) {
  return pc + insn->length + insn->operand[0].imm;
}

/*
 * Read a branch target into kX86pWasmLocalTarget.
 *
 * Read BEFORE anything else touches the stack, because `CALL [ESP+4]` must
 * take its target from the stack as it stands and not from the stack after the
 * return address has been pushed onto it.
 */
static void read_target(X86pWasmLower *l, const X86pOperand *o, uint32_t pc) {
  if (o->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, o, pc, 4, kX86pMemRead);
    x86p_wasm_state_load_mem(&l->state, 4);
  } else {
    x86p_wasm_state_load_reg(&l->state, o->reg, 4);
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalTarget);
}

int x86p_wasm_jmp_accepts(const X86pInsn *insn) {
  if (is_relative_immediate(insn)) {
    return 1;
  }
  return insn->operands == 1 && is_indirect_target(&insn->operand[0]);
}

void x86p_wasm_jmp_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  if (is_relative_immediate(insn)) {
    x86p_wasm_state_exit_imm(&l->state, relative_target(insn, pc), kX86pJitExitBlockEnd);
    return;
  }
  read_target(l, &insn->operand[0], pc);
  x86p_wasm_state_exit_local(&l->state, kX86pWasmLocalTarget, kX86pJitExitBlockEnd);
}

int x86p_wasm_jcc_accepts(const X86pInsn *insn) {
  return is_relative_immediate(insn);
}

void x86p_wasm_jcc_continue(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_cond_value(l, (X86pCond)insn->cond);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, relative_target(insn, pc), kX86pJitExitBlockEnd);
  x86p_wasm_end(l->e);
}

void x86p_wasm_jcc_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  /* The taken path alone, then the fall-through's exit: a block that ends here
     emits exactly what a block that continues past it does, plus that one
     epilogue. */
  x86p_wasm_jcc_continue(l, insn, pc);
  x86p_wasm_state_exit_imm(&l->state, pc + insn->length, kX86pJitExitBlockEnd);
}

int x86p_wasm_jecxz_accepts(const X86pInsn *insn) {
  if (!is_relative_immediate(insn)) {
    return 0;
  }
  /* The counter is ECX or CX, and which one is the ADDRESS width rather than
     the operand width -- the decoder records it, and reading the wrong half
     makes the branch depend on bits the instruction does not look at. */
  return insn->address_width == 32 || insn->address_width == 16;
}

void x86p_wasm_jecxz_continue(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_state_load_reg(&l->state, kX86pEcx, insn->address_width == 16 ? 2 : 4);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, relative_target(insn, pc), kX86pJitExitBlockEnd);
  x86p_wasm_end(l->e);
}

void x86p_wasm_jecxz_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_jecxz_continue(l, insn, pc);
  x86p_wasm_state_exit_imm(&l->state, pc + insn->length, kX86pJitExitBlockEnd);
}

int x86p_wasm_call_accepts(const X86pInsn *insn) {
  if (is_relative_immediate(insn)) {
    return 1;
  }
  return insn->operands == 1 && is_indirect_target(&insn->operand[0]);
}

void x86p_wasm_call_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const uint32_t next = pc + insn->length;
  const int indirect = !is_relative_immediate(insn);

  if (indirect) {
    read_target(l, &insn->operand[0], pc);
  }
  x86p_wasm_i32_const(l->e, (int32_t)next);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_push_local(l, kX86pWasmLocalR, pc);
  if (x86p_wasm_leaf_call_lower(l, indirect ? 0u : relative_target(insn, pc), next, indirect)) {
    return;
  }
  if (indirect) {
    x86p_wasm_state_exit_local(&l->state, kX86pWasmLocalTarget, kX86pJitExitBlockEnd);
    return;
  }
  x86p_wasm_state_exit_imm(&l->state, relative_target(insn, pc), kX86pJitExitBlockEnd);
}

int x86p_wasm_ret_accepts(const X86pInsn *insn) {
  if (insn->operands == 0) {
    return 1;
  }
  return insn->operands == 1 && insn->operand[0].kind == kX86pOperandImm;
}

void x86p_wasm_ret_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  /* `RET imm16`'s argument count is applied AFTER the pop, because the
     immediate counts bytes ABOVE the return address. */
  const uint32_t release = (insn->operands == 1) ? insn->operand[0].imm : 0u;

  x86p_wasm_state_load_reg(&l->state, kX86pEsp, 4);
  x86p_wasm_local_tee(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalAddr);
  x86p_wasm_state_guard_addr(&l->state, pc, 4, kX86pMemRead);
  x86p_wasm_state_load_mem(&l->state, 4);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalTarget);

  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_i32_const(l->e, (int32_t)(4u + release));
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalA);
  x86p_wasm_state_store_reg(&l->state, kX86pEsp, 4, kX86pWasmLocalA);

  x86p_wasm_state_exit_local(&l->state, kX86pWasmLocalTarget, kX86pJitExitBlockEnd);
}
