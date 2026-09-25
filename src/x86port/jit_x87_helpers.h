/*
 * jit_x87_helpers.h -- the x87 helpers translated code calls, in the
 * register file's own storage type.
 *
 * Every one stays in X86pX87Reg from guest memory to guest memory. The
 * `long double` API in x87.h is the interpreter's and the oracle's; on a host
 * whose `long double` is software binary128 (Linux/Android AArch64, wasm)
 * each call through it widened and re-encoded the value, and that round trip
 * was most of the x87 cost on both. Operands arrive as bits split into two
 * 32-bit halves so the WebAssembly backend can pass them as i32s; the AArch64
 * backend passes the same halves.
 *
 * A helper that completed its operation also does its pops (see `popped` in
 * the .c); only its success path pops.
 */
#ifndef X86PORT_JIT_X87_HELPERS_H
#define X86PORT_JIT_X87_HELPERS_H

#include "cpu.h"

#include <stdint.h>

int x86p_jit_x87_load_bits(X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t pops);
int x86p_jit_x87_store(
    X86pX87 *f, const X86pMem *mem, uint32_t address, uint32_t width, uint32_t integer, uint32_t pops);
/* ST(0) as `width` bytes of memory operand into `out`, without storing or
   popping: 2 when ST(0) is empty, 0 when the conversion refused, 1 done. The
   conversion's status flags are raised either way, before any address is
   looked at, as the interpreter raises them. */
int x86p_jit_x87_store_bytes(X86pX87 *f, uint32_t width, uint32_t integer, uint8_t *out);
int x86p_jit_x87_store_at(X86pX87 *f, uint8_t *at, uint32_t permitted, uint32_t width, uint32_t integer, uint32_t pops);
int x86p_jit_x87_arith_mem_bits(X86pX87 *f,
                                uint32_t lo,
                                uint32_t hi,
                                uint32_t width,
                                uint32_t integer,
                                uint32_t op,
                                uint32_t reverse,
                                uint32_t pops);
int x86p_jit_x87_arith_reg(X86pX87 *f, uint32_t dst, uint32_t src, uint32_t op, uint32_t reverse, uint32_t pops);
int x86p_jit_x87_compare_mem_bits(
    X86pX87 *f, uint32_t lo, uint32_t hi, uint32_t width, uint32_t integer, uint32_t pops);
int x86p_jit_x87_copy(X86pX87 *f, uint32_t src, uint32_t dst, uint32_t push, uint32_t pops);

#endif
