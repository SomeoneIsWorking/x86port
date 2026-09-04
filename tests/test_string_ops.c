/*
 * test_string_ops.c -- the string operations, checked against the silicon.
 *
 * These have more rules than they look like they have: a stride whose SIGN
 * comes from DF and whose MAGNITUDE is the element width, two pointers of
 * which each operation advances a different subset, a repeat that tests ECX
 * before the first pass, and -- for SCAS and CMPS -- a ZF test that happens
 * after the decrement. Reading the manual gets most of that right, which is
 * the problem: the ones it gets wrong are exactly the ones no synthetic test
 * thinks to cover.
 *
 * So this EXECUTES each instruction on the host CPU with a controlled starting
 * state and compares the whole outcome: both buffers, ESI, EDI, ECX, EAX, and
 * the real EFLAGS word. The host is a 64-bit CPU running the same instruction
 * with 64-bit pointers, so the POINTERS are compared as deltas rather than as
 * values -- everything else is compared literally.
 *
 * On a non-x86 host it says loudly that no oracle ran rather than passing on
 * the hermetic cases alone.
 */
#include "cpu.h"
#include "decode.h"
#include "flags.h"
#include "string_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#define HAVE_ORACLE 1
#else
#define HAVE_ORACLE 0
#endif

static unsigned long g_failed;
#if HAVE_ORACLE
static unsigned long g_checks;
static unsigned long g_oracle_runs;
#endif

#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(c)) {                                                                                                        \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                          \
    }                                                                                                                  \
  } while (0)

#define BUF 512u
#define GUEST_BASE 0x00020000u
/* The two guest regions, far enough apart that a stride bug cannot make one
   overlap the other and appear to work. */
/* Placed so that the longest run this test uses stays inside the mapping in
   BOTH directions: a backward run from too low an offset would leave the
   guest mapping and be refused by the model while the host, which has no
   mapping, walked happily past it -- a harness limit reported as a defect. */
#define SRC_OFF 0x80u
#define DST_OFF 0x140u

#if HAVE_ORACLE
static uint8_t g_guest[BUF];
#endif

typedef struct Outcome {
  uint8_t mem[BUF];
  int32_t esi_delta;
  int32_t edi_delta;
  uint32_t ecx;
  uint32_t eax;
  uint32_t eflags; /* the six arithmetic flags only */
} Outcome;

#if HAVE_ORACLE
/*
 * Run the real instruction.
 *
 * `opcode` is the one-byte string opcode and `prefix` the repeat prefix byte
 * (0 for none). The instruction is assembled into a small buffer and CALLED,
 * rather than written as inline asm per case, because fifteen nearly identical
 * asm blocks is fifteen chances to typo one of them into a different
 * instruction that still assembles.
 */
static void oracle(uint8_t opcode,
                   uint8_t prefix,
                   int operand16,
                   int df,
                   uint8_t *src,
                   uint8_t *dst,
                   uint32_t ecx,
                   uint32_t eax,
                   Outcome *out) {
  uint64_t rsi = (uint64_t)(uintptr_t)src;
  uint64_t rdi = (uint64_t)(uintptr_t)dst;
  uint64_t rsi_out = 0;
  uint64_t rdi_out = 0;
  uint64_t rcx_out = 0;
  uint64_t rax_out = 0;
  uint64_t flags_out = 0;
  uint64_t ecx64 = ecx;
  uint64_t eax64 = eax;
  uint64_t df64 = (df != 0);
  /*
   * prefix? + 0x66? + opcode, then RET, assembled into an executable page.
   *
   * Assembled rather than written as fifteen inline-asm blocks: fifteen nearly
   * identical blocks is fifteen chances to typo one into a different
   * instruction that still assembles, and the typo would be invisible -- the
   * test would compare the model against the wrong oracle and agree.
   */
  static uint8_t *code;
  unsigned n = 0;

  if (!code) {
    code = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
      printf("    REFUSED: no executable page for the oracle\n");
      g_failed++;
      code = NULL;
      return;
    }
  }
  if (prefix) {
    code[n++] = prefix;
  }
  if (operand16) {
    code[n++] = 0x66u;
  }
  code[n++] = opcode;
  code[n++] = 0xC3u; /* RET */

  /*
   * Memory operands throughout, not registers: eleven register operands is
   * more than the constraint allocator has, and it refuses rather than
   * silently reusing one. Nothing here touches memory between the PUSHFQ and
   * the POP, which is the only window where an RSP-relative operand would be
   * displaced.
   *
   * DF is set before the call and cleared after, with no flag save/restore:
   * the ABI guarantees DF is clear on entry, so restoring it means clearing
   * it. CLD does not touch the six flags being captured.
   */
  __asm__ volatile(
      "movq %[isi], %%rsi\n\t"
      "movq %[idi], %%rdi\n\t"
      "movq %[icx], %%rcx\n\t"
      "movq %[iax], %%rax\n\t"
      "cmpq $0, %[df]\n\t"
      "je 1f\n\t"
      "std\n\t"
      "1:\n\t"
      "callq *%[code]\n\t"
      "cld\n\t"
      "movq %%rsi, %[osi]\n\t"
      "movq %%rdi, %[odi]\n\t"
      "movq %%rcx, %[ocx]\n\t"
      "movq %%rax, %[oax]\n\t"
      "pushfq\n\t"
      "popq %%rax\n\t"
      "movq %%rax, %[ofl]\n\t"
      : [osi] "=m"(rsi_out), [odi] "=m"(rdi_out), [ocx] "=m"(rcx_out), [oax] "=m"(rax_out), [ofl] "=m"(flags_out)
      : [isi] "m"(rsi), [idi] "m"(rdi), [icx] "m"(ecx64), [iax] "m"(eax64), [df] "m"(df64), [code] "m"(code)
      : "rsi", "rdi", "rcx", "rax", "memory", "cc");

  out->esi_delta = (int32_t)(int64_t)(rsi_out - rsi);
  out->edi_delta = (int32_t)(int64_t)(rdi_out - rdi);
  out->ecx = (uint32_t)rcx_out;
  out->eax = (uint32_t)rax_out;
  out->eflags = (uint32_t)flags_out & X86P_ARITH_FLAGS;
  g_oracle_runs++;
}
#endif /* HAVE_ORACLE */

