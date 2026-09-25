/*
 * jit_x87_bench -- what one translated x87 instruction costs, per instruction.
 *
 * AN INSTRUMENT, NOT A TEST. It gates nothing. It times one block of ordinary
 * game-shaped x87 code -- float loads, arithmetic against memory and
 * registers, an exchange, a sign change, a compare and a store -- through the
 * backend this build has, with the consumer's double-arithmetic mode off and
 * on, and prints nanoseconds per guest x87 instruction.
 *
 * Before any column is timed the block is run once against the interpreter
 * from the same state and the whole CPU compared, so a number is never the
 * cost of a different program. The minimum of several repetitions is
 * reported with the maximum beside it, for jit_bench.c's reasons.
 */
#include "cpu.h"
#include "cpu_compare.h"
#include "exec.h"
#include "jit_storage.h"
#include "jit_x64.h"
#include "x87.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 4096u
#define DATA_OFF 0x800u
#define REPS 5

static uint8_t g_guest[GUEST_SIZE];
static X86pCpu g_cpu;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* One group: ten x87 instructions over [EBX+d8], leaving the stack as it
   found it. */
static const uint8_t kGroup[] = {
    0xD9, 0x43, 0x00, /* fld dword [ebx]        */
    0xD8, 0x4B, 0x04, /* fmul dword [ebx+4]     */
    0xDC, 0x43, 0x08, /* fadd qword [ebx+8]     */
    0xD9, 0x43, 0x04, /* fld dword [ebx+4]      */
    0xD8, 0xC9,       /* fmul st0, st1          */
    0xD9, 0xC9,       /* fxch st1               */
    0xD9, 0xE0,       /* fchs                   */
    0xDE, 0xE9,       /* fsubp st1, st0         */
    0xD8, 0x53, 0x00, /* fcom dword [ebx]       */
    0xD9, 0x5B, 0x10, /* fstp dword [ebx+16]    */
};
#define GROUP_X87 10u
#define GROUPS 6u

static uint32_t build_kernel(void) {
  uint32_t g;
  memset(g_guest, 0x90, sizeof g_guest);
  for (g = 0; g < GROUPS; g++) {
    memcpy(g_guest + g * sizeof kGroup, kGroup, sizeof kGroup);
  }
  {
    const float a = 1.5f;
    const float b = 0.75f;
    const double c = 2.0;
    memcpy(g_guest + DATA_OFF, &a, sizeof a);
    memcpy(g_guest + DATA_OFF + 4u, &b, sizeof b);
    memcpy(g_guest + DATA_OFF + 8u, &c, sizeof c);
  }
  return GROUPS * GROUP_X87;
}

static void seed(X86pCpu *cpu, int double_arith) {
  x86p_cpu_reset(cpu);
  cpu->reg[kX86pEbx] = GUEST_BASE + DATA_OFF;
  cpu->reg[kX86pEsp] = GUEST_BASE + 0x700u;
  cpu->eip = GUEST_BASE;
  x86p_x87_set_double_arith(&cpu->x87, double_arith);
}

static void report_diff(const char *field, const char *a_text, const char *b_text, void *user) {
  (void)user;
  printf("  %s: interpreter %s, jit %s\n", field, a_text, b_text);
}

/* The block and the interpreter agree over one run from the same state. */
static int agrees(const X86pMem *mem, const X86pJitBlock *blk, int double_arith) {
  X86pCpu interp;
  uint32_t i;
  seed(&interp, double_arith);
  for (i = 0; i < blk->insns; i++) {
    if (x86p_step(&interp, mem, NULL) != kX86pStepOk) {
      printf("REFUSED: the interpreter could not run the kernel\n");
      return 0;
    }
  }
  seed(&g_cpu, double_arith);
  if (x86p_jit_enter(blk, &g_cpu) != kX86pJitExitBlockEnd) {
    printf("REFUSED: the block did not run to its end\n");
    return 0;
  }
  if (x86p_cpu_diff(&interp, &g_cpu, report_diff, NULL) != 0u) {
    printf("REFUSED: interpreter and jit disagree after one kernel\n");
    return 0;
  }
  return 1;
}

static int time_column(const X86pMem *mem, const X86pJitBlock *blk, uint32_t x87, unsigned long iters, int da) {
  double best = 1e30;
  double worst = 0.0;
  int rep;
  if (!agrees(mem, blk, da)) {
    return 0;
  }
  for (rep = 0; rep < REPS; rep++) {
    unsigned long i;
    double t;
    seed(&g_cpu, da);
    t = now_s();
    for (i = 0; i < iters; i++) {
      if (x86p_jit_enter(blk, &g_cpu) != kX86pJitExitBlockEnd) {
        printf("REFUSED: the block stopped early on iteration %lu\n", i);
        return 0;
      }
    }
    t = now_s() - t;
    best = t < best ? t : best;
    worst = t > worst ? t : worst;
  }
  printf("double_arith=%d: %.2f ns per x87 instruction (worst of %d: %.2f), %.1f ns per block\n",
         da,
         best * 1e9 / ((double)iters * x87),
         REPS,
         worst * 1e9 / ((double)iters * x87),
         best * 1e9 / (double)iters);
  return 1;
}

int main(int argc, char **argv) {
  X86pMem mem = {0};
  X86pJitBlock blk;
  X86pJitStorage *storage;
  char reason[256];
  unsigned long iters = 200000ul;
  uint32_t x87;
  X86pJitStatus st;

  if (argc > 1) {
    iters = strtoul(argv[1], NULL, 10);
  }
  if (!x86p_jit_available()) {
    printf("REFUSED: no native JIT backend in this build\n");
    return 1;
  }
  x87 = build_kernel();
  mem.host = g_guest;
  mem.lo = GUEST_BASE;
  mem.size = GUEST_SIZE;
  storage = x86p_jit_storage_create(65536u, 16u, reason, sizeof reason);
  if (!storage) {
    printf("REFUSED: storage -> %s\n", reason);
    return 1;
  }
  st = x86p_jit_storage_translate(storage, &mem, GUEST_BASE, NULL, &blk, reason, sizeof reason);
  if (st != kX86pJitOk) {
    printf("REFUSED: translate -> %s (%s)\n", x86p_jit_status_name(st), reason);
    return 1;
  }
  printf("block: %u guest instruction(s), %u of them x87, %zu host byte(s)\n", blk.insns, x87, blk.host_bytes);
  if (blk.insns < x87) {
    printf("REFUSED: the block stopped at %s before the kernel's end\n", blk.stopper ? blk.stopper : "?");
    return 1;
  }
  if (!time_column(&mem, &blk, x87, iters, 0) || !time_column(&mem, &blk, x87, iters, 1)) {
    return 1;
  }
  x86p_jit_storage_destroy(storage);
  return 0;
}
