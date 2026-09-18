/*
 * Exact page permissions in EMITTED code, on the contiguous memory mode.
 *
 * The contiguous mode lowers a guest access to a bounds compare, a page
 * permission load and a direct wasm load or store -- no call and no search.
 * Its whole point is that the browser can have the sparse mode's exact
 * permissions at the contiguous mode's cost, so what has to be proved is that
 * the GENERATED code refuses what the helpers refuse. A test that only
 * exercised x86p_mem_read/write would agree with itself and prove nothing
 * about the guard the translator writes.
 *
 * Every check below runs through x86p_jit_engine_run, so it is the emitted
 * guard that answers.
 */
#include "cpu.h"
#include "jit_engine.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  kPageShift = 12,
  kPage = 1u << kPageShift,
  kPages = 8,
  kLo = 0u,
  kCodePage = 4,
  kCode = kCodePage * kPage,
  kDataPage = 6
};
#define DATA ((uint32_t)(kDataPage * kPage))
/* Two bytes before the page end, so a 4-byte access straddles into the next
   page -- the case the guard's second permission load exists for. */
#define CROSS (DATA + kPage - 2u)

static unsigned checks, failures;
static uint8_t g_bytes[kPages * kPage];
static uint8_t g_perms[kPages];

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

static X86pJitRunStatus run(X86pJitEngine *engine, X86pCpu *cpu) {
  char reason[256] = {0};
  X86pJitRunStatus result = x86p_jit_engine_run(engine, cpu, NULL, 1, reason, sizeof reason);
  if (result != kX86pRunBudget && result != kX86pRunMemoryFault) {
    check(0, reason);
  }
  return result;
}

int main(void) {
  /* MOV [EBX],EAX; MOV EDX,[EBX]; JMP $ */
  static const uint8_t code[] = {0x89, 0x03, 0x8b, 0x13, 0xeb, 0xfe};
  X86pMem memory = {0};
  X86pJitEngine *engine;
  X86pCpu cpu, snapshot;
  char reason[256] = {0};
  unsigned page;

  memory.host = g_bytes;
  memory.lo = kLo;
  memory.size = sizeof g_bytes;
  memory.perms = g_perms;
  memory.page_shift = kPageShift;
  for (page = 0; page < kPages; ++page) {
    g_perms[page] = kX86pMemRead | kX86pMemWrite;
  }
  memcpy(g_bytes + kCode, code, sizeof code);

  engine = x86p_jit_engine_create(&memory, 65536, 128, reason, sizeof reason);
  check(engine != NULL, reason);
  if (!engine) {
    return 1;
  }
  x86p_cpu_reset(&cpu);
  cpu.eip = kCode;
  cpu.reg[kX86pEax] = 0x76543210;
  cpu.reg[kX86pEbx] = CROSS;
  check(run(engine, &cpu) == kX86pRunBudget, "generated straddling store and load execute");
  check(cpu.reg[kX86pEdx] == 0x76543210, "generated straddling load value");
  check(g_bytes[DATA + kPage - 2u] == 0x10 && g_bytes[DATA + kPage] == 0x54,
        "generated straddling store reached both pages");

  /*
   * Read-only, on the SECOND page of the straddle only. A guard that checked
   * just the first byte's page would let this through, which is the whole
   * reason the second load is emitted.
   */
  g_perms[kDataPage + 1] = kX86pMemRead;
  cpu.eip = kCode;
  cpu.reg[kX86pEax] = 0xffffffffu;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "generated store straddling into a read-only page faults before any CPU commit");
  check(g_bytes[DATA + kPage - 2u] == 0x10 && g_bytes[DATA + kPage] == 0x54,
        "the faulted store wrote neither page");

  /* Write-only on that page refuses the LOAD instead, so a guard that treated
     the two permissions alike cannot pass either. */
  g_perms[kDataPage + 1] = kX86pMemWrite;
  cpu.eip = kCode + 2; /* the load alone */
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "generated load straddling into a write-only page faults");

  /* Zero is an unmapped page: refused both ways, from emitted code, with no
     trap and no partial write. */
  g_perms[kDataPage + 1] = 0u;
  cpu.eip = kCode;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "generated access straddling into an unmapped page faults");

  /*
   * Restoring the byte is enough -- NO invalidation. The guard loads the
   * permission on every access, so only MOVING the table would need a flush,
   * and proving that here is what says a VirtualProtect does not have to throw
   * translated code away.
   */
  g_perms[kDataPage + 1] = kX86pMemRead | kX86pMemWrite;
  cpu.eip = kCode;
  cpu.reg[kX86pEax] = 0x0badf00du;
  check(run(engine, &cpu) == kX86pRunBudget && cpu.reg[kX86pEdx] == 0x0badf00du,
        "the same translated block sees the restored permission without invalidation");

  /* An access wholly inside one permitted page still works, so the straddle
     handling has not broken the ordinary case. */
  cpu.eip = kCode;
  cpu.reg[kX86pEbx] = DATA + 16u;
  cpu.reg[kX86pEax] = 0x11223344u;
  check(run(engine, &cpu) == kX86pRunBudget && cpu.reg[kX86pEdx] == 0x11223344u,
        "a single-page access is unaffected");

  /* And an address past the window still faults on the bounds check, which the
     permission table must not have replaced. */
  cpu.eip = kCode;
  cpu.reg[kX86pEbx] = (uint32_t)sizeof g_bytes;
  snapshot = cpu;
  check(run(engine, &cpu) == kX86pRunMemoryFault && !memcmp(&cpu, &snapshot, sizeof cpu),
        "an address past the window still faults on bounds, not permissions");

  x86p_jit_engine_destroy(engine);
  printf("WebAssembly page permissions: %s -- %u check(s), %u failure(s)\n", failures ? "FAILED" : "PASSED", checks,
         failures);
  return failures != 0;
}
