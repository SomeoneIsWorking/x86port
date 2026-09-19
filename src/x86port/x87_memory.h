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
#endif
