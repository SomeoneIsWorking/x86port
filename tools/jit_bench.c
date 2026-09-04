/*
 * jit_bench -- how fast is translated code, against the two things that bound it?
 *
 * AN INSTRUMENT, NOT A TEST. It gates nothing. Its whole job is to produce a
 * number that can be argued with, because "the JIT is faster" without a
 * denominator is not a claim anyone can act on.
 *
 * THREE ENGINES, BECAUSE TWO WOULD NOT SAY ANYTHING USEFUL.
 *
 *   - The INTERPRETER is the floor. It is the correctness authority and the
 *     thing the JIT has to beat to justify existing at all.
 *   - NATIVE C is the ceiling. It performs the same operations on the same
 *     values without guest bookkeeping. The native column is therefore the
 *     lower-bound control the JIT is required to approach, and the JIT/native ratio is the real
 *     score. A JIT that beats the interpreter by 10x and is 40x off native has
 *     not met the bar.
 *   - The JIT is what is being measured.
 *
 * The native column is deliberately NOT a faithful emulation -- it does not
 * maintain guest flags, because compiled C does not either. That is the point:
 * it measures what the operations cost when nothing is bookkeeping for a guest
 * machine, which is the budget the translator is spending against.
 *
 * WHAT THIS DOES NOT MEASURE. A straight-line arithmetic kernel. No memory
 * traffic, no indirect branches, no cache pressure from a real game's working
 * set, no translation cost amortised over a real execution profile. It is the
 * best case for a translator, so treat the ratio as an upper bound on how well
 * things are going, never as a frame-rate prediction.
 */
#include "cpu.h"
#include "exec.h"
#include "flags.h"
#include "jit_x64.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 4096u
#define CODE_SIZE 65536u

static uint8_t g_guest[GUEST_SIZE];

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*
 * The kernel: a fixed, straight-line sequence of 32-bit register ALU and MOV.
 *
 * Hand-written rather than generated so all three engines provably do the same
 * work and the native column can be written to match instruction for
 * instruction. Registers are reused densely, which is what real compiled code
 * looks like and what makes register allocation matter.
 */
static uint32_t build_kernel(void) {
  uint8_t *p = g_guest;
  uint32_t n = 0;
  int i;

#define ALU_RR(op, dst, src)                                                                                           \
  do {                                                                                                                 \
    *p++ = (uint8_t)(((op) << 3) | 1u);                                                                                \
    *p++ = (uint8_t)(0xC0u | ((src) << 3) | (dst));                                                                    \
    n++;                                                                                                               \
  } while (0)
#define MOV_RR(dst, src)                                                                                               \
  do {                                                                                                                 \
    *p++ = 0x89u;                                                                                                      \
    *p++ = (uint8_t)(0xC0u | ((src) << 3) | (dst));                                                                    \
    n++;                                                                                                               \
  } while (0)

  for (i = 0; i < 8; i++) {
    ALU_RR(0, 0, 1); /* add eax, ecx */
    ALU_RR(6, 2, 3); /* xor edx, ebx */
    ALU_RR(5, 1, 6); /* sub ecx, esi */
    ALU_RR(4, 3, 0); /* and ebx, eax */
    MOV_RR(6, 2);    /* mov esi, edx */
    ALU_RR(1, 7, 5); /* or  edi, ebp */
    ALU_RR(0, 5, 4); /* add ebp, esp */
    ALU_RR(6, 4, 7); /* xor esp, edi */
  }
#undef ALU_RR
#undef MOV_RR
  return n;
}

/* The same operations in C, with no guest bookkeeping. Kept literally
   parallel to build_kernel above. */
static void native_kernel(uint32_t *r) {
  int i;
  for (i = 0; i < 8; i++) {
    r[0] += r[1];
    r[2] ^= r[3];
    r[1] -= r[6];
    r[3] &= r[0];
    r[6] = r[2];
    r[7] |= r[5];
    r[5] += r[4];
    r[4] ^= r[7];
  }
}

