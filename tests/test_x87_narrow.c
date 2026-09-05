/* Store conversion results and host rounding-state preservation. */
#include "x87.h"
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      failures++;                                                                                                      \
      printf("FAIL line %d: %s\n", __LINE__, #c);                                                                      \
    }                                                                                                                  \
  } while (0)

static void controls(void) {
  static const int host_modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
  static const long double values[] = {
      0x1.000001p0L, -0x1.000001p0L, 0x1p-150L, -0x1p-150L, 0x1.ffffffp127L, -0x1.ffffffp127L};
  static const uint32_t expected[][4] = {{0x3f800000, 0x3f800000, 0x3f800001, 0x3f800000},
                                         {0xbf800000, 0xbf800001, 0xbf800000, 0xbf800000},
                                         {0, 0, 1, 0},
                                         {0x80000000, 0x80000001, 0x80000000, 0x80000000},
                                         {0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff},
                                         {0xff800000, 0xff800000, 0xff7fffff, 0xff7fffff}};
  const int saved = fegetround();
  X86pX87 f;
  x86p_x87_reset(&f);
  for (unsigned host = 0; host < 4; host++) {
    CHECK(fesetround(host_modes[host]) == 0);
    for (unsigned guest = 0; guest < 4; guest++) {
      f.control = (uint16_t)(0x37F | (guest << 10));
      for (unsigned v = 0; v < 6; v++) {
        feraiseexcept(FE_DIVBYZERO);
        CHECK(x86p_x87_to_f32(&f, values[v]) == expected[v][guest]);
        CHECK(fegetround() == host_modes[host]);
        CHECK((fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO) != 0);
      }
      CHECK(x86p_x87_to_f64(&f, 0x1.0000000000001p0L) == UINT64_C(0x3ff0000000000001));
      CHECK(x86p_x87_to_f64(&f, -0.0L) == UINT64_C(0x8000000000000000));
      CHECK(fegetround() == host_modes[host]);
    }
  }
  CHECK(fesetround(saved) == 0);
}
static void benchmark(void) {
  const unsigned iterations = 10000000;
  X86pX87 f;
  x86p_x87_reset(&f);
  volatile uint64_t checksum = 0;
  for (unsigned width = 0; width < 2; width++) {
    const clock_t begin = clock();
    for (unsigned i = 0; i < iterations; i++) {
      checksum += width ? x86p_x87_to_f64(&f, 0x1.000001p0L) : x86p_x87_to_f32(&f, 0x1.000001p0L);
    }
    printf("%u stores to f%u: %.2f ns/store (checksum %llu)\n",
           iterations,
           width ? 64 : 32,
           (double)(clock() - begin) * 1e9 / CLOCKS_PER_SEC / iterations,
           (unsigned long long)checksum);
  }
}
int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
    benchmark();
    return 0;
  }
  controls();
  printf("%u narrowing checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
