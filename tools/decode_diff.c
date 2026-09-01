/*
 * decode_diff -- does x86port's decoder read the shipped bytes the same way the
 * corpus says they read?
 *
 * WHY. The decoder was chosen (decode.h), not derived, so the choice has to be
 * checked against something that did not come from it. pc/xmen2's Ghidra export
 * carries the RAW BYTES of every instruction beside Ghidra's own mnemonic and
 * length -- 2,168,629 instructions across 20 modules. That is an offline,
 * byte-level second opinion on 116,500 real functions, and it costs nothing to
 * consult.
 *
 * INPUT is one instruction per line, tab separated, from tools/corpus_extract.py:
 *
 *     <hex bytes>\t<MNEMONIC>\t<length>
 *
 * THE NEGATIVE, WRITTEN FIRST. This tool must never be able to print a clean
 * bill of health it did not earn:
 *
 *   - Zero input lines is a REFUSAL (exit 2), not "0 disagreements". A corpus
 *     that was not extracted and a corpus that agrees perfectly must not print
 *     the same thing.
 *   - Every count is reported against the denominator it came from, and the
 *     categories are disjoint and sum to the total -- checked, and the check is
 *     part of the output.
 *   - Mnemonic spelling and instruction LENGTH are counted separately, because
 *     they are different claims. Length is objective: two decoders that agree on
 *     it read the same bytes as the same instruction. Spelling is a convention,
 *     and Ghidra's differs from Zydis's in places for reasons that are not
 *     defects. Folding them together would let a naming difference read as a
 *     decode failure, or hide one.
 *   - Disagreements are SAMPLED INTO THE REPORT, not summarised away. A count
 *     with no example is not actionable.
 *
 * MEASURED 2026-09-01 against pc/xmen2's 20-module export, 2,168,629
 * instructions: 0 failed to decode, and 37 length disagreements -- ALL 37 of
 * them the 0x9B prefix, where Ghidra folds FWAIT into the following x87
 * instruction (FSTSW 24, FSTCW 11, FSAVE 2) and Zydis reports FWAIT as the
 * separate instruction it architecturally is. Neither is wrong, and the literal
 * reading is the one an interpreter wants, because the CPU really does execute
 * two instructions there. This is NOT special-cased in the code: an instrument
 * that silences a category cannot tell you when a real defect joins it.
 */
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 512
#define SAMPLES 12
#define NAMEPAIRS 64

struct NamePair {
  char corpus[24];
  char ours[24];
  unsigned long n;
};

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

/* hex text -> bytes. Returns the byte count, or -1 for anything malformed --
   an odd digit count and a stray character are corpus defects, and silently
   truncating either would decode a different instruction than the corpus meant. */
static int unhex(const char *s, uint8_t *out, size_t cap) {
  size_t n = 0;
  while (*s && *s != '\t' && *s != '\n') {
    int hi = hexval((unsigned char)s[0]);
    int lo = s[1] ? hexval((unsigned char)s[1]) : -1;
    if (hi < 0 || lo < 0 || n >= cap) {
      return -1;
    }
    out[n++] = (uint8_t)((hi << 4) | lo);
    s += 2;
  }
  return (int)n;
}

static int ci_equal(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    int ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
    int cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
    if (ca != cb) {
      return 0;
    }
  }
  return *a == '\0' && *b == '\0';
}

static void note_pair(struct NamePair *tab, int *ntab, const char *corpus, const char *ours) {
  int i;
  for (i = 0; i < *ntab; i++) {
    if (ci_equal(tab[i].corpus, corpus) && ci_equal(tab[i].ours, ours)) {
      tab[i].n++;
      return;
    }
  }
  if (*ntab >= NAMEPAIRS) {
    return;
  }
  snprintf(tab[*ntab].corpus, sizeof tab[0].corpus, "%s", corpus);
  snprintf(tab[*ntab].ours, sizeof tab[0].ours, "%s", ours);
  tab[*ntab].n = 1;
  (*ntab)++;
}

