/*
 * jit_coverage -- how much of a REAL game does the translator cover, and what
 * is stopping it?
 *
 * The differential in tests/ generates its own programs, which proves the
 * backend correct on the instructions it already supports and says NOTHING
 * about how much of a shipped binary that is. A translator can be flawless on
 * everything it emits and still cover two percent of a game.
 *
 * So this reads a title's Ghidra export -- the same corpus tools/decode_diff.c
 * already uses to check the decoder, 2.1M instructions of X-Men Legends II --
 * lays each function out at its real entry address, and translates from there.
 * It reports two things a synthetic test cannot:
 *
 *   - COVERAGE: what fraction of real instructions land inside a translated
 *     block. This is the honest progress number toward running the game.
 *   - THE RANKED STOPPER LIST: which mnemonic ended each block, by frequency.
 *     That is the work queue, ordered by what the game actually executes
 *     rather than by what seemed important. An unsupported instruction that
 *     appears once does not matter; one that ends 40% of blocks decides the
 *     next commit.
 *
 * It also runs the DIFFERENTIAL on every block it translates, against the
 * interpreter, on real game code. Coverage without correctness would be a
 * number that only sounds like progress.
 *
 * STATIC COVERAGE, NOT DYNAMIC. Every function counts once, whether it runs
 * every frame or never. A profile-weighted number would be more useful and
 * needs the game running, which is not yet possible; this is the bound
 * available today and is labelled as such rather than quietly presented as
 * the real thing.
 */
#include "cpu.h"
#include "decode.h"
#include "exec.h"
#include "jit_x64.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAX_FN_BYTES 65536u
#define CODE_SIZE 262144u

static uint8_t g_body[MAX_FN_BYTES];

typedef struct Stopper {
  char name[32];
  unsigned long count;
} Stopper;

static Stopper g_stop[512];

/*
 * A mnemonic alone is not a work item.
 *
 * "MOV, 1021" says nothing about what to implement: register-to-register MOV
 * has been translated since the first commit, so the thousand refusals are
 * some SHAPE of MOV, and which one decides whether the fix is an afternoon or
 * a week. Appending the operand kinds and widths turns the ranked list into an
 * actual queue. Widths are in bytes; `m` is memory, `i` an immediate, `s` a
 * segment or other operand kind this backend does not model.
 */
static char operand_letter(const X86pOperand *o) {
  switch (o->kind) {
  case kX86pOperandReg:
    return 'r';
  case kX86pOperandMem:
    return 'm';
  case kX86pOperandImm:
    return 'i';
  case kX86pOperandSt:
    return 'f';
  default:
    return 's';
  }
}

static void format_shape(const X86pInsn *in, char *out, size_t cap) {
  size_t n = 0;
  int i;
  n += (size_t)snprintf(out + n, cap - n, "%s", in->mnemonic);
  for (i = 0; i < (int)in->operands && n + 6u < cap; i++) {
    n += (size_t)snprintf(
        out + n, cap - n, "%c%c%d", (i == 0) ? ' ' : ',', operand_letter(&in->operand[i]), in->operand[i].size);
  }
}
static unsigned g_stoppers;

/*
 * A SECOND census, over every instruction in the corpus rather than over block
 * boundaries.
 *
 * The stopper list answers "what should I do next", because it is weighted by
 * how often an instruction actually blocks progress. It cannot answer "how far
 * is 100%", because everything after a function's first refusal is never
 * decoded at all -- an instruction that only appears deep inside long functions
 * is invisible to it. This walks every function to its end and asks
 * x86p_jit_can_translate about each one, so the remaining work has a
 * denominator.
 */
static Stopper g_refused[512];
static unsigned g_refusals;
static unsigned long g_insn_seen;
static unsigned long g_insn_refused;
/* Of the refused, how many the INTERPRETER cannot run either. That is the
   difference between "needs an emitter" and "needs semantics", and they are
   different jobs: the first can be routed through the existing authority, the
   second has no authority to route to. */
