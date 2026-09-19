#ifndef X86PORT_JIT_WASM_X87_H
#define X86PORT_JIT_WASM_X87_H
#include "cpu.h"
#include "decode.h"

struct X86pWasmLower;
int x86p_wasm_x87_accepts(const X86pInsn *insn);
void x86p_wasm_x87_lower(struct X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);

/* Wasm's scalar ABI cannot carry long double. These narrow operand adapters
 * call the shared x87 value/stack owners; none decodes or dispatches
 * instructions.
 *
 * The three READING forms take the operand itself, as a low/high i32 pair,
 * because the backend emits the load inline -- the same wasm load and the same
 * inline permission check an integer operand gets. They never touch guest
 * memory, so they cannot fault; they return 0 only for a width no conversion
 * knows, which is a refusal rather than a guest fault.
 *
 * The STORE form still takes an address, because writing back needs a second
 * result -- the bytes -- and every import returns one i32 at most. It keeps the
 * older contract: 0 on fault, 1 on completed operation, 2 on an empty source
 * register (no access and no pop). */
int x86p_wasm_x87_load_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer);
int x86p_wasm_x87_store(X86pX87 *f, const X86pMem *mem, uint32_t address, uint32_t width, uint32_t integer);
int x86p_wasm_x87_arith_mem_bits(
    X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t op, uint32_t reverse);
int x86p_wasm_x87_arith_reg(X86pX87 *f, uint32_t dst, uint32_t src, uint32_t op, uint32_t reverse);
int x86p_wasm_x87_compare_mem_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer);
int x86p_wasm_x87_copy(X86pX87 *f, uint32_t src, uint32_t dst, uint32_t push);
#endif