int main(void) {
  char line[MAX_LINE];
  unsigned long total = 0, malformed = 0, undecodable = 0;
  unsigned long len_agree = 0, len_disagree = 0;
  unsigned long name_agree = 0, name_differ = 0;
  struct NamePair pairs[NAMEPAIRS];
  int npairs = 0;
  char undec_sample[SAMPLES][MAX_LINE];
  int nundec = 0;
  char lendis_sample[SAMPLES][MAX_LINE];
  int nlendis = 0;
  int i;

  while (fgets(line, sizeof line, stdin)) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    X86pInsn insn;
    char *tab1, *tab2;
    const char *corpus_mn;
    long corpus_len;
    int nbytes;

    if (line[0] == '\n' || line[0] == '\0') {
      continue;
    }
    total++;

    tab1 = strchr(line, '\t');
    tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
    if (!tab1 || !tab2) {
      malformed++;
      continue;
    }
    *tab1 = '\0';
    *tab2 = '\0';
    corpus_mn = tab1 + 1;
    corpus_len = strtol(tab2 + 1, NULL, 10);

    nbytes = unhex(line, bytes, sizeof bytes);
    if (nbytes <= 0 || corpus_len <= 0) {
      malformed++;
      continue;
    }

    if (!x86p_decode(bytes, (size_t)nbytes, &insn)) {
      undecodable++;
      if (nundec < SAMPLES) {
        snprintf(undec_sample[nundec++], MAX_LINE, "%s  corpus says %s (%ld bytes)", line, corpus_mn, corpus_len);
      }
      continue;
    }

    if ((long)insn.length == corpus_len) {
      len_agree++;
    } else {
      len_disagree++;
      if (nlendis < SAMPLES) {
        snprintf(lendis_sample[nlendis++],
                 MAX_LINE,
                 "%s  corpus %s/%ld bytes, ours %s/%u bytes",
                 line,
                 corpus_mn,
                 corpus_len,
                 insn.mnemonic,
                 insn.length);
      }
    }

    if (ci_equal(insn.mnemonic, corpus_mn)) {
      name_agree++;
    } else {
      name_differ++;
      note_pair(pairs, &npairs, corpus_mn, insn.mnemonic);
    }
  }

  /* REFUSE rather than certify an empty run. "The corpus was never extracted"
     and "the corpus agrees perfectly" must not look the same. */
  if (total == 0) {
    fprintf(stderr,
            "decode_diff: read 0 instructions from stdin. REFUSING rather than "
            "reporting agreement -- an empty corpus proves nothing about the "
            "decoder. Pipe tools/corpus_extract.py's output in.\n");
    return 2;
  }

  printf("decoder: %s\n", x86p_decoder_id());
  printf("instructions read: %lu\n", total);
  printf("  malformed input lines:   %8lu\n", malformed);
  printf("  FAILED TO DECODE:        %8lu  (%.4f%%)\n", undecodable, 100.0 * (double)undecodable / (double)total);
  printf("  decoded:                 %8lu\n", len_agree + len_disagree);
  printf("    length agrees:         %8lu  (%.4f%% of decoded)\n",
         len_agree,
         100.0 * (double)len_agree / (double)(len_agree + len_disagree ? len_agree + len_disagree : 1));
  printf("    LENGTH DISAGREES:      %8lu\n", len_disagree);
  printf("    mnemonic agrees:       %8lu\n", name_agree);
  printf("    mnemonic spelled differently: %lu (a convention gap, not "
         "necessarily a defect -- judge from the pairs below)\n",
         name_differ);

  /* The categories must account for every line read. Printing the identity
     makes a future edit that drops a case visible here instead of silently
     shrinking the denominator. */
  {
    unsigned long accounted = malformed + undecodable + len_agree + len_disagree;
    printf("  accounted for: %lu of %lu%s\n",
           accounted,
           total,
           accounted == total ? "" : "  <-- CATEGORIES DO NOT SUM; the report is wrong");
    if (accounted != total) {
      return 3;
    }
  }

  if (nundec) {
    printf("\nundecodable samples (bytes, then what the corpus called them):\n");
    for (i = 0; i < nundec; i++) {
      printf("  %s\n", undec_sample[i]);
    }
  }
  if (nlendis) {
    printf("\nlength disagreements -- the two readings consume different "
           "amounts of instruction. A real defect OR a rendering convention; "
           "judge from the samples:\n");
    for (i = 0; i < nlendis; i++) {
      printf("  %s\n", lendis_sample[i]);
    }
  }
  if (npairs) {
    printf("\nmnemonic spelling pairs (corpus -> ours), up to %d distinct:\n", NAMEPAIRS);
    for (i = 0; i < npairs; i++) {
      printf("  %-14s -> %-14s %lu\n", pairs[i].corpus, pairs[i].ours, pairs[i].n);
    }
  }

  /* Length disagreement and decode failure are the two that mean something is
     wrong. Spelling is reported and does not fail the run. */
  return (undecodable || len_disagree) ? 1 : 0;
}
