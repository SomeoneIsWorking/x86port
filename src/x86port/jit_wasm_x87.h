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
 * The STORE forms do their own writing, because handing the bytes back would
 * need a second result and every import returns one i32 at most. There is one
 * per mapping a block may have been translated for, because the address they
 * are given is not the same kind of thing:
 *
 *  - _store, the sparse mapping: a GUEST address, written through
 *    x86p_mem_write_bytes, which resolves and checks it.
 *  - _store_at, the contiguous mapping: a pointer the emitted code has already
 *    bounds- and permission-checked, reached with no walk at all. `permitted`
 *    is that check's verdict, and it arrives as a value rather than as an
 *    early return so the conversion can set the status word first.
 *
 * Both keep the older contract: 0 on fault, 1 on completed operation, 2 on an
 * empty source register (no access and no pop). */
int x86p_wasm_x87_load_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer);
int x86p_wasm_x87_store(X86pX87 *f, const X86pMem *mem, uint32_t address, uint32_t width, uint32_t integer);
int x86p_wasm_x87_store_at(X86pX87 *f, uint8_t *at, uint32_t permitted, uint32_t width, uint32_t integer);
int x86p_wasm_x87_arith_mem_bits(
    X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t op, uint32_t reverse);
int x86p_wasm_x87_arith_reg(X86pX87 *f, uint32_t dst, uint32_t src, uint32_t op, uint32_t reverse);
int x86p_wasm_x87_compare_mem_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer);
int x86p_wasm_x87_copy(X86pX87 *f, uint32_t src, uint32_t dst, uint32_t push);
#endif
