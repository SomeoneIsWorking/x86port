/* Architectural x87 memory formats, separate from effective addressing and
 * instruction dispatch. All widths are validated before accessing guest data. */
#ifndef X86PORT_X87_MEMORY_H
#define X86PORT_X87_MEMORY_H
#include "cpu.h"

typedef enum X86pX87MemoryStatus {
  kX86pX87MemoryOk = 0,
  kX86pX87MemoryFault,
  kX86pX87MemoryUnsupported
} X86pX87MemoryStatus;
X86pX87MemoryStatus
x86p_x87_read_value(const X86pMem *mem, uint32_t address, unsigned width, int integer, long double *out);

/* The same two in the register file's storage type, which is what a JIT
   backend has and what the long double forms above are wrappers over. */
X86pX87MemoryStatus
x86p_x87_read_value_raw(const X86pMem *mem, uint32_t address, unsigned width, int integer, X86pX87Reg *out);
X86pX87MemoryStatus x86p_x87_write_value_raw(
    X86pX87 *f, const X86pMem *mem, uint32_t address, unsigned width, int integer, X86pX87Reg value);
X86pX87MemoryStatus
x86p_x87_write_value(X86pX87 *f, const X86pMem *mem, uint32_t address, unsigned width, int integer, long double value);

/*
 * The architectural bytes of a 2-, 4- or 8-byte operand, little-endian, as the
 * register file's storage type.
 *
 * A backend that has already fetched the operand converts through here instead
 * of handing an address back to x86p_x87_read_value_raw: the WebAssembly one
 * emits the load inline, exactly as it does for an integer operand, and would
 * otherwise pay a second walk of the guest mapping for a value it is already
 * holding. x86p_x87_read_value_raw calls it too, so the conversion table has
 * one home.
 *
 * Width 10 is absent on purpose. An 80-bit operand is ten bytes rather than an
 * integer's worth of bits, and no backend admits one.
 */
X86pX87MemoryStatus x86p_x87_reg_from_operand_bits(uint64_t bits, unsigned width, int integer, X86pX87Reg *out);
#endif
