#include "jit_wasm_x87.h"

#include "jit_wasm_internal.h"
#include "jit_x87_predicates.h"
#include "x87_memory.h"
#include <stddef.h>

/*
 * These are the per-instruction helpers the emitted code calls, and they are
 * the reason x87 was 47% of the browser's guest worker: every one of them used
 * to convert the value into the host's widest float and straight back out
 * again, because that is what the register file held. They now stay in the
 * storage type from guest memory to guest memory.
 *
 * The reading three no longer receive an address either. Handing one over cost
 * a call into x86p_x87_read_value_raw, a walk of the guest mapping in
 * x86p_mem_read_bytes and a span resolution behind it -- together about a fifth
 * of the guest worker -- to fetch four or eight bytes that the emitted code can
 * load itself with the instruction wasm has for exactly that.
 */
static uint64_t operand_bits(uint32_t lo, uint32_t hi) {
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

int x86p_wasm_x87_load_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer) {
  X86pX87Reg value;
  if (x86p_x87_reg_from_operand_bits(operand_bits(lo, hi), width, (int)integer, &value) != kX86pX87MemoryOk) {
    return 0;
  }
  x86p_x87_push_raw(f, value);
  return 1;
}
int x86p_wasm_x87_store(X86pX87 *f, const X86pMem *mem, uint32_t address, uint32_t width, uint32_t integer) {
  X86pX87Reg value;
  if (!x86p_x87_get_raw(f, 0, &value)) {
    return 2;
  }
  return x86p_x87_write_value_raw(f, mem, address, width, (int)integer, value) == kX86pX87MemoryOk;
}
int x86p_wasm_x87_arith_mem_bits(
    X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t op, uint32_t reverse) {
  X86pX87Reg value;
  if (x86p_x87_reg_from_operand_bits(operand_bits(lo, hi), width, (int)integer, &value) != kX86pX87MemoryOk) {
    return 0;
  }
  x86p_x87_arith_raw(f, (X86pX87Op)op, 0, value, (int)reverse);
  return 1;
}
int x86p_wasm_x87_arith_reg(X86pX87 *f, uint32_t dst, uint32_t src, uint32_t op, uint32_t reverse) {
  X86pX87Reg value;
  if (!x86p_x87_get_raw(f, (int)src, &value)) {
    return 0;
  }
  x86p_x87_arith_raw(f, (X86pX87Op)op, (int)dst, value, (int)reverse);
  return 1;
}
int x86p_wasm_x87_compare_mem_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer) {
  X86pX87Reg value;
  if (x86p_x87_reg_from_operand_bits(operand_bits(lo, hi), width, (int)integer, &value) != kX86pX87MemoryOk) {
    return 0;
  }
  /* x86p_x87_compare still takes the host's widest float, so this one keeps a
     conversion the other two shed. It is the same conversion the old path made
     inside x86p_x87_read_value, not a new one -- comparison in the storage
     type is a separate change and needs its own ordering authority. */
  x86p_x87_compare(f, x86p_x87_reg_to_long_double(value));
  return 1;
}
int x86p_wasm_x87_copy(X86pX87 *f, uint32_t src, uint32_t dst, uint32_t push) {
  X86pX87Reg value;
  if (!x86p_x87_get_raw(f, (int)src, &value)) {
    return 0;
  }
  if (push) {
    x86p_x87_push_raw(f, value);
  } else {
    x86p_x87_set_raw(f, (int)dst, value);
  }
  return 1;
}

static int stack_operand(const X86pOperand *operand) {
  return operand->kind == kX86pOperandSt && operand->reg >= 0 && operand->reg < X86P_X87_REGS;
}
static int memory_operand(const X86pInsn *insn, unsigned size) {
  return insn->operands == 1 && insn->operand[0].kind == kX86pOperandMem && insn->operand[0].size == size &&
         !insn->operand[0].addr16;
}
int x86p_wasm_x87_accepts(const X86pInsn *insn) {
  if (insn->x87_pops > 2) {
    return 0;
  }
  if (x87_load_is_emittable(insn) || x87_arith_is_emittable(insn) || x87_store_reg_is_emittable(insn) ||
      x87_store_mem_is_emittable(insn) || x87_register_is_emittable(insn) || x87_compare_mem_is_emittable(insn) ||
      x87_constant_is_emittable(insn) || x87_fn_is_emittable(insn) || x87_control_is_emittable(insn) ||
      x87_status_ax_is_emittable(insn) || x87_clear_exceptions_is_emittable(insn)) {
    return 1;
  }
  switch (insn->x87) {
  case kX86pX87InsnWait:
  case kX86pX87InsnInit:
    return insn->operands == 0;
  case kX86pX87InsnStoreStatus:
    return memory_operand(insn, 2);
  case kX86pX87InsnFree:
    return insn->operands == 1 && stack_operand(&insn->operand[0]);
  case kX86pX87InsnCompareInt:
    return x87_values_are_emittable() && ((insn->operands == 1 && stack_operand(&insn->operand[0])) ||
                                          (insn->operands == 2 && stack_operand(&insn->operand[0]) &&
                                           insn->operand[0].reg == 0 && stack_operand(&insn->operand[1])));
  case kX86pX87InsnCmov:
    return x87_values_are_emittable() && insn->operands == 2 && stack_operand(&insn->operand[0]) &&
           insn->operand[0].reg == 0 && stack_operand(&insn->operand[1]) && insn->cond < kX86pCondCount;
  default:
    /* Raw f80 load/store and state restore can contain encodings binary128
       cannot preserve. Their named refusal survives the numeric admission. */
    return 0;
  }
}