/*
 * The same operations, maintaining guest flags INLINE -- the like-for-like
 * control, and the number the JIT is required to approach.
 *
 * This deliberately does NOT call x86p_flags_set. An earlier version did, and
 * it made the JIT look 3.4x faster than this control. That was measuring a
 * cross-translation-unit call the JIT had learned to inline and the control had
 * not. The carry_in derivations below are therefore specialised per site,
 * matching what the JIT emits instruction for instruction.
 *
 * Beating a baseline that was handicapped is worse than having no baseline,
 * because it produces a number that sounds like success.
 */
static void native_kernel_flags(X86pCpu *c) {
  uint32_t *r = c->reg;
  /*
   * VOLATILE, and the reason is the whole credibility of this column.
   *
   * Without it the C compiler deletes every flag store in this kernel, because
   * nothing here reads them back -- and the baseline measured 0.05 ns/insn,
   * identical to doing no flag work at all. That is not a fair target for a
   * translator that stores them: it is a different program.
   *
   * So this measures the cost of ACTUALLY PERFORMING the same stores the JIT
   * performs, which is the like-for-like comparison. The elimination the
   * compiler wanted to do is real and valuable, but it is a SEPARATE advantage
   * (dead flag-write elimination), and folding it silently into this number
   * would hide it instead of costing it out. The JIT does not do it yet; see
   * the note printed below.
   */
  volatile X86pFlags *f = &c->flags;
  int i;
  for (i = 0; i < 8; i++) {
    uint32_t a;
    uint32_t b;
    uint32_t v;
    /* `cin` is the CF the PREVIOUS operation implies, derived from its
       statically known kind -- Add: r<a, Sub: a<b, Logic: 0. */
#define OPF(dst, src, expr, kk, cin)                                                                                   \
  do {                                                                                                                 \
    f->carry_in = (uint8_t)(cin);                                                                                      \
    a = r[dst];                                                                                                        \
    b = r[src];                                                                                                        \
    v = (expr);                                                                                                        \
    f->kind = (uint8_t)(kk);                                                                                           \
    f->a = a;                                                                                                          \
    f->b = b;                                                                                                          \
    f->r = v;                                                                                                          \
    f->w = 4u;                                                                                                         \
    r[dst] = v;                                                                                                        \
  } while (0)
#define OPF_NODST(dst, src, expr, kk, cin)                                                                             \
  do {                                                                                                                 \
    f->carry_in = (uint8_t)(cin);                                                                                      \
    a = r[dst];                                                                                                        \
    b = r[src];                                                                                                        \
    v = (expr);                                                                                                        \
    f->kind = (uint8_t)(kk);                                                                                           \
    f->a = a;                                                                                                          \
    f->b = b;                                                                                                          \
    f->r = v;                                                                                                          \
    f->w = 4u;                                                                                                         \
  } while (0)
    OPF(0, 1, a + b, kX86pFlagsAdd, 0);             /* prev Logic */
    OPF(2, 3, a ^ b, kX86pFlagsLogic, f->r < f->a); /* prev Add   */
    OPF(1, 6, a - b, kX86pFlagsSub, 0);             /* prev Logic */
    OPF(3, 0, a & b, kX86pFlagsLogic, f->a < f->b); /* prev Sub   */
    r[6] = r[2];                                    /* MOV writes no flags */
    OPF(7, 5, a | b, kX86pFlagsLogic, 0);           /* prev Logic */
    OPF(5, 4, a + b, kX86pFlagsAdd, 0);             /* prev Logic */
    OPF(4, 7, a ^ b, kX86pFlagsLogic, f->r < f->a); /* prev Add   */
#undef OPF
#undef OPF_NODST
  }
}

static void seed(X86pCpu *cpu) {
  int i;
  x86p_cpu_reset(cpu);
  for (i = 0; i < kX86pRegCount; i++) {
    cpu->reg[i] = 0x1234567u * (uint32_t)(i + 1);
  }
  cpu->eip = GUEST_BASE;
}