#if HAVE_ORACLE
static uint32_t model_eflags(const X86pCpu *cpu) {
  return x86p_eflags(&cpu->flags) & X86P_ARITH_FLAGS;
}

/* A deterministic, boundary-crossing fill: a comparison that never sees equal
   bytes cannot tell REPE from REPNE. */
static void fill(uint8_t *p, size_t n, unsigned seed) {
  size_t i;
  for (i = 0; i < n; i++) {
    p[i] = (uint8_t)((seed * 37u + i * 17u + (i / 4u)) & 0xFFu);
  }
}
#endif

typedef struct Case {
  const char *name;
  uint8_t opcode;
  int operand16;
  X86pStringOp op;
  int width;
} Case;

static const Case kCases[] = {
    {"MOVSB", 0xA4u, 0, kX86pStringMovs, 1},
    {"MOVSW", 0xA5u, 1, kX86pStringMovs, 2},
    {"MOVSD", 0xA5u, 0, kX86pStringMovs, 4},
    {"STOSB", 0xAAu, 0, kX86pStringStos, 1},
    {"STOSW", 0xABu, 1, kX86pStringStos, 2},
    {"STOSD", 0xABu, 0, kX86pStringStos, 4},
    {"LODSB", 0xACu, 0, kX86pStringLods, 1},
    {"LODSW", 0xADu, 1, kX86pStringLods, 2},
    {"LODSD", 0xADu, 0, kX86pStringLods, 4},
    {"SCASB", 0xAEu, 0, kX86pStringScas, 1},
    {"SCASW", 0xAFu, 1, kX86pStringScas, 2},
    {"SCASD", 0xAFu, 0, kX86pStringScas, 4},
    {"CMPSB", 0xA6u, 0, kX86pStringCmps, 1},
    {"CMPSW", 0xA7u, 1, kX86pStringCmps, 2},
    {"CMPSD", 0xA7u, 0, kX86pStringCmps, 4},
};

