/*
 * test_x87_fn.c -- the x87 functions, and what they leave the stack looking
 * like.
 *
 * The ARITHMETIC here needs no test: these instructions are evaluated by
 * executing the real opcode on the host's x87 unit, so comparing the result
 * against the same opcode would compare a thing with itself.
 *
 * WHAT DOES NEED ONE IS THE STACK DISCIPLINE, which is this framework's own
 * code and is where the mistakes live. FSINCOS leaves the sine in ST(0) and
 * pushes the cosine ABOVE it -- or the other way round, and both readings are
 * grammatical. FPTAN pushes a literal 1.0 that is easy to forget entirely.
 * FPATAN and FYL2X consume two registers and leave one, so a version that
 * merely overwrote ST(0) would leave the stack one deep too far and every
 * later ST(i) would name the wrong register -- silently, and only visibly
 * several instructions later.
 *
 * So this sets up an identical stack on the host and in the model, runs the
 * instruction through the FULL interpreter path -- decode included, because
 * the decoder is what says which function an opcode is -- and compares TOP and
 * the registers around it.
 */
#include "cpu.h"
#include "decode.h"
#include "exec.h"
#include "x87.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#define HAVE_ORACLE 1
#else
#define HAVE_ORACLE 0
#endif

#if HAVE_ORACLE
static unsigned long g_checks;
static unsigned long g_failed;
static unsigned long g_oracle_runs;
#endif

#define BASE 0x00030000u
#if HAVE_ORACLE
static uint8_t g_code_mem[64];
#endif

typedef struct Case {
  const char *name;
  const uint8_t *bytes;
  unsigned len;
} Case;

#define I(id, ...) static const uint8_t id[] = {__VA_ARGS__};
I(k_fsqrt, 0xD9u, 0xFAu)
I(k_fsin, 0xD9u, 0xFEu)
I(k_fcos, 0xD9u, 0xFFu)
I(k_fsincos, 0xD9u, 0xFBu)
I(k_fptan, 0xD9u, 0xF2u)
I(k_fpatan, 0xD9u, 0xF3u)
I(k_fyl2x, 0xD9u, 0xF1u)
I(k_fyl2xp1, 0xD9u, 0xF9u)
I(k_f2xm1, 0xD9u, 0xF0u)
I(k_fscale, 0xD9u, 0xFDu)
I(k_frndint, 0xD9u, 0xFCu)
I(k_fxtract, 0xD9u, 0xF4u)
I(k_fprem, 0xD9u, 0xF8u)
I(k_fprem1, 0xD9u, 0xF5u)
I(k_fabs, 0xD9u, 0xE1u)
I(k_fchs, 0xD9u, 0xE0u)

#define C(n, b) {n, b, (unsigned)(sizeof b)}
static const Case kCases[] = {
    C("FSQRT", k_fsqrt),
    C("FSIN", k_fsin),
    C("FCOS", k_fcos),
    C("FSINCOS", k_fsincos),
    C("FPTAN", k_fptan),
    C("FPATAN", k_fpatan),
    C("FYL2X", k_fyl2x),
    C("FYL2XP1", k_fyl2xp1),
    C("F2XM1", k_f2xm1),
    C("FSCALE", k_fscale),
    C("FRNDINT", k_frndint),
    C("FXTRACT", k_fxtract),
    C("FPREM", k_fprem),
    C("FPREM1", k_fprem1),
    C("FABS", k_fabs),
    C("FCHS", k_fchs),
};

/* ST(0), ST(1). Chosen so every function has a defined, ordinary answer:
   positive arguments for the logarithms and the square root, a value inside
   [-1,1] for F2XM1, and an angle small enough that the trigonometric
   reduction succeeds rather than setting C2. */
static const long double kInputs[][2] = {
    {0.5L, 2.0L},
    {1.5L, 3.0L},
    {0.25L, 1.0L},
    {2.0L, 0.5L},
    {0.75L, 8.0L},
};

#if HAVE_ORACLE
typedef struct HostOut {
  long double st0;
  long double st1;
  unsigned top; /* how far TOP moved: 0 unchanged, 1 pushed, -1 as 7 */
} HostOut;

static uint8_t *g_page;

/*
 * Run the instruction on a host stack holding, from the top down, a, b and a
 * sentinel.
 *
 * TOP is read from FNSTSW BEFORE anything is stored, because storing changes
 * it. The sentinel is there so that an instruction which pops leaves a
 * register this test can still read without underflowing the host's stack --
 * an underflow would put a QNaN in the result and look like an arithmetic
 * difference rather than a stack one.
 */
static void oracle(const Case *c, long double a, long double b, HostOut *out) {
  static long double in[3];
  static long double res[2];
  static uint16_t sw_before, sw_after;
  unsigned n = 0;
  void (*fn)(void);

  if (!g_page) {
    g_page = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_page == MAP_FAILED) {
      g_page = NULL;
      g_failed++;
      return;
    }
  }
  in[0] = -12345.0L; /* sentinel */
  in[1] = b;
  in[2] = a;

