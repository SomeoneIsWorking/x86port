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
 * empty source register (no access and no pop).
 *
 * `pops` is how many stack slots the instruction retires, and the helper does
 * them itself, on its success path only. It used to be emitted as a separate
 * `x87_pop` import call per pop, guarded in wasm by the value the helper had
 * just returned -- so FSTP, the commonest x87 form the game emits, crossed
 * twice for one instruction. The helper already knows whether it completed. */
#endif