static void self(X86pWasmLower *l) {
  x86p_wasm_state_x87_addr(&l->state);
}
static void integer(X86pWasmLower *l, uint32_t value) {
  x86p_wasm_i32_const(l->e, (int32_t)value);
}
static void pop_values(X86pWasmLower *l, unsigned count) {
  while (count--) {
    self(l);
    integer(l, 0);
    x86p_wasm_call_import(l, kX86pWasmImportX87Pop);
    x86p_wasm_drop(l->e);
  }
}
static uint32_t operand_is_integer(const X86pInsn *insn) {
  return insn->x87_mem_int || insn->x87 == kX86pX87InsnLoadInt || insn->x87 == kX86pX87InsnStoreInt;
}
static void memory_arguments(X86pWasmLower *l, const X86pInsn *insn) {
  self(l);
  integer(l, (uint32_t)(uintptr_t)l->fetch);
  x86p_wasm_state_address(&l->state, &insn->operand[0]);
  integer(l, insn->operand[0].size);
  integer(l, operand_is_integer(insn));
}

/*
 * The arguments for a reading form: the operand itself, fetched inline.
 *
 * The guard comes first and with an empty operand stack, and it is the same
 * x86p_wasm_state_guard every integer load emits -- bounds, then the two page
 * permission bytes, then an early return on a fault, all in wasm. Only after it
 * has passed does anything reach a helper, and what reaches the helper is a
 * value, so the helper has no address to fault on.
 */
static void memory_bits_arguments(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const int width = (int)insn->operand[0].size;
  x86p_wasm_state_guard(&l->state, &insn->operand[0], pc, width, kX86pMemRead);
  self(l);
  x86p_wasm_state_load_mem_pair(&l->state, width);
  integer(l, (uint32_t)width);
  integer(l, operand_is_integer(insn));
}
static void memory_result(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_local_tee(l->e, kX86pWasmLocalR);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitMemoryFault);
  x86p_wasm_end(l->e);
  if (insn->x87_pops) {
    x86p_wasm_local_get(l->e, kX86pWasmLocalR);
    integer(l, 1);
    x86p_wasm_i32_op(l->e, kWasmI32Eq);
    x86p_wasm_if(l->e, kWasmVoid);
    pop_values(l, insn->x87_pops);
    x86p_wasm_end(l->e);
  }
}
/*
 * A reading form's result. The zero branch is NOT the fault branch -- the guard
 * already took that one, before the load -- it is the width admitted by
 * x86p_wasm_x87_accepts meeting a conversion that does not know it, which would
 * mean this backend and x86p_x87_reg_from_operand_bits had drifted apart. It
 * refuses by name rather than pushing whatever a defaulted conversion returned.
 *
 * The pops are unconditional because the exit above is a `return`: control only
 * reaches them when the operation completed.
 */
static void bits_result(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitUnsupported);
  x86p_wasm_end(l->e);
  pop_values(l, insn->x87_pops);
}
static void copy_value(X86pWasmLower *l, unsigned src, unsigned dst, int push, unsigned pops) {
  self(l);
  integer(l, src);
  integer(l, dst);
  integer(l, (uint32_t)push);
  x86p_wasm_call_import(l, kX86pWasmImportX87Copy);
  x86p_wasm_if(l->e, kWasmVoid);
  pop_values(l, pops);
  x86p_wasm_end(l->e);
}
static void control(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const uint32_t offset = (uint32_t)(offsetof(X86pCpu, x87) + offsetof(X86pX87, control));
  const int load = insn->x87 == kX86pX87InsnLoadControl;
  x86p_wasm_state_guard(&l->state, &insn->operand[0], pc, 2, load ? 1u : 2u);
  if (load) {
    x86p_wasm_state_load_mem(&l->state, 2);
    x86p_wasm_local_set(l->e, kX86pWasmLocalR);
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_local_get(l->e, kX86pWasmLocalR);
    x86p_wasm_i32_store16(l->e, 1, offset);
  } else {
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load16_u(l->e, 1, offset);
    x86p_wasm_local_set(l->e, kX86pWasmLocalR);
    x86p_wasm_state_store_mem(&l->state, 2, kX86pWasmLocalR);
  }
}