#define EMIT(...)                                                                                                      \
  do {                                                                                                                 \
    static const uint8_t bytes__[] = {__VA_ARGS__};                                                                    \
    memcpy(g_page + n, bytes__, sizeof bytes__);                                                                       \
    n += (unsigned)sizeof bytes__;                                                                                     \
  } while (0)

  /* finit; fldt in[0]; fldt in[1]; fldt in[2] -- absolute addresses reach the
     statics, which is why they are static rather than automatic. */
  EMIT(0x9Bu, 0xDBu, 0xE3u); /* FINIT */
  {
    unsigned k;
    for (k = 0; k < 3u; k++) {
      uint64_t addr = (uint64_t)(uintptr_t)&in[k];
      /* mov rax, imm64 ; fldt [rax] */
      g_page[n++] = 0x48u;
      g_page[n++] = 0xB8u;
      memcpy(g_page + n, &addr, 8);
      n += 8u;
      EMIT(0xDBu, 0x28u);
    }
  }
  memcpy(g_page + n, c->bytes, c->len);
  n += c->len;
  /* fnstsw [sw_before] */
  {
    uint64_t addr = (uint64_t)(uintptr_t)&sw_before;
    g_page[n++] = 0x48u;
    g_page[n++] = 0xB8u;
    memcpy(g_page + n, &addr, 8);
    n += 8u;
    EMIT(0xDDu, 0x38u); /* FNSTSW word [rax] */
  }
  /* fstpt res[0] ; fstpt res[1] */
  {
    unsigned k;
    for (k = 0; k < 2u; k++) {
      uint64_t addr = (uint64_t)(uintptr_t)&res[k];
      g_page[n++] = 0x48u;
      g_page[n++] = 0xB8u;
      memcpy(g_page + n, &addr, 8);
      n += 8u;
      EMIT(0xDBu, 0x38u); /* FSTPT [rax] */
    }
  }
  EMIT(0x9Bu, 0xDBu, 0xE3u); /* FINIT, so the host stack is left clean */
  g_page[n++] = 0xC3u;
#undef EMIT

  sw_before = 0u;
  sw_after = 0u;
  (void)sw_after;
  memcpy(&fn, &g_page, sizeof fn);
  fn();
  out->st0 = res[0];
  out->st1 = res[1];
  /* Three values were pushed from an empty stack, so TOP is 8-3 == 5 before
     the instruction; whatever it is after says what the instruction did. */
  out->top = (unsigned)((sw_before >> 11) & 7u);
  g_oracle_runs++;
}
#endif

static void run_case(const Case *c, long double a, long double b) {
#if HAVE_ORACLE
  HostOut host;
  X86pCpu cpu;
  X86pMem mem;
  X86pStepReport rep;
  long double m0 = 0.0L;
  long double m1 = 0.0L;
  const size_t significant = 10u; /* the architectural bytes of an 80-bit value */

  memset(&host, 0, sizeof host);
  oracle(c, a, b, &host);

  memset(g_code_mem, 0x90, sizeof g_code_mem);
  memcpy(g_code_mem, c->bytes, c->len);
  mem.host = g_code_mem;
  mem.lo = BASE;
  mem.size = (uint32_t)sizeof g_code_mem;

  x86p_cpu_reset(&cpu);
  cpu.eip = BASE;
  /* The same three values, pushed in the same order. */
  x86p_x87_push(&cpu.x87, -12345.0L);
  x86p_x87_push(&cpu.x87, b);
  x86p_x87_push(&cpu.x87, a);

  g_checks++;
  if (x86p_step(&cpu, &mem, &rep) != kX86pStepOk) {
    g_failed++;
    printf("    FAIL %s: the model refused (%s)\n", c->name, rep.mnemonic ? rep.mnemonic : "?");
    return;
  }

  g_checks++;
  if (cpu.x87.top != host.top) {
    g_failed++;
    printf("    FAIL %s: TOP cpu=%u model=%u -- the stack effect differs\n", c->name, host.top, cpu.x87.top);
    return;
  }

  (void)x86p_x87_get(&cpu.x87, 0, &m0);
  (void)x86p_x87_get(&cpu.x87, 1, &m1);
  g_checks++;
  if (memcmp(&m0, &host.st0, significant) != 0) {
    g_failed++;
    printf("    FAIL %s(%Lg, %Lg): ST(0) cpu=%.20Lg model=%.20Lg\n", c->name, a, b, host.st0, m0);
  }
  g_checks++;
  if (memcmp(&m1, &host.st1, significant) != 0) {
    g_failed++;
    printf("    FAIL %s(%Lg, %Lg): ST(1) cpu=%.20Lg model=%.20Lg\n", c->name, a, b, host.st1, m1);
  }
#else
  (void)c;
  (void)a;
  (void)b;
#endif
}

int main(void) {
  unsigned i;
  unsigned v;
  printf("test the x87 functions and their stack effects against the host CPU\n");
  for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    for (v = 0; v < sizeof kInputs / sizeof kInputs[0]; v++) {
      run_case(&kCases[i], kInputs[v][0], kInputs[v][1]);
    }
  }
#if HAVE_ORACLE
  if (g_oracle_runs == 0u) {
    printf("REFUSED: the host oracle never ran, so nothing here was verified\n");
    return 1;
  }
  printf(
      "%lu check(s), %lu failure(s); %lu instruction(s) executed on the host CPU\n", g_checks, g_failed, g_oracle_runs);
  return g_failed ? 1 : 0;
#else
  printf("REFUSED: this host is not x86-64, so nothing here was verified.\n");
  return 1;
#endif
}
