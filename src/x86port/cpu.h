/*
 * cpu.h -- the guest machine state, and the two things every instruction does
 * to it: touch a register, touch memory.
 *
 * REGISTERS ARE AN ARRAY, INDEXED THE WAY THE ENCODING INDEXES THEM. The
 * substrate has named fields (`uint32_t eax, ecx, edx, ...`) and the generated
 * code names one per site, which works when a translator resolves the register
 * at build time and does not when an interpreter resolves it at run time from
 * three bits of a ModRM byte. The order here IS the encoding order, so a
 * decoded register number is an index and never a lookup table that can drift
 * from the manual.
 *
 * THE TWO TRAPS THIS FILE EXISTS TO GET RIGHT:
 *
 *  - A PARTIAL WRITE PRESERVES THE REST. `MOV AL, 0` leaves the top 24 bits of
 *    EAX alone, and `MOV AX, 0` leaves the top 16 alone. Writing the whole
 *    register is the single most natural mistake to make here, it looks right
 *    at every call site, and it corrupts a value the guest is still using.
 *  - BYTE REGISTERS 4-7 ARE HIGH BYTES, not registers 4-7. Byte index 4 is AH,
 *    the second byte of EAX -- NOT ESP. An interpreter that treats the byte and
 *    dword register files as the same eight things will read a stack pointer
 *    where the guest asked for a character.
 */
#ifndef X86PORT_CPU_H
#define X86PORT_CPU_H

#include "flags.h"
#include "x87.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register numbers, in the order the ModRM and opcode encodings use them. Do
   not reorder: a decoded field indexes this directly. */
typedef enum X86pReg {
  kX86pEax = 0,
  kX86pEcx = 1,
  kX86pEdx = 2,
  kX86pEbx = 3,
  kX86pEsp = 4,
  kX86pEbp = 5,
  kX86pEsi = 6,
  kX86pEdi = 7,
  kX86pRegCount /* MUST stay last */
} X86pReg;

/*
 * CACHE-LINE ALIGNED, and this is a measured requirement rather than tidiness.
 *
 * Translated code reads or writes this struct on nearly every emitted
 * instruction -- guest registers live here, and so does the flag state each
 * arithmetic operation records. When an instance straddled a cache-line
 * boundary, `x86p_jit_bench` measured translated code at 1.93 ns per guest
 * instruction; aligned, the same code measured 0.97. A clean factor of two,
 * decided by where the allocator happened to put it.
 *
 * The interpreter is indifferent (its cost is dominated by decode), which is
 * exactly why this went unnoticed until there was a translator, and why the
 * alignment belongs on the type rather than in one caller.
 */
typedef struct X86pCpu {
  /* The alignment specifier sits on the first member, which propagates to the
     whole struct -- C11 does not accept one on the struct tag itself. */
  _Alignas(64) uint32_t reg[kX86pRegCount];
  uint32_t eip;
  X86pFlags flags;
  /* The FPU is part of the guest's architectural state, not a separate
     machine: FNSTSW writes AX, and a guest that saves context saves both. It
     lives here so there is one thing to snapshot when two engines are compared
     against each other. */
  X86pX87 x87;
} X86pCpu;

/*
 * Guest memory: one host mapping of a contiguous guest range.
 *
 * This matches what the consuming port already has -- `pc/xmen2` maps every PE
 * section into a single arena and resolves addresses against it -- so the
 * interpreter does not impose a new memory model on a port that already works.
 * It is a struct rather than a global because two engines have to be able to
 * run against independent memory for in-process comparison, which is the exact
 * thing jit-common I004 records the port as not yet able to do.
 */
typedef struct X86pMem {
  uint8_t *host; /* host pointer for guest address `lo` */
  uint32_t lo;   /* first guest address covered */
  uint32_t size; /* bytes covered from `lo` */
} X86pMem;

/*
 * Read/write `w` bytes (1, 2 or 4) at a guest address, little-endian.
 *
 * Return 1 on success. Return 0, touching nothing, when the access is not
 * wholly inside the mapping -- INCLUDING an access that would wrap the 32-bit
 * address space. A guest fault is a fact the engine must be able to deliver,
 * so it is reported rather than aborted, and it must never be a truncated or
 * wrapped access that appears to have worked.
 *
 * Byte order is assembled explicitly rather than by casting a host pointer:
 * the guest is little-endian by definition and the host is whatever it is, and
 * a memcpy that happens to be right on x86 is a bug waiting for the ARM64 host
 * that S042 already plans for.
 */
int x86p_mem_read(const X86pMem *m, uint32_t addr, int w, uint32_t *out);
int x86p_mem_write(const X86pMem *m, uint32_t addr, int w, uint32_t value);

/*
 * Whole-span access, for operands that are not integers: an x87 double is 8
 * bytes and the extended format is 10, and neither is a value that fits the
 * uint32_t interface above. The bytes are copied as they lie -- these formats
 * are already little-endian in guest memory and interpreting them is x87.c's
 * job, not this module's.
 */
int x86p_mem_read_bytes(const X86pMem *m, uint32_t addr, void *dst, uint32_t n);
int x86p_mem_write_bytes(const X86pMem *m, uint32_t addr, const void *src, uint32_t n);

/* Whether an access of `w` bytes at `addr` is wholly mapped. Exposed so a
   caller can check before doing work it would have to undo. */
int x86p_mem_ok(const X86pMem *m, uint32_t addr, int w);

/*
 * Read/write a register by ENCODED NUMBER at a width.
 *
 * For w == 1 the index is a BYTE register: 0-3 are AL, CL, DL, BL and 4-7 are
 * AH, CH, DH, BH -- the high bytes of EAX..EBX, not registers 4-7. For w == 2
 * and 4 the index is the plain register number.
 *
 * A write of width 1 or 2 preserves every bit outside it.
 */
/*
 * Which 32-bit register a BYTE register index names, and how far up it sits.
 *
 * Indices 0-3 are AL/CL/DL/BL, the low byte of EAX..EBX; 4-7 are AH/CH/DH/BH,
 * the SECOND byte of those same four registers -- not SPL/BPL/SIL/DIL, which
 * need a REX prefix the 32-bit guest never has. Exported because a translator
 * has to reach the same byte the interpreter does, and two copies of this
 * mapping would disagree on exactly the four indices that matter.
 */
int x86p_byte_reg(int index, int *shift);

uint32_t x86p_reg_read(const X86pCpu *cpu, int index, int w);
void x86p_reg_write(X86pCpu *cpu, int index, int w, uint32_t value);

/* The register's name at a width -- "AL", "AH", "AX", "EAX" -- for divergence
   reports and traces. Never null, including for an out-of-range index. */
const char *x86p_reg_name(int index, int w);

/* Zero the machine. Registers, EIP and flags all start defined, so a
   divergence report can never be explained away by uninitialised state. */
void x86p_cpu_reset(X86pCpu *cpu);

/* Push/pop a dword, moving ESP. Return 1 on success; on a memory fault they
   return 0 having changed NOTHING, so a faulting push does not leave ESP moved
   past a value it never stored. */
int x86p_push32(X86pCpu *cpu, const X86pMem *m, uint32_t value);
int x86p_pop32(X86pCpu *cpu, const X86pMem *m, uint32_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_CPU_H */