static unsigned long g_insn_no_semantics;

static void note_into(Stopper *tab, unsigned cap, unsigned *n, const char *m) {
  unsigned i;
  for (i = 0; i < *n; i++) {
    if (strcmp(tab[i].name, m) == 0) {
      tab[i].count++;
      return;
    }
  }
  if (*n == cap) {
    return;
  }
  snprintf(tab[*n].name, sizeof tab[*n].name, "%s", m);
  tab[*n].count = 1u;
  (*n)++;
}

static void note_stopper(const char *m) {
  unsigned i;
  if (!m) {
    m = "(ran out of room / insn cap)";
  }
  for (i = 0; i < g_stoppers; i++) {
    if (strcmp(g_stop[i].name, m) == 0) {
      g_stop[i].count++;
      return;
    }
  }
  if (g_stoppers >= sizeof g_stop / sizeof g_stop[0]) {
    return;
  }
  snprintf(g_stop[g_stoppers].name, sizeof g_stop[0].name, "%s", m);
  g_stop[g_stoppers].count = 1;
  g_stoppers++;
}

static int by_count(const void *a, const void *b) {
  const Stopper *x = (const Stopper *)a;
  const Stopper *y = (const Stopper *)b;
  if (x->count < y->count) {
    return 1;
  }
  return (x->count > y->count) ? -1 : 0;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* Count the instructions a function really holds, so coverage has a
   denominator that does not come from the translator being measured. */
static uint32_t count_insns(const X86pMem *mem, uint32_t base, uint32_t len) {
  uint32_t off = 0;
  uint32_t n = 0;
  while (off < len) {
    uint8_t b[X86P_MAX_INSN_LEN];
    X86pInsn in;
    uint32_t i;
    uint32_t have = 0;
    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN && off + i < len; i++) {
      uint32_t v;
      if (!x86p_mem_read(mem, base + off + i, 1, &v)) {
        break;
      }
      b[i] = (uint8_t)v;
      have++;
    }
    if (have == 0 || !x86p_decode(b, have, &in) || in.length == 0) {
      break;
    }
    /* The census: every instruction in the corpus, whether or not any block
       ever reached it. This is the denominator for 100% coverage. */
    g_insn_seen++;
    if (!x86p_jit_can_translate(&in)) {
      char shape[32];
      format_shape(&in, shape, sizeof shape);
      g_insn_refused++;
      if (in.op == (uint8_t)kX86pInsnUnsupported) {
        g_insn_no_semantics++;
      }
      note_into(g_refused, (unsigned)(sizeof g_refused / sizeof g_refused[0]), &g_refusals, shape);
    }
    off += in.length;
    n++;
  }
  return n;
}