static void run_case(const Case *c, X86pRepKind rep, int df, uint32_t ecx, uint32_t eax, unsigned seed) {
#if HAVE_ORACLE
  static uint8_t host[BUF];
  Outcome real;
  X86pCpu cpu;
  X86pMem mem;
  X86pInsn insn;
  uint32_t esi0 = GUEST_BASE + SRC_OFF;
  uint32_t edi0 = GUEST_BASE + DST_OFF;
  uint8_t prefix = (rep == kX86pRepRep) ? 0xF3u : (rep == kX86pRepRepne ? 0xF2u : 0x00u);

  fill(host, sizeof host, seed);
  memcpy(g_guest, host, sizeof host);

  oracle(c->opcode, prefix, c->operand16, df, host + SRC_OFF, host + DST_OFF, ecx, eax, &real);
  memcpy(real.mem, host, sizeof host);

  memset(&insn, 0, sizeof insn);
  insn.op = (uint8_t)kX86pInsnString;
  insn.str = (uint8_t)c->op;
  insn.rep = (uint8_t)rep;
  insn.str_width = (uint8_t)c->width;

  x86p_cpu_reset(&cpu);
  cpu.df = (uint8_t)(df != 0);
  cpu.reg[kX86pEsi] = esi0;
  cpu.reg[kX86pEdi] = edi0;
  cpu.reg[kX86pEcx] = ecx;
  cpu.reg[kX86pEax] = eax;
  mem.host = g_guest;
  mem.lo = GUEST_BASE;
  mem.size = (uint32_t)sizeof g_guest;

  g_checks++;
  if (x86p_string_execute(&cpu, &mem, &insn, NULL) != kX86pStringOk) {
    g_failed++;
    printf("    FAIL %s rep=%d df=%d ecx=%u: model refused\n", c->name, (int)rep, df, ecx);
    return;
  }

  if (memcmp(real.mem, g_guest, sizeof g_guest) != 0) {
    size_t i;
    g_failed++;
    for (i = 0; i < sizeof g_guest; i++) {
      if (real.mem[i] != g_guest[i]) {
        printf("    FAIL %s rep=%d df=%d ecx=%u: memory[%zu] cpu=%02X model=%02X\n",
               c->name,
               (int)rep,
               df,
               ecx,
               i,
               real.mem[i],
               g_guest[i]);
        break;
      }
    }
    return;
  }
  g_checks++;

#define SAME(what, a, b)                                                                                               \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if ((a) != (b)) {                                                                                                  \
      g_failed++;                                                                                                      \
      printf("    FAIL %s rep=%d df=%d ecx=%u: %s cpu=%08X model=%08X\n",                                              \
             c->name,                                                                                                  \
             (int)rep,                                                                                                 \
             df,                                                                                                       \
             ecx,                                                                                                      \
             (what),                                                                                                   \
             (unsigned)(a),                                                                                            \
             (unsigned)(b));                                                                                           \
    }                                                                                                                  \
  } while (0)
  SAME("ESI delta", (uint32_t)real.esi_delta, cpu.reg[kX86pEsi] - esi0);
  SAME("EDI delta", (uint32_t)real.edi_delta, cpu.reg[kX86pEdi] - edi0);
  SAME("ECX", real.ecx, cpu.reg[kX86pEcx]);
  SAME("EAX", real.eax, cpu.reg[kX86pEax]);
  /* Flags only where the instruction writes them. MOVS, STOS and LODS write
     none at all, and the host's leftover flags are not something to match. */
  if ((c->op == kX86pStringScas || c->op == kX86pStringCmps) && !(rep != kX86pRepNone && ecx == 0u)) {
    /* A repeat with ECX == 0 executes NOTHING, so the flags are whatever they
       already were -- on the host, this harness's own leftovers. There is no
       architectural value to compare against. */
    SAME("EFLAGS", real.eflags, model_eflags(&cpu));
  }
#undef SAME
#else
  (void)c;
  (void)rep;
  (void)df;
  (void)ecx;
  (void)eax;
  (void)seed;
#endif
}

int main(void) {
  unsigned i;
  static const uint32_t kCounts[] = {0u, 1u, 2u, 5u, 17u};
  static const uint32_t kEax[] = {0x00000000u, 0x000000FFu, 0x37262524u, 0xFFFFFFFFu};

  printf("test string operations against the host CPU\n");
  for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    unsigned df;
    for (df = 0; df < 2u; df++) {
      unsigned ci;
      unsigned ai;
      for (ai = 0; ai < sizeof kEax / sizeof kEax[0]; ai++) {
        /* No prefix: one pass, whatever ECX holds. */
        run_case(&kCases[i], kX86pRepNone, (int)df, 3u, kEax[ai], 1u + i + df);
      }
      for (ci = 0; ci < sizeof kCounts / sizeof kCounts[0]; ci++) {
        for (ai = 0; ai < sizeof kEax / sizeof kEax[0]; ai++) {
          run_case(&kCases[i], kX86pRepRep, (int)df, kCounts[ci], kEax[ai], 2u + i + df + ci);
          if (kCases[i].op == kX86pStringScas || kCases[i].op == kX86pStringCmps) {
            run_case(&kCases[i], kX86pRepRepne, (int)df, kCounts[ci], kEax[ai], 3u + i + df + ci);
          }
        }
      }
    }
  }

#if HAVE_ORACLE
  if (g_oracle_runs == 0u) {
    printf("REFUSED: the host oracle never ran, so nothing here was verified\n");
    return 1;
  }
  printf(
      "%lu check(s), %lu failure(s); %lu instruction(s) executed on the host CPU\n", g_checks, g_failed, g_oracle_runs);
#else
  printf("REFUSED: this host is not x86-64, so no instruction was executed and\n"
         "nothing here was verified. The string operations are UNCHECKED on this build.\n");
  return 1;
#endif
  return g_failed ? 1 : 0;
}