void x86p_wasm_x87_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pOperand *first = &insn->operand[0];
  const int memory = insn->operands && first->kind == kX86pOperandMem;
  const unsigned index = insn->operands ? (unsigned)first->reg : 1u;
  switch (insn->x87) {
  case kX86pX87InsnLoad:
  case kX86pX87InsnLoadInt:
    if (!memory) {
      copy_value(l, index, 0, 1, 0);
      return;
    }
    memory_bits_arguments(l, insn, pc);
    x86p_wasm_call_import(l, kX86pWasmImportX87LoadBits);
    bits_result(l, insn, pc);
    return;
  case kX86pX87InsnStore:
  case kX86pX87InsnStoreInt:
    if (!memory) {
      copy_value(l, 0, index, 0, insn->x87_pops);
      return;
    }
    memory_arguments(l, insn);
    x86p_wasm_call_import(l, kX86pWasmImportX87Store);
    memory_result(l, insn, pc);
    return;
  case kX86pX87InsnArith:
    if (memory) {
      memory_bits_arguments(l, insn, pc);
      integer(l, insn->x87_op);
      integer(l, insn->x87_reverse);
      x86p_wasm_call_import(l, kX86pWasmImportX87ArithMemBits);
      bits_result(l, insn, pc);
    } else {
      self(l);
      integer(l, insn->operands == 2 ? index : 0);
      integer(l, insn->operands == 2 ? (unsigned)insn->operand[1].reg : index);
      integer(l, insn->x87_op);
      integer(l, insn->x87_reverse);
      x86p_wasm_call_import(l, kX86pWasmImportX87ArithReg);
      x86p_wasm_if(l->e, kWasmVoid);
      pop_values(l, insn->x87_pops);
      x86p_wasm_end(l->e);
    }
    return;
  case kX86pX87InsnCompare:
    if (memory) {
      memory_bits_arguments(l, insn, pc);
      x86p_wasm_call_import(l, kX86pWasmImportX87CompareMemBits);
      bits_result(l, insn, pc);
    } else {
      self(l);
      integer(l, index);
      integer(l, insn->x87_pops);
      x86p_wasm_call_import(l, kX86pWasmImportX87CompareRegister);
    }
    return;
  case kX86pX87InsnCompareInt:
    self(l);
    x86p_wasm_state_flags_addr(&l->state);
    integer(l, insn->operands == 2 ? (unsigned)insn->operand[1].reg : index);
    x86p_wasm_call_import(l, kX86pWasmImportX87CompareFlags);
    x86p_wasm_if(l->e, kWasmVoid);
    pop_values(l, insn->x87_pops);
    x86p_wasm_end(l->e);
    l->last_kind = -1;
    return;
  case kX86pX87InsnCmov:
    integer(l, insn->cond);
    x86p_wasm_state_flags_addr(&l->state);
    x86p_wasm_call_import(l, kX86pWasmImportCond);
    x86p_wasm_if(l->e, kWasmVoid);
    copy_value(l, (unsigned)insn->operand[1].reg, 0, 0, 0);
    x86p_wasm_end(l->e);
    return;
  case kX86pX87InsnLoadControl:
  case kX86pX87InsnStoreControl:
    control(l, insn, pc);
    return;
  case kX86pX87InsnStoreStatus:
    if (memory) {
      x86p_wasm_state_guard(&l->state, first, pc, 2, 2u);
    }
    self(l);
    x86p_wasm_call_import(l, kX86pWasmImportX87Status);
    x86p_wasm_local_set(l->e, kX86pWasmLocalR);
    if (memory) {
      x86p_wasm_state_store_mem(&l->state, 2, kX86pWasmLocalR);
    } else {
      x86p_wasm_state_store_reg(&l->state, kX86pEax, 2, kX86pWasmLocalR);
    }
    return;
  case kX86pX87InsnExchange:
  case kX86pX87InsnFree:
    self(l);
    integer(l, index);
    x86p_wasm_call_import(l, insn->x87 == kX86pX87InsnFree ? kX86pWasmImportX87Free : kX86pWasmImportX87Exchange);
    return;
  case kX86pX87InsnChangeSign:
  case kX86pX87InsnAbs:
    self(l);
    integer(l, insn->x87 == kX86pX87InsnAbs);
    x86p_wasm_call_import(l, kX86pWasmImportX87Sign);
    return;
  case kX86pX87InsnTest:
    self(l);
    x86p_wasm_call_import(l, kX86pWasmImportX87Test);
    return;
  case kX86pX87InsnClearExc:
  case kX86pX87InsnInit:
    self(l);
    x86p_wasm_call_import(l, insn->x87 == kX86pX87InsnInit ? kX86pWasmImportX87Reset : kX86pWasmImportX87Clear);
    return;
  case kX86pX87InsnWait:
    return;
  case kX86pX87InsnFn:
    self(l);
    integer(l, insn->x87_fn);
    x86p_wasm_call_import(l, kX86pWasmImportX87Fn);
    x86p_wasm_i32_op(l->e, kWasmI32Eqz);
    x86p_wasm_if(l->e, kWasmVoid);
    x86p_wasm_state_exit_imm(&l->state, pc, kX86pJitExitUnsupported);
    x86p_wasm_end(l->e);
    return;
  default: /* The admission predicate restricts this branch to constants. */
    self(l);
    integer(l, insn->x87);
    x86p_wasm_call_import(l, kX86pWasmImportX87Constant);
    x86p_wasm_drop(l->e);
    return;
  }
}