int main(int argc, char **argv) {
  FILE *fp = stdin;
  void *code;
  char line[MAX_FN_BYTES * 2 + 64];
  unsigned long fns = 0;
  unsigned long fns_with_block = 0;
  unsigned long insns_total = 0;
  unsigned long insns_covered = 0;
  unsigned long blocks = 0;
  unsigned long diverged = 0;
  unsigned long compared = 0;
  unsigned long helper_insns = 0;
  unsigned i;

  if (argc > 1) {
    fp = fopen(argv[1], "r");
    if (!fp) {
      fprintf(stderr, "jit_coverage: cannot open %s\n", argv[1]);
      return 1;
    }
  }
  if (!x86p_jit_available()) {
    fprintf(stderr, "REFUSED: no x86-64 backend in this build\n");
    return 1;
  }
  code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) {
    fprintf(stderr, "REFUSED: cannot map code memory\n");
    return 1;
  }

  while (fgets(line, sizeof line, fp)) {
    char *tab = strchr(line, '\t');
    uint32_t entry;
    uint32_t nbytes = 0;
    const char *h;
    X86pMem mem;
    X86pJitBlock blk;
    char reason[256];
    X86pJitStatus st;
    uint32_t total;
    uint32_t walked;
    unsigned long blocks_here = 0u;

    if (!tab) {
      continue;
    }
    *tab = '\0';
    entry = (uint32_t)strtoul(line, NULL, 16);
    h = tab + 1;
    while (h[0] && h[1] && h[0] != '\n') {
      int hi = hexval((unsigned char)h[0]);
      int lo = hexval((unsigned char)h[1]);
      if (hi < 0 || lo < 0) {
        break;
      }
      if (nbytes >= MAX_FN_BYTES) {
        break;
      }
      g_body[nbytes++] = (uint8_t)((hi << 4) | lo);
      h += 2;
    }
    if (nbytes == 0) {
      continue;
    }

    mem.host = g_body;
    mem.lo = entry;
    mem.size = nbytes;

    fns++;
    total = count_insns(&mem, entry, nbytes);
    insns_total += total;

    /*
     * TRANSLATE THE WHOLE FUNCTION, not just its entry block.
     *
     * Translating once at the entry measured "how long is the first basic
     * block", which is about five instructions in any compiled code and has
     * almost nothing to do with how much of the program can run. Every
     * function was reported at 5/N covered and the number moved barely at all
     * when whole families gained emitters.
     *
     * So this walks the body linearly, translating at each address the
     * previous block ended on. A refused instruction is stepped OVER and the
     * walk continues, because the engine does exactly that -- it hands the
     * instruction to the interpreter and translates again from the next
     * address -- and abandoning the rest of the function here would report a
     * coverage this framework does not actually lose.
     *
     * Linear rather than following branches: a function's bytes are its
     * instructions, and a static walk cannot know which are reachable. That
     * over-counts unreachable padding and under-counts nothing, and it is
     * stated rather than hidden.
     */
    walked = 0u;
    while (walked < nbytes) {
      X86pMem sub;
      sub.host = g_body + walked;
      sub.lo = entry + walked;
      sub.size = nbytes - walked;
      if (sub.size < 4u) {
        break;
      }
      st = x86p_jit_translate(&sub, entry + walked, code, CODE_SIZE, &blk, reason, sizeof reason);
      if (st != kX86pJitOk) {
        uint8_t b[X86P_MAX_INSN_LEN];
        X86pInsn in;
        uint32_t k;
        uint32_t have = 0;
        for (k = 0; k < (uint32_t)X86P_MAX_INSN_LEN && walked + k < nbytes; k++) {
          b[k] = g_body[walked + k];
          have++;
        }
        if (have && x86p_decode(b, have, &in) && in.length) {
          char shape[32];
          format_shape(&in, shape, sizeof shape);
          note_stopper(shape);
          walked += in.length;
          continue;
        }
        note_stopper("(undecodable)");
        break;
      }

      if (blocks_here == 0u) {
        fns_with_block++;
      }
      blocks_here++;
      blocks++;
      insns_covered += blk.insns;
      helper_insns += blk.helper_calls;
      if (blk.ends_in_branch) {
        note_stopper("(branch: block ended normally)");
      } else if (blk.stopper == NULL) {
        note_stopper("(ran out of room / insn cap)");
      } else {
        uint8_t b[X86P_MAX_INSN_LEN];
        X86pInsn in;
        uint32_t k;
        uint32_t have = 0;
        for (k = 0; k < (uint32_t)X86P_MAX_INSN_LEN && walked + blk.guest_len + k < nbytes; k++) {
          b[k] = g_body[walked + blk.guest_len + k];
          have++;
        }
        if (have && x86p_decode(b, have, &in)) {
          char shape[32];
          format_shape(&in, shape, sizeof shape);
          note_stopper(shape);
        } else {
          note_stopper(blk.stopper);
        }
      }
      if (blk.guest_len == 0u) {
        /* A zero-length block would loop here forever. It is a defect in the
           translator, not a corpus oddity, so it is reported and not skipped
           past quietly. */
        fprintf(stderr, "jit_coverage: zero-length block at %08X\n", entry + walked);
        break;
      }

      /*
       * Differential on REAL code: same start state, both engines, whole
       * machine. Coverage without correctness is a number that only sounds
       * like progress.
       */
      {
        X86pCpu ci;
        X86pCpu cj;
        uint32_t k;
        int bad = 0;
        x86p_cpu_reset(&ci);
        for (k = 0; k < kX86pRegCount; k++) {
          ci.reg[k] = 0x11111111u * (k + 1u);
        }
        ci.eip = entry + walked;
        cj = ci;
        for (k = 0; k < blk.insns; k++) {
          if (x86p_step(&ci, &sub, NULL) != kX86pStepOk) {
            bad = 1;
            break;
          }
        }
        if (!bad) {
          (void)x86p_jit_enter(&blk, &cj);
          compared++;
          /* The SIMD state too. Six per cent of translated instructions are
             floating point or vector, and comparing only the integer file
             would call a block that dropped every one of them identical. */
          if (memcmp(ci.reg, cj.reg, sizeof ci.reg) != 0 || ci.eip != cj.eip || ci.flags.kind != cj.flags.kind ||
              ci.flags.a != cj.flags.a || ci.flags.b != cj.flags.b || ci.flags.r != cj.flags.r ||
              ci.flags.w != cj.flags.w || ci.flags.carry_in != cj.flags.carry_in ||
              memcmp(ci.xmm, cj.xmm, sizeof ci.xmm) != 0 || ci.mxcsr != cj.mxcsr || ci.df != cj.df ||
              memcmp(ci.seg, cj.seg, sizeof ci.seg) != 0 || ci.x87.top != cj.x87.top ||
              memcmp(ci.x87.tag, cj.x87.tag, sizeof ci.x87.tag) != 0 ||
              memcmp(ci.x87.reg, cj.x87.reg, sizeof ci.x87.reg) != 0) {
            diverged++;
            if (diverged <= 5) {
              unsigned q;
              uint32_t at = entry + walked;
              printf("DIVERGENCE at %08X after %u insn(s)\n", at, blk.insns);
              /* WHICH field, and over WHICH instructions. An address alone
                 says a block disagreed and leaves the reader to rediscover
                 everything the tool already knew. */
              for (q = 0; q < 8u; q++) {
                if (ci.reg[q] != cj.reg[q]) {
                  printf("    r%u: interp=%08X jit=%08X\n", q, ci.reg[q], cj.reg[q]);
                }
              }
              if (ci.eip != cj.eip) {
                printf("    eip: interp=%08X jit=%08X\n", ci.eip, cj.eip);
              }
              if (ci.flags.kind != cj.flags.kind || ci.flags.a != cj.flags.a || ci.flags.b != cj.flags.b ||
                  ci.flags.r != cj.flags.r || ci.flags.w != cj.flags.w || ci.flags.carry_in != cj.flags.carry_in) {
                printf("    flags: interp kind=%d a=%08X b=%08X r=%08X w=%d cin=%u | jit kind=%d a=%08X b=%08X "
                       "r=%08X w=%d cin=%u\n",
                       (int)ci.flags.kind, ci.flags.a, ci.flags.b, ci.flags.r, ci.flags.w,
                       (unsigned)ci.flags.carry_in, (int)cj.flags.kind, cj.flags.a, cj.flags.b, cj.flags.r,
                       cj.flags.w, (unsigned)cj.flags.carry_in);
              }
              if (ci.x87.top != cj.x87.top) {
                printf("    x87 top: interp=%u jit=%u\n", ci.x87.top, cj.x87.top);
              }
              {
                uint32_t pc2 = at;
                for (q = 0; q < blk.insns; q++) {
                  X86pInsn di;
                  uint8_t bb[X86P_MAX_INSN_LEN];
                  uint32_t avail = 0, z;
                  for (z = 0; z < X86P_MAX_INSN_LEN; z++) {
                    uint32_t bv;
                    if (!x86p_mem_read(&sub, pc2 + z, 1, &bv)) {
                      break;
                    }
                    bb[z] = (uint8_t)bv;
                    avail++;
                  }
                  memset(&di, 0, sizeof di);
                  if (!avail || x86p_decode(bb, avail, &di) == 0u) {
                    break;
                  }
                  printf("      %08X  %s\n", pc2, di.mnemonic ? di.mnemonic : "?");
                  pc2 += di.length;
                }
              }
            }
          }
        }
      }
      walked += blk.guest_len;
    }
  }

  if (fns == 0) {
    printf("REFUSED: no functions read. A coverage report over zero functions is not 0%%, it is nothing.\n");
    return 1;
  }

  printf("\n=== JIT coverage over real game code (STATIC, unweighted by execution) ===\n\n");
  printf("  functions read            %lu\n", fns);
  printf("  functions with a block    %lu  (%.1f%%)\n", fns_with_block, 100.0 * (double)fns_with_block / (double)fns);
  printf("  instructions in corpus    %lu\n", insns_total);
  printf("  instructions translated   %lu  (%.2f%%)   <-- COVERAGE\n",
         insns_covered,
         insns_total ? 100.0 * (double)insns_covered / (double)insns_total : 0.0);
  printf("  mean block length         %.2f guest instruction(s)\n",
         blocks ? (double)insns_covered / (double)blocks : 0.0);
  printf("  of those, via a helper    %lu  (%.2f%% of translated)   <-- NOT host code\n",
         helper_insns,
         insns_covered ? 100.0 * (double)helper_insns / (double)insns_covered : 0.0);
  printf("\n  differential on real code: %lu block(s) compared, %lu divergence(s)\n", compared, diverged);
  if (compared == 0) {
    printf("  REFUSED: nothing was compared, so correctness here is unproven\n");
  }

  qsort(g_stop, g_stoppers, sizeof g_stop[0], by_count);
  printf("\n  what ends a block, ranked -- THIS IS THE WORK QUEUE:\n\n");
  for (i = 0; i < g_stoppers && i < 25u; i++) {
    printf("    %-32s %8lu\n", g_stop[i].name, g_stop[i].count);
  }
  qsort(g_refused, g_refusals, sizeof g_refused[0], by_count);
  printf("\n  every instruction the translator would REFUSE, ranked -- the road to 100%%:\n\n");
  printf("    %lu of %lu instruction(s) refused (%.2f%%), in %u distinct shape(s)\n\n",
         g_insn_refused,
         g_insn_seen,
         g_insn_seen ? 100.0 * (double)g_insn_refused / (double)g_insn_seen : 0.0,
         g_refusals);
  printf("    of those, %lu have NO SEMANTICS ANYWHERE (%.2f%% of the corpus): the\n",
         g_insn_no_semantics,
         g_insn_seen ? 100.0 * (double)g_insn_no_semantics / (double)g_insn_seen : 0.0);
  printf("    interpreter cannot run them either, so they need semantics, not an emitter.\n");
  printf("    The other %lu already have an authority to route to.\n\n", g_insn_refused - g_insn_no_semantics);
  if (g_refusals == (unsigned)(sizeof g_refused / sizeof g_refused[0])) {
    printf("    WARNING: the shape table is FULL, so this list is truncated and the\n");
    printf("    distinct-shape count is a floor rather than the real number.\n\n");
  }
  for (i = 0; i < g_refusals; i++) {
    printf("    %-32s %8lu  (%.2f%%)\n",
           g_refused[i].name,
           g_refused[i].count,
           100.0 * (double)g_refused[i].count / (double)g_insn_seen);
  }
  munmap(code, CODE_SIZE);
  return diverged ? 1 : 0;
}
