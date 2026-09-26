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
#include "cpu_compare.h"
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
  /*
   * The first occurrence, verbatim. A shape is a work item only if you can
   * reproduce it: "ADD s0,s0" names a decode this framework does not model and
   * says nothing about WHICH encoding produced it, and the difference between
   * a far-pointer form and a genuinely mis-decoded padding byte is exactly the
   * thing the reader needs and the tool already had in its hand.
   */
  uint32_t first_addr;
  uint8_t first_bytes[16];
  uint8_t first_len;
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
/* Of the refused, how many are not represented by the shared semantic model.
   Both categories are product refusals; this split identifies the owner of the
   missing work without creating a runtime fallback. */
static unsigned long g_insn_no_semantics;

static void
note_into(Stopper *tab, unsigned cap, unsigned *n, const char *m, const uint8_t *bytes, unsigned len, uint32_t addr) {
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
  tab[*n].first_addr = addr;
  tab[*n].first_len = (uint8_t)(len > sizeof tab[*n].first_bytes ? sizeof tab[*n].first_bytes : len);
  if (bytes) {
    memcpy(tab[*n].first_bytes, bytes, tab[*n].first_len);
  }
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

/* One diverging field, interpreter first. */
static void print_diff(const char *field, const char *interp_text, const char *jit_text, void *user) {
  (void)user;
  printf("    %s: interp=%s jit=%s\n", field, interp_text, jit_text);
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
/*
 * How many instructions live in [base, base+len).
 *
 * `census` says whether this walk is the one that owns the denominator. It is
 * called a second time to ACCOUNT for instructions the translating walk gave
 * up on, and counting those into the corpus total as well would inflate both
 * sides of the ratio it exists to explain.
 */
static uint32_t count_insns_ex(const X86pMem *mem, uint32_t base, uint32_t len, int census) {
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
    if (census) {
      g_insn_seen++;
    }
    if (census && !x86p_jit_can_translate(&in)) {
      char shape[32];
      format_shape(&in, shape, sizeof shape);
      g_insn_refused++;
      if (in.op == (uint8_t)kX86pInsnUnsupported) {
        g_insn_no_semantics++;
      }
      note_into(
          g_refused, (unsigned)(sizeof g_refused / sizeof g_refused[0]), &g_refusals, shape, b, in.length, base + off);
    }
    off += in.length;
    n++;
  }
  return n;
}

static uint32_t count_insns(const X86pMem *mem, uint32_t base, uint32_t len) {
  return count_insns_ex(mem, base, len, 1);
}

/*
 * WHERE A BLOCK CAN GO.
 *
 * Block chaining removes the dispatcher's hash lookup and its second indirect
 * call, and it can only do that for a successor the translator already knows.
 * A RET or an indirect jump goes back through the dispatcher whatever else is
 * built, so the ratio between these is what decides whether chaining is worth
 * building -- and it is a property of the shipped binary, which no generated
 * program can stand in for.
 *
 * The three nested subsets under it are the loop backedges, which pay a
 * dispatch PER ITERATION and are worth the most for the least. X86pWasmExitCensus
 * states what separates them; each is a cheaper fix reaching fewer exits.
 *
 * WHICH TIER A LOOP LANDS IN DEPENDS ON THIS TOOL, NOT ON THE GUEST. It walks
 * each function linearly from its first byte, so a loop head the running engine
 * would dispatch to -- and translate a block AT -- is mid-block here, and one
 * above a CALL is in an earlier block still. `to_self` and `to_loop` are floors
 * in this census; `to_backward` is the one that does not move, because a
 * backedge is backward however the blocks were cut.
 *
 * READ FROM THE TRANSLATOR, NOT RE-DERIVED HERE. This tool used to walk each
 * block's bytes and classify its LAST instruction, and that is the wrong
 * question twice over: a conditional branch does not end a block in the
 * WebAssembly backend, so the loop backedges are exits INSIDE the body that a
 * terminator walk never sees. It reported one self-loop in 113,272 blocks of a
 * real title. The lowering knows each exit's target exactly, because it is the
 * thing that emits it, so it counts them and this sums what it counted.
 */
typedef struct ExitCensus {
  unsigned long blocks; /* blocks that reported an exit -- the denominator */
  unsigned long total;
  unsigned long to_static;
  unsigned long to_backward;
  unsigned long to_loop;
  unsigned long to_self;
} ExitCensus;

static void note_exit(const X86pJitBlock *blk, ExitCensus *c) {
  if (blk->exits == 0u) {
    /* Either a backend that does not fill these in, or a block whose only way
       out is a refusal rather than a successor. Neither is an exit, and the
       report refuses outright if NO block in the corpus reported one. */
    return;
  }
  c->blocks++;
  c->total += blk->exits;
  c->to_static += blk->exits_static;
  c->to_backward += blk->exits_backward;
  c->to_loop += blk->exits_loop;
  c->to_self += blk->exits_self;
}

int main(int argc, char **argv) {
  FILE *fp = stdin;
  void *code;
  /* Static, not automatic: at two hex digits per byte this is 128 KiB, and a
     wasm build's default stack is 64 KiB -- as an automatic it overflowed the
     stack before the first line was read, which surfaced as an out-of-bounds
     access inside the module with no line of C to blame. */
  static char line[MAX_FN_BYTES * 2 + 64];
  unsigned long fns = 0;
  unsigned long fns_with_block = 0;
  unsigned long insns_total = 0;
  unsigned long insns_covered = 0;
  unsigned long conds = 0;
  unsigned long conds_inline = 0;
  unsigned long conds_unknown = 0;
  /*
   * WHERE A BLOCK GOES WHEN IT ENDS.
   *
   * Block chaining removes the dispatcher's hash lookup and its second
   * indirect call, and it can only do that for a successor the translator
   * already knows. A RET or an indirect jump goes back through the dispatcher
   * whatever else is built, so the ratio between these is what decides whether
   * chaining is worth building -- and it is a property of the shipped binary,
   * which no generated program can stand in for.
   */
  ExitCensus exits = {0};
  /*
   * Blocks this build cannot ENTER, which is not the same as blocks it cannot
   * translate. A backend that produces a WebAssembly module has no host
   * address to call, so the differential below has nothing to run. Counted so
   * the report can refuse by name instead of printing zero divergences over
   * zero comparisons, which reads exactly like agreement.
   */
  unsigned long unenterable = 0;
  /*
   * WHERE THE UNTRANSLATED INSTRUCTIONS WENT.
   *
   * "99.45% covered" is only a fact if the other 0.55% is named. These three
   * account for every instruction the walk did not hand to a block, and the
   * report prints what is left over -- which must be zero, or one of the
   * three is wrong about its own cause.
   */
  unsigned long skipped_refused = 0; /* stepped over: no emitter for it */
  unsigned long skipped_undec = 0;   /* the bytes stopped decoding: data, not code */
  unsigned long skipped_tail = 0;    /* fewer than four bytes left to translate from */
  unsigned long blocks = 0;
  /* Host code emitted for those blocks, prologue and tails included: the
     static cost of the translation, which codegen work moves. */
  unsigned long long host_bytes = 0;
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
    X86pMem mem = {0};
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
     * previous block ended on. A refused instruction is stepped OVER only by
     * this offline census so later independent instructions are counted. The
     * product engine refuses at that instruction and never advances through an
     * interpreter.
     *
     * Linear rather than following branches: a function's bytes are its
     * instructions, and a static walk cannot know which are reachable. That
     * over-counts unreachable padding and under-counts nothing, and it is
     * stated rather than hidden.
     */
    /*
     * The walk moves the START ADDRESS through the function and keeps the
     * WHOLE function mapped, which is what the engine does.
     *
     * Re-slicing the mapping at each step instead -- host, lo and size all
     * advanced together -- looked equivalent and was not: the translator
     * requires a mapping of at least four bytes, because its bounds check
     * computes size - 4, so every function ended with a stub too small to
     * translate. It cost 11,584 instructions, 0.53% of the corpus, all of
     * them translatable, and it read as a coverage gap in the engine rather
     * than a defect in the instrument measuring it.
     */
    walked = 0u;
    while (walked < nbytes) {
      const X86pMem sub = mem;
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
          skipped_refused++;
          walked += in.length;
          continue;
        }
        note_stopper("(undecodable)");
        skipped_undec += count_insns_ex(&sub, entry + walked, nbytes - walked, 0);
        break;
      }

      if (blocks_here == 0u) {
        fns_with_block++;
      }
      blocks_here++;
      blocks++;
      insns_covered += blk.insns;
      host_bytes += blk.host_bytes;
      conds += blk.conds;
      conds_inline += blk.cond_inline;
      conds_unknown += blk.cond_unknown_kind;
      note_exit(&blk, &exits);
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
      if (blk.entry == NULL) {
        /*
         * There is no host address to call. A WebAssembly backend hands back a
         * MODULE, not a pointer, so entering this block means instantiating it
         * -- which this offline census does not do. Entering NULL anyway
         * produced a divergence for every block and then a fault, which is the
         * worst kind of diagnostic: one that answers confidently and wrongly.
         */
        unenterable++;
      } else {
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
          /* The one authority on "architecturally identical" (cpu_compare.h),
             SIMD and x87 state included, not a field list of this tool's own:
             that list compared the carry_in cache where no instruction can
             read it, which reported hundreds of blocks that agree on every
             observable bit and buried any real divergence among them. */
          if (x86p_cpu_diff(&ci, &cj, NULL, NULL) != 0u) {
            diverged++;
            if (diverged <= 5) {
              unsigned q;
              uint32_t at = entry + walked;
              printf("DIVERGENCE at %08X after %u insn(s)\n", at, blk.insns);
              /* WHICH field, and over WHICH instructions. An address alone
                 says a block disagreed and leaves the reader to rediscover
                 everything the tool already knew. */
              (void)x86p_cpu_diff(&ci, &cj, print_diff, NULL);
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
  {
    /* The residual is the honest part: three named causes and whatever they
       failed to explain. A non-zero leftover means this accounting is wrong,
       and printing it is the only way that ever surfaces. */
    unsigned long lost = insns_total - insns_covered;
    unsigned long named = skipped_refused + skipped_undec + skipped_tail;
    printf("  not translated            %lu  (%.2f%%)\n",
           lost,
           insns_total ? 100.0 * (double)lost / (double)insns_total : 0.0);
    printf("    refused, stepped over   %lu\n", skipped_refused);
    printf("    undecodable bytes       %lu\n", skipped_undec);
    printf("    function tail < 4 bytes %lu\n", skipped_tail);
    printf("    unaccounted             %ld   <-- must be 0\n", (long)lost - (long)named);
  }
  /* How often the backend read a condition off the host's own flags instead of
     calling x86p_cond, over REAL game code rather than generated programs: the
     synthetic differential almost never places a width-4 ALU immediately before
     a branch, so its rate says nothing about a shipped binary. */
  if (conds == 0u) {
    printf("  NO Jcc or SETcc was emitted, so condition lowering is unmeasured here\n");
  } else {
    printf("  conditions lowered inline %lu of %lu  (%.1f%%); %lu call x86p_cond\n",
           conds_inline,
           conds,
           100.0 * (double)conds_inline / (double)conds,
           conds - conds_inline);
  }
  printf("  mean block length         %.2f guest instruction(s)\n",
         blocks ? (double)insns_covered / (double)blocks : 0.0);
  printf("  host code                 %llu byte(s), %.1f per guest instruction\n",
         host_bytes,
         insns_covered ? (double)host_bytes / (double)insns_covered : 0.0);
  if (conds == 0u) {
    printf("  NO Jcc or SETcc was emitted, so the condition census is unmeasured\n");
  } else {
    printf("    of the %lu not lowered inline, %lu had no recorded predecessor and\n"
           "    %lu a kind no derivation covers\n",
           conds - conds_inline,
           conds_unknown,
           conds - conds_inline - conds_unknown);
  }
  printf("\n  where a block can go -- what block chaining could reach:\n");
  if (exits.total == 0) {
    printf("    REFUSED: not one of the %lu block(s) reported an exit. Either this\n"
           "    build's backend does not fill X86pJitBlock's exit counters -- only\n"
           "    the WebAssembly one does -- or nothing was translated. This is NOT\n"
           "    a corpus without branches in it.\n",
           blocks);
  } else {
    const double per = 100.0 / (double)exits.total;
    printf("    %lu exit(s) from %lu block(s), %.2f per block\n",
           exits.total,
           exits.blocks,
           exits.blocks ? (double)exits.total / (double)exits.blocks : 0.0);
    printf("    successor already known    %lu  (%.1f%%)  <-- chaining can reach these\n",
           exits.to_static,
           per * (double)exits.to_static);
    printf("      of those, BACKWARD -- a guest loop backedge, paying a\n"
           "      dispatch per iteration:          %lu  (%.1f%% of exits)\n",
           exits.to_backward,
           per * (double)exits.to_backward);
    printf("        head inside this same block:   %lu  (%.1f%%)\n", exits.to_loop, per * (double)exits.to_loop);
    printf("        head AT this block's entry:    %lu  (%.1f%%)  <-- a single-pass\n"
           "                                       `loop` around the body reaches these\n",
           exits.to_self,
           per * (double)exits.to_self);
    printf("      The last two are floors: this walk splits each function linearly,\n"
           "      so a loop head the engine would start a block at is mid-block here.\n");
    printf("    Each block counts ONCE. A hot loop and a function that never runs weigh\n"
           "    the same here, so this is the static ratio and not the dynamic one.\n");
    printf("    computed at run time       %lu  (%.1f%%)  <-- RET, indirect JMP/CALL\n",
           exits.total - exits.to_static,
           per * (double)(exits.total - exits.to_static));
  }

  printf("\n  differential on real code: %lu block(s) compared, %lu divergence(s)\n", compared, diverged);
  if (unenterable) {
    printf("  REFUSED: %lu block(s) have no host entry address, so this build's\n"
           "  backend cannot be run in process and the differential above covers\n"
           "  only what could be entered. Correctness for those blocks is UNPROVEN\n"
           "  here; the differential suites are where that backend is checked.\n",
           unenterable);
  } else if (compared == 0) {
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
  printf("    of those, %lu have no shared semantics (%.2f%% of the corpus)\n",
         g_insn_no_semantics,
         g_insn_seen ? 100.0 * (double)g_insn_no_semantics / (double)g_insn_seen : 0.0);
  printf("    The other %lu need JIT emission for semantics already owned by the test oracle.\n\n",
         g_insn_refused - g_insn_no_semantics);
  if (g_refusals == (unsigned)(sizeof g_refused / sizeof g_refused[0])) {
    printf("    WARNING: the shape table is FULL, so this list is truncated and the\n");
    printf("    distinct-shape count is a floor rather than the real number.\n\n");
  }
  for (i = 0; i < g_refusals; i++) {
    unsigned b;
    printf("    %-32s %8lu  (%.2f%%)   first at %08X:",
           g_refused[i].name,
           g_refused[i].count,
           100.0 * (double)g_refused[i].count / (double)g_insn_seen,
           g_refused[i].first_addr);
    for (b = 0; b < g_refused[i].first_len; b++) {
      printf(" %02X", g_refused[i].first_bytes[b]);
    }
    printf("\n");
  }
  munmap(code, CODE_SIZE);
  return diverged ? 1 : 0;
}
