/* REP MOVS against independent one-element stepping, plus fast-span controls. */
#include "cpu.h"
#include "cpu_compare.h"
#include "string_ops.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures, observed;
static uint32_t observed_hash;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      failures++;                                                                                                      \
      printf("FAIL line %d: %s\n", __LINE__, #c);                                                                      \
    }                                                                                                                  \
  } while (0)
static void observer(uint32_t addr, uint32_t len, void *user) {
  (void)user;
  observed++;
  observed_hash = (observed_hash * 31u + addr) * 31u + len;
}
static X86pStringStatus reference(X86pCpu *cpu, const X86pMem *mem, X86pInsn in, uint32_t *fault) {
  in.rep = kX86pRepNone;
  *fault = 0;
  while (cpu->reg[kX86pEcx]) {
    X86pStringStatus status = x86p_string_execute(cpu, mem, &in, fault);
    if (status != kX86pStringOk) {
      return status;
    }
    cpu->reg[kX86pEcx]--;
  }
  return kX86pStringOk;
}
static void differential(void) {
  static const unsigned counts[] = {0, 1, 2, 3, 8, 31, 64, 129, UINT32_MAX};
  static const unsigned offsets[][2] = {{32, 512},
                                        {512, 32},
                                        {32, 32},
                                        {32, 33},
                                        {33, 32},
                                        {32, 34},
                                        {34, 32},
                                        {32, 35},
                                        {35, 32},
                                        {1000, 0},
                                        {0, 1000},
                                        {1023, 512},
                                        {512, 1023}};
  uint8_t actual[1024], expected[1024];
  for (unsigned width = 1; width <= 4; width *= 2) {
    for (unsigned df = 0; df < 2; df++) {
      for (unsigned count = 0; count < sizeof counts / sizeof counts[0]; count++) {
        for (unsigned pos = 0; pos < sizeof offsets / sizeof offsets[0]; pos++) {
          for (unsigned watch = 0; watch < 2; watch++) {
            for (unsigned high = 0; high < 2; high++) {
              for (unsigned i = 0; i < sizeof actual; i++) {
                actual[i] = expected[i] = (uint8_t)(i * 37);
              }
              const uint32_t base = high ? 0xFFFFFE00u : 0x10000u;
              X86pMem am = {.host = actual, .lo = base, .size = sizeof actual}, em = am;
              em.host = expected;
              X86pCpu a, b;
              x86p_cpu_reset(&a);
              a.reg[kX86pEsi] = base + offsets[pos][0];
              a.reg[kX86pEdi] = base + offsets[pos][1];
              a.reg[kX86pEcx] = counts[count];
              a.df = (uint8_t)df;
              x86p_flags_set_explicit(&a.flags, 0xAD7);
              b = a;
              X86pInsn in = {0};
              in.str = kX86pStringMovs;
              in.rep = kX86pRepRep;
              in.str_width = (uint8_t)width;
              uint32_t af, bf;
              x86p_mem_set_write_observer(watch ? observer : NULL, NULL);
              observed = 0;
              observed_hash = 0;
              X86pStringStatus as = x86p_string_execute(&a, &am, &in, &af);
              unsigned aw = observed;
              uint32_t ah = observed_hash;
              observed = 0;
              observed_hash = 0;
              X86pStringStatus bs = reference(&b, &em, in, &bf);
              CHECK(as == bs && af == bf);
              CHECK(x86p_cpu_diff(&a, &b, NULL, NULL) == 0);
              CHECK(memcmp(actual, expected, sizeof actual) == 0);
              CHECK(aw == observed && ah == observed_hash);
              x86p_mem_set_write_observer(NULL, NULL);
            }
          }
        }
      }
    }
  }
}
static void admission(void) {
  uint8_t data[32], saved[32];
  for (unsigned i = 0; i < sizeof data; i++) {
    data[i] = (uint8_t)i;
  }
  X86pMem mem = {.host = data, .lo = 0x10000, .size = sizeof data};
  CHECK(x86p_mem_copy_disjoint(&mem, mem.lo + 16, mem.lo, 16));
  CHECK(memcmp(data, data + 16, 16) == 0);
  memcpy(saved, data, sizeof data);
  CHECK(!x86p_mem_copy_disjoint(NULL, mem.lo + 16, mem.lo, 16));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo + 16, mem.lo, 0));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo + 16, mem.lo, 17));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo, mem.lo + 16, 17));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo + 1, mem.lo, 16));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo, mem.lo + 1, 16));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo, mem.lo, 16));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo - 1, mem.lo, 16));
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo, mem.lo - 1, 16));
  observed = 0;
  x86p_mem_set_write_observer(observer, NULL);
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo + 16, mem.lo, 16));
  CHECK(observed == 0);
  x86p_mem_set_write_observer(NULL, NULL);
  mem.lo = UINT32_MAX - 15;
  CHECK(!x86p_mem_copy_disjoint(&mem, mem.lo + 8, mem.lo, 16));
  CHECK(memcmp(data, saved, sizeof data) == 0);
}

static void benchmark(void) {
  uint8_t data[8192] = {0};
  X86pMem mem = {.host = data, .lo = 0x10000, .size = sizeof data};
  X86pInsn in = {0};
  in.str = kX86pStringMovs;
  in.rep = kX86pRepRep;
  in.str_width = 4;
  X86pCpu cpu;
  x86p_cpu_reset(&cpu);
  const unsigned repetitions = 100000;
  clock_t start = clock();
  unsigned completed = 0;
  for (unsigned i = 0; i < repetitions; i++) {
    cpu.reg[kX86pEsi] = mem.lo;
    cpu.reg[kX86pEdi] = mem.lo + 4096;
    cpu.reg[kX86pEcx] = 1024;
    completed += x86p_string_execute(&cpu, &mem, &in, NULL) == kX86pStringOk;
  }
  printf("%u/%u REP MOVSD copies of 4096 bytes: %.1f ns/copy\n",
         completed,
         repetitions,
         (double)(clock() - start) * 1e9 / CLOCKS_PER_SEC / repetitions);
}
int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
    benchmark();
    return 0;
  }
  admission();
  differential();
  printf("%u copy state checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
