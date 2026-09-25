/* See jit_x87_helpers.h. */
#include "jit_x87_helpers.h"

#include "x87_ext80_arith.h"
#include "x87_memory.h"

#include <string.h>

/*
 * These are the per-instruction helpers the WebAssembly and AArch64 backends'
 * emitted code calls. They were the reason x87 was 47% of the browser's guest
 * worker: every one of them used
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

/*
 * A helper that completed its operation also does its pops.
 *
 * The stack pop after an x87 instruction used to be emitted as its own import
 * call -- `x87_pop` once per pop, guarded by whatever branch decided the
 * operation had happened. FSTP is the commonest x87 form the game emits and it
 * paid two crossings for one instruction: the store, then the pop. The pops
 * are 1.89% of the browser's guest worker on their own, and that is before the
 * argument setup and the `drop` around each call.
 *
 * `x87_compare_register` already took its pop count and did this; the other
 * helpers did not, and the branch that decided whether to pop was emitted in
 * wasm from the value the helper had just returned. So this is one rule
 * applied to all of them rather than a new one: the helper knows whether it
 * succeeded without being asked a second time.
 *
 * Only a helper's SUCCESS path pops. A store that found the stack empty
 * returns 2 and pops nothing, which is the condition the emitted code used to
 * spell as `r == 1`.
 */
static void popped(X86pX87 *f, uint32_t pops) {
  while (pops--) {
    x86p_x87_pop(f, NULL);
  }
}

int x86p_jit_x87_load_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t pops) {
  X86pX87Reg value;
  if (x86p_x87_reg_from_operand_bits(operand_bits(lo, hi), width, (int)integer, &value) != kX86pX87MemoryOk) {
    return 0;
  }
  x86p_x87_push_raw(f, value);
  popped(f, pops);
  return 1;
}
int x86p_jit_x87_store(
    X86pX87 *f, const X86pMem *mem, uint32_t address, uint32_t width, uint32_t integer, uint32_t pops) {
  X86pX87Reg value;
  if (!x86p_x87_get_raw(f, 0, &value)) {
    return 2;
  }
  if (x86p_x87_write_value_raw(f, mem, address, width, (int)integer, value) != kX86pX87MemoryOk) {
    return 0;
  }
  popped(f, pops);
  return 1;
}
int x86p_jit_x87_store_bytes(X86pX87 *f, uint32_t width, uint32_t integer, uint8_t *out) {
  X86pX87Reg value;
  if (!x86p_x87_get_raw(f, 0, &value)) {
    return 2;
  }
  return x86p_x87_operand_bytes_from_reg(f, value, width, (int)integer, out) == kX86pX87MemoryOk;
}
int x86p_jit_x87_store_at(
    X86pX87 *f, uint8_t *at, uint32_t permitted, uint32_t width, uint32_t integer, uint32_t pops) {
  uint8_t bytes[X86P_X87_OPERAND_BYTES];
  /* The conversion runs BEFORE the verdict is consulted, and that order is the
     reason the verdict arrives as an argument rather than as a branch in the
     emitted code: FIST of a value it cannot represent sets the
     invalid-operation flag even when the destination is not writable, and the
     interpreter this is checked against sets it too. */
  const int converted = x86p_jit_x87_store_bytes(f, width, integer, bytes);
  if (converted != 1) {
    return converted;
  }
  if (!permitted) {
    return 0;
  }
  memcpy(at, bytes, width);
  popped(f, pops);
  return 1;
}
/*
 * A float memory operand as the encoding the rules take, skipping the register
 * type entirely. Only the two widths a game actually multiplies by; an integer
 * operand or an 80-bit one falls through to the long path, which is still the
 * one authority for them.
 */
static int operand_ext80(uint64_t bits, uint32_t width, uint32_t integer, X86pExt80 *out) {
  if (integer) {
    return 0;
  }
  if (width == 4u) {
    *out = x86p_ext80_from_f32_bits((uint32_t)bits);
    return 1;
  }
  if (width == 8u) {
    *out = x86p_ext80_from_f64_bits(bits);
    return 1;
  }
  return 0;
}

int x86p_jit_x87_arith_mem_bits(X86pX87 *f,
                                uint32_t lo,
                                uint32_t hi,
                                uint32_t width,
                                uint32_t integer,
                                uint32_t op,
                                uint32_t reverse,
                                uint32_t pops) {
  const uint64_t bits = operand_bits(lo, hi);
  X86pX87Reg value;
  X86pExt80 wide;
  if (operand_ext80(bits, width, integer, &wide) &&
      x86p_x87_arith_ext80_fast(f, (X86pX87Op)op, 0, wide, (int)reverse)) {
    popped(f, pops);
    return 1;
  }
  if (x86p_x87_reg_from_operand_bits(bits, width, (int)integer, &value) != kX86pX87MemoryOk) {
    return 0;
  }
  x86p_x87_arith_raw(f, (X86pX87Op)op, 0, value, (int)reverse);
  popped(f, pops);
  return 1;
}
int x86p_jit_x87_arith_reg(X86pX87 *f, uint32_t dst, uint32_t src, uint32_t op, uint32_t reverse, uint32_t pops) {
  X86pX87Reg value;
  X86pExt80 wide;
  if (x86p_x87_ext80_of_st(f, (int)src, &wide) &&
      x86p_x87_arith_ext80_fast(f, (X86pX87Op)op, (int)dst, wide, (int)reverse)) {
    popped(f, pops);
    return 1;
  }
  if (!x86p_x87_get_raw(f, (int)src, &value)) {
    return 0;
  }
  x86p_x87_arith_raw(f, (X86pX87Op)op, (int)dst, value, (int)reverse);
  popped(f, pops);
  return 1;
}
int x86p_jit_x87_compare_mem_bits(
    X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t pops) {
  X86pX87Reg value;
  if (x86p_x87_reg_from_operand_bits(operand_bits(lo, hi), width, (int)integer, &value) != kX86pX87MemoryOk) {
    return 0;
  }
  /* x86p_x87_compare still takes the host's widest float, so this one keeps a
     conversion the other two shed. It is the same conversion the old path made
     inside x86p_x87_read_value, not a new one -- comparison in the storage
     type is a separate change and needs its own ordering authority. */
  x86p_x87_compare(f, x86p_x87_reg_to_long_double(value));
  popped(f, pops);
  return 1;
}
int x86p_jit_x87_copy(X86pX87 *f, uint32_t src, uint32_t dst, uint32_t push, uint32_t pops) {
  X86pX87Reg value;
  if (!x86p_x87_get_raw(f, (int)src, &value)) {
    return 0;
  }
  if (push) {
    x86p_x87_push_raw(f, value);
  } else {
    x86p_x87_set_raw(f, (int)dst, value);
  }
  popped(f, pops);
  return 1;
}
