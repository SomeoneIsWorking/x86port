/*
 * jit_x64_harness -- the executable scaffolding x64 JIT differential tests
 * share: a guest mapping that ends on an unreadable page, and one code region
 * translated into and published from.
 *
 * Test-only. Each test keeps its own programs, oracles and comparisons.
 */
#ifndef X86PORT_TESTS_JIT_X64_HARNESS_H
#define X86PORT_TESTS_JIT_X64_HARNESS_H

#include "cpu.h"
#include "jit_x64.h"

#include <stddef.h>
#include <stdint.h>

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 4096u

/* GUEST_SIZE bytes whose last byte is followed by an unreadable page. Exits
   the process with a FATAL line if the platform cannot provide one. */
uint8_t *jit_x64_harness_guest_init(void);
X86pMem jit_x64_harness_mem(uint8_t *guest);

/* One writable code region at a time; NULL (with a REFUSED line) if the host
   refuses executable memory. */
void *jit_x64_harness_code_alloc(size_t n);
void jit_x64_harness_code_free(void *code, size_t n);

/* Translate at `eip` into the active region and publish it executable. */
X86pJitStatus jit_x64_harness_translate(const X86pMem *mem,
                                        uint32_t eip,
                                        void *code,
                                        size_t code_cap,
                                        X86pJitBlock *block,
                                        char *reason,
                                        unsigned reason_len);

#endif /* X86PORT_TESTS_JIT_X64_HARNESS_H */