/*
 * The guest CPU state, CACHE-LINE ALIGNED and static rather than a stack local.
 *
 * Measured, not decorative: as a stack local this benchmark was BIMODAL,
 * reporting either 0.97 or 1.93 ns/insn -- a clean factor of two, stable within
 * a run and varying between runs, while the interpreter column stayed fixed at
 * 133 ns/insn. Stack randomisation was moving X86pCpu across a cache-line
 * boundary, and translated code touches this struct on nearly every emitted
 * instruction, so the split doubled its cost. The interpreter was unaffected
 * because its per-instruction cost is dominated by decode.
 *
 * This is a fact about the JIT, not about the benchmark: a consuming port
 * should align its X86pCpu too.
 */
static X86pCpu g_cpu; /* alignment now comes from the type itself */

/* Repetitions per column. Enough that a single scheduling hiccup cannot be the
   reported number, few enough that the tool stays usable in a loop. */
#define REPS 5

int main(int argc, char **argv) {
  X86pMem mem;
  X86pCpu *cpup = &g_cpu;
  X86pJitBlock blk;
  char reason[256];
  X86pJitStatus st;
  void *code;
  uint32_t kernel_insns;
  unsigned long iters = 200000ul;
  double t0;
  double t_interp = 1e30;
  double t_jit = 1e30;
  double t_native = 1e30;
  double t_native_flags = 1e30;
  double w_interp = 0.0;
  double w_jit = 0.0;
  double w_native_flags = 0.0;
  unsigned long total;
  unsigned long i;
  int rep;

  if (argc > 1) {
    iters = strtoul(argv[1], NULL, 10);
  }

  if (!x86p_jit_available()) {
    printf("REFUSED: no x86-64 backend in this build; nothing to measure\n");
    return 1;
  }

  memset(g_guest, 0x90, sizeof g_guest);
  kernel_insns = build_kernel();

  mem.host = g_guest;
  mem.lo = GUEST_BASE;
  mem.size = GUEST_SIZE;

  code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) {
    printf("REFUSED: could not map code memory\n");
    return 1;
  }

  t0 = now_s();
  st = x86p_jit_translate(&mem, GUEST_BASE, code, CODE_SIZE, &blk, reason, sizeof reason);
  if (st != kX86pJitOk) {
    printf("REFUSED: translate -> %s (%s)\n", x86p_jit_status_name(st), reason);
    return 1;
  }
  printf("kernel: %u guest instruction(s), block translated %u of them into %zu host byte(s) in %.3f ms\n",
         kernel_insns,
         blk.insns,
         blk.host_bytes,
         (now_s() - t0) * 1e3);
  if (blk.insns != kernel_insns) {
    /* If the block stopped early the three engines are not doing the same work
       and every number below would be a comparison of different programs. */
    printf("REFUSED: block covers %u of %u instruction(s) (stopped at %s); the columns would not be comparable\n",
           blk.insns,
           kernel_insns,
           blk.stopper ? blk.stopper : "?");
    return 1;
  }

  /*
   * EVERY COLUMN IS THE BEST OF SEVERAL REPETITIONS, AND THE SPREAD IS PRINTED.
   *
   * A single timed run of this shape varies by a third on an ordinary desktop:
   * measured, the same binary reported anywhere from 1.33x to 1.79x on
   * consecutive runs. Quoting one of those to two decimal places is a precision
   * the instrument does not have, and a change that moved the number inside
   * that band would read as a regression or an improvement at random.
   *
   * The minimum is the right estimator for a microbenchmark: interference from
   * other processes, migration and frequency scaling can only ADD time, so the
   * fastest observed run is the closest to the cost being measured. The spread
   * is printed beside it so a reader can see when the machine was too busy for
   * the number to mean anything.
   */
  for (rep = 0; rep < REPS; rep++) {
    double t;

    /* ---- interpreter ---- */
    seed(cpup);
    t0 = now_s();
    for (i = 0; i < iters; i++) {
      uint32_t k;
      cpup->eip = GUEST_BASE;
      for (k = 0; k < kernel_insns; k++) {
        if (x86p_step(cpup, &mem, NULL) != kX86pStepOk) {
          printf("REFUSED: interpreter stopped mid-kernel\n");
          return 1;
        }
      }
    }
    t = now_s() - t0;
    if (t < t_interp) {
      t_interp = t;
    }
    if (t > w_interp) {
      w_interp = t;
    }

    /* ---- jit ---- */
    seed(cpup);
    t0 = now_s();
    for (i = 0; i < iters; i++) {
      cpup->eip = GUEST_BASE;
      (void)x86p_jit_enter(&blk, cpup);
    }
    t = now_s() - t0;
    if (t < t_jit) {
      t_jit = t;
    }
    if (t > w_jit) {
      w_jit = t;
    }

    /* ---- native ---- */
    {
      X86pCpu nc;
      seed(&nc);
      t0 = now_s();
      for (i = 0; i < iters; i++) {
        native_kernel(nc.reg);
      }
      t = now_s() - t0;
      if (t < t_native) {
        t_native = t;
      }
      /* Consume the result so the loop cannot be optimised away entirely. */
      if (nc.reg[0] == 0xDEADBEEFu) {
        printf("(unreachable)\n");
      }
    }

    /* ---- native with guest flags: the like-for-like control ---- */
    {
      X86pCpu nc;
      seed(&nc);
      t0 = now_s();
      for (i = 0; i < iters; i++) {
        native_kernel_flags(&nc);
      }
      t = now_s() - t0;
      if (t < t_native_flags) {
        t_native_flags = t;
      }
      if (t > w_native_flags) {
        w_native_flags = t;
      }
      if (nc.reg[0] == 0xDEADBEEFu) {
        printf("(unreachable)\n");
      }
    }
  }

  total = iters * (unsigned long)kernel_insns;
  printf(
      "\n%lu iteration(s) x %u instruction(s) = %lu guest instruction(s) per engine\n\n", iters, kernel_insns, total);
  printf("  %-12s %8.3f s   %7.2f ns/insn   %6.2fx native\n",
         "native C",
         t_native,
         t_native / total * 1e9,
         t_native / t_native);
  printf("  %-12s %8.3f s   %7.2f ns/insn   %6.2fx native\n",
         "native+flags",
         t_native_flags,
         t_native_flags / total * 1e9,
         t_native_flags / t_native);
  printf("  %-12s %8.3f s   %7.2f ns/insn   %6.2fx native\n", "jit", t_jit, t_jit / total * 1e9, t_jit / t_native);
  printf("  %-12s %8.3f s   %7.2f ns/insn   %6.2fx native\n",
         "interpreter",
         t_interp,
         t_interp / total * 1e9,
         t_interp / t_native);
  printf("\n  best of %d repetition(s); slowest run was %.0f%% (jit) and %.0f%% (native+flags) above the best\n",
         REPS,
         100.0 * (w_jit / t_jit - 1.0),
         100.0 * (w_native_flags / t_native_flags - 1.0));
  printf("\n  jit is %.2fx faster than the interpreter\n", t_interp / t_jit);
  printf("  jit is %.2fx the cost of native+flags  <-- THE SCORE (1.00 would match the control)\n",
         t_jit / t_native_flags);
  printf("\nNOTE: native+flags stores every flag unconditionally. The JIT now DOES eliminate\n"
         "dead flag writes (flag_write_is_dead in jit_x64.c): a lazy tuple whose bits a later\n"
         "in-block instruction overwrites before any read is not stored at all. This kernel\n"
         "never reads its flags, so the JIT drops nearly all of them -- which is why it can\n"
         "score below 1.00x here. On flag-heavy code where flags ARE read the elimination does\n"
         "not fire and the per-op cost below is what remains.\n");
  printf("\nnative+flags is the like-for-like control: the same operations, maintaining\n"
         "the same guest flags. The flag-free\n"
         "column is a floor no correct x86 implementation can reach and is shown only for scale.\n");
  munmap(code, CODE_SIZE);
  return 0;
}
