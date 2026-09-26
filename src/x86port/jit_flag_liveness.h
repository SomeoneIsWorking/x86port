#ifndef X86PORT_JIT_FLAG_LIVENESS_H
#define X86PORT_JIT_FLAG_LIVENESS_H

#include "cpu.h"
#include "decode.h"
#include "jit_x64.h"

#include <stddef.h>
#include <stdint.h>

/*
 * DEAD FLAG STORE ELIMINATION, shared by the native backends.
 *
 * Most guest arithmetic never has its flags read: `add / add / cmp / jl` writes
 * three flag tuples and only the last one matters. This scans forward from
 * `pc`, the instruction AFTER a flag writer, and returns 1 when the flags that
 * writer produced are provably overwritten before anything can observe them --
 * so its tuple stores (and its carry-in computation) can be skipped entirely.
 *
 * It only says "dead" for a later full-width ALU flag write (ADD, SUB, CMP,
 * AND, OR, XOR, TEST), a SHL/SHR/SAR by a nonzero constant, or NEG, each with
 * register/immediate operands: those overwrite every EFLAGS bit and cannot
 * fault. It stops -- conservatively "not dead" -- at the first thing that
 * could read flags (Jcc, INC/DEC which preserve CF, ADC/SBB, a helper), could
 * fault mid-block and expose a stale tuple (any memory operand), or ends the
 * block (branch, ret, unsupported, interception point, unmapped). Register
 * moves, LEA, NOT and NOPs are transparent and scanned through.
 *
 * Safety of the carry-in: the block's LAST flag writer is never dead (the scan
 * from it hits the block boundary), so it always stores. If ITS predecessor was
 * elided, its carry_in is computed from an older kind -- but a wrong carry_in is
 * only observable for kind Inc/Dec, and the predecessor of an Inc/Dec is never
 * elided (this scan returns 0 at Inc/Dec). cpu_compare.c states the matching
 * rule for the differential.
 *
 * The killer must be an instruction this block will ACTUALLY EMIT: `count`
 * (instructions already in the block), `max_insns`, and the remaining code
 * budget (`code_len` of `code_cap`) cut the scan short exactly where the emit
 * loop would stop, and `can_emit` is the backend's own translation gate, so a
 * store is never dropped on the strength of a successor that ends up in the
 * next block instead.
 */
typedef int (*X86pJitCanEmitFn)(const X86pInsn *insn);

typedef struct X86pJitFlagScan {
  const X86pMem *mem;
  uint32_t eip; /* the block's first instruction */
  X86pJitBoundaryFn boundary;
  void *boundary_user;
  uint32_t count;
  uint32_t max_insns;
  size_t code_len;
  size_t code_cap;
  X86pJitCanEmitFn can_emit;
} X86pJitFlagScan;

int x86p_jit_flag_write_is_dead(const X86pJitFlagScan *scan, uint32_t pc);

#endif /* X86PORT_JIT_FLAG_LIVENESS_H */
