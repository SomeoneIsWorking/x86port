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
  char name[24];
  unsigned long count;
} Stopper;

static Stopper g_stop[512];
static unsigned g_stoppers;

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

    st = x86p_jit_translate(&mem, entry, code, CODE_SIZE, &blk, reason, sizeof reason);
    if (st != kX86pJitOk) {
      /* No block at all. The FIRST instruction is the stopper, and naming it is
         the whole point -- these are the entries that dominate the queue. */
      uint8_t b[X86P_MAX_INSN_LEN];
      X86pInsn in;
      uint32_t k;
      uint32_t have = 0;
      for (k = 0; k < (uint32_t)X86P_MAX_INSN_LEN && k < nbytes; k++) {
        b[k] = g_body[k];
        have++;
      }
      if (have && x86p_decode(b, have, &in)) {
        note_stopper(in.mnemonic);
      } else {
        note_stopper("(undecodable)");
      }
      continue;
    }

    fns_with_block++;
    blocks++;
    insns_covered += blk.insns;
    note_stopper(blk.ends_in_branch ? "(branch: block ended normally)" : blk.stopper);

    /* Differential on REAL code: same start state, both engines, whole machine. */
    {
      X86pCpu ci;
      X86pCpu cj;
      uint32_t k;
      int bad = 0;
      x86p_cpu_reset(&ci);
      for (k = 0; k < kX86pRegCount; k++) {
        ci.reg[k] = 0x11111111u * (k + 1u);
      }
      ci.eip = entry;
      cj = ci;
      for (k = 0; k < blk.insns; k++) {
        if (x86p_step(&ci, &mem, NULL) != kX86pStepOk) {
          bad = 1;
          break;
        }
      }
      if (!bad) {
        (void)x86p_jit_enter(&blk, &cj);
        compared++;
        if (memcmp(ci.reg, cj.reg, sizeof ci.reg) != 0 || ci.eip != cj.eip ||
            ci.flags.kind != cj.flags.kind || ci.flags.a != cj.flags.a || ci.flags.b != cj.flags.b ||
            ci.flags.r != cj.flags.r || ci.flags.w != cj.flags.w || ci.flags.carry_in != cj.flags.carry_in) {
          diverged++;
          if (diverged <= 5) {
            printf("DIVERGENCE at %08X after %u insn(s)\n", entry, blk.insns);
          }
        }
      }
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
  printf("  instructions translated   %lu  (%.2f%%)   <-- COVERAGE\n", insns_covered,
         insns_total ? 100.0 * (double)insns_covered / (double)insns_total : 0.0);
  printf("  mean block length         %.2f guest instruction(s)\n",
         blocks ? (double)insns_covered / (double)blocks : 0.0);
  printf("\n  differential on real code: %lu block(s) compared, %lu divergence(s)\n", compared, diverged);
  if (compared == 0) {
    printf("  REFUSED: nothing was compared, so correctness here is unproven\n");
  }

  qsort(g_stop, g_stoppers, sizeof g_stop[0], by_count);
  printf("\n  what ends a block, ranked -- THIS IS THE WORK QUEUE:\n\n");
  for (i = 0; i < g_stoppers && i < 25u; i++) {
    printf("    %-32s %8lu\n", g_stop[i].name, g_stop[i].count);
  }
  munmap(code, CODE_SIZE);
  return diverged ? 1 : 0;
}
