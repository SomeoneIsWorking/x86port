#ifndef X86PORT_TEST_WASM_DIFFERENTIAL_H
#define X86PORT_TEST_WASM_DIFFERENTIAL_H
#include "cpu.h"
#include <stddef.h>
#define GUEST_BASE 0x400000u
#define GUEST_SIZE 4096u
typedef struct WasmTest {
  uint8_t guest[GUEST_SIZE], reference[GUEST_SIZE];
  unsigned checks, failures, cases, entered, faults;
  const char *current;
  X86pCpu last_cpu;
} WasmTest;
void wasm_test_check(WasmTest *suite, int value, const char *message);
X86pCpu wasm_test_initial(WasmTest *suite);
void wasm_test_case(WasmTest *suite, const char *name, const uint8_t *code, size_t size, X86pCpu cpu, int branch);
void wasm_test_case_mem(WasmTest *suite,
                        const char *name,
                        const uint8_t *code,
                        size_t size,
                        X86pCpu cpu,
                        int branch,
                        const X86pMem *mem,
                        const X86pMem *oracle_mem);
#endif
