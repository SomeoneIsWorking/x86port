#include "cpu.h"
#include "jit_engine.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kGuestBase = 0x400000, kGuestBytes = 4096 };

static uint8_t guest[kGuestBytes];
static unsigned failures;

static void check(int condition, const char *message) {
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

static void run_on_current_worker(void) {
  X86pMem mem = {.host = guest, .lo = kGuestBase, .size = sizeof guest};
  X86pCpu cpu;
  X86pJitEngine *engine;
  char reason[256] = {0};
  X86pJitRunStatus status;

  engine = x86p_jit_engine_create(&mem, 65536u, 128u, reason, sizeof reason);
  check(engine != NULL, reason);
  if (!engine) {
    return;
  }
  x86p_cpu_reset(&cpu);
  cpu.eip = kGuestBase;
  status = x86p_jit_engine_run(engine, &cpu, NULL, 2u, reason, sizeof reason);
  check(status == kX86pRunBudget, reason);
  check(cpu.reg[kX86pEax] == 0x1234u, "worker executed the wrong translated result");
  x86p_jit_engine_destroy(engine);
}

static void *worker_main(void *unused) {
  (void)unused;
  run_on_current_worker();
  return NULL;
}

int main(void) {
  pthread_t worker;
  int result;
  guest[0] = 0xb8; /* MOV EAX,0x1234; JMP $ */
  guest[1] = 0x34;
  guest[2] = 0x12;
  guest[3] = 0;
  guest[4] = 0;
  guest[5] = 0xeb;
  guest[6] = 0xfe;

  run_on_current_worker();
  result = pthread_create(&worker, NULL, worker_main, NULL);
  check(result == 0, "cannot start the second WASM worker");
  if (result == 0) {
    check(pthread_join(worker, NULL) == 0, "cannot join the second WASM worker");
  }
  printf("WebAssembly worker-local runtime: %u failures\n", failures);
  return failures ? 1 : 0;
}
