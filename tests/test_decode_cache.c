/*
 * test_decode_cache -- the cache answers exactly what the decoder would.
 *
 * The failure this guards against is silent by construction: a cache that
 * returns a stale instruction runs the WRONG semantics on the right address,
 * and every symptom appears somewhere else. So each test states the negative
 * as well -- that a rewritten byte is noticed, that a displaced slot decodes
 * again -- and the counters are checked, not just the answers, because a cache
 * that decoded every time would return correct answers too and save nothing.
 */
#include "cpu.h"
#include "decode_cache.h"
#include "exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                       \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ_U(got, want)                                                                                          \
  do {                                                                                                                 \
    unsigned long long g_ = (unsigned long long)(got);                                                                 \
    unsigned long long w_ = (unsigned long long)(want);                                                                \
    g_checks++;                                                                                                        \
    if (g_ != w_) {                                                                                                    \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s: got %llu want %llu\n", __FILE__, __LINE__, #got, g_, w_);                            \
    }                                                                                                                  \
  } while (0)

static int g_test_failed;
#define RUN(fn)                                                                                                        \
  do {                                                                                                                 \
    int before = g_failed;                                                                                             \
    printf("test %s\n", #fn);                                                                                          \
    fn();                                                                                                              \
    if (g_failed != before) {                                                                                          \
      g_test_failed++;                                                                                                 \
      printf("  FAIL\n");                                                                                              \
    } else {                                                                                                           \
      printf("  PASS\n");                                                                                              \
    }                                                                                                                  \
  } while (0)

/* MOV EAX, 1 -- five bytes, one immediate, easy to tell from the alternative
   below at a glance in a failure message. */
static const uint8_t kMovEax1[] = {0xB8, 0x01, 0x00, 0x00, 0x00};
/* MOV EAX, 2 at the same length, so a rewrite changes the ANSWER without
   changing the length the validation compares. */
static const uint8_t kMovEax2[] = {0xB8, 0x02, 0x00, 0x00, 0x00};

static X86pDecodeCache *fresh(void) {
  X86pDecodeCache *c = (X86pDecodeCache *)calloc(1, sizeof *c);
  if (!c) {
    printf("    FAIL: out of memory for a decode cache\n");
    exit(1);
  }
  return c;
}

static void test_second_lookup_is_a_hit(void) {
  X86pDecodeCache *c = fresh();
  X86pInsn a, b;
  CHECK(x86p_decode_cached(c, 0x1000u, kMovEax1, sizeof kMovEax1, &a));
  CHECK_EQ_U(c->hits, 0u);
  CHECK_EQ_U(c->cold, 1u);
  CHECK(x86p_decode_cached(c, 0x1000u, kMovEax1, sizeof kMovEax1, &b));
  CHECK_EQ_U(c->hits, 1u);
  CHECK_EQ_U(x86p_decode_cache_lookups(c), 2u);
  /* The point of the whole module: the cached answer is the decoded one. */
  CHECK_EQ_U(b.op, a.op);
  CHECK_EQ_U(b.length, a.length);
  CHECK_EQ_U(b.operand[1].imm, a.operand[1].imm);
  free(c);
}

static void test_rewritten_bytes_are_decoded_again(void) {
  X86pDecodeCache *c = fresh();
  X86pInsn a, b;
  CHECK(x86p_decode_cached(c, 0x1000u, kMovEax1, sizeof kMovEax1, &a));
  CHECK(x86p_decode_cached(c, 0x1000u, kMovEax2, sizeof kMovEax2, &b));
  CHECK_EQ_U(c->rewritten, 1u);
  CHECK_EQ_U(c->hits, 0u);
  /* Self-modifying code: the SAME address now decodes to a different value,
     and the cache must not have answered with the first one. */
  CHECK_EQ_U(a.operand[1].imm, 1u);
  CHECK_EQ_U(b.operand[1].imm, 2u);
  free(c);
}

static void test_a_displaced_slot_decodes_again(void) {
  X86pDecodeCache *c = fresh();
  X86pInsn a;
  unsigned i;
  /* Two addresses that share a slot. Found rather than assumed: the mapping is
     the module's own, so a test that hardcoded a stride would silently stop
     testing collisions the day it changed. */
  uint32_t other = 0u;
  CHECK(x86p_decode_cached(c, 0x1000u, kMovEax1, sizeof kMovEax1, &a));
  for (i = 1u; i < 1000000u && !other; i++) {
    X86pDecodeCache *probe = fresh();
    X86pInsn tmp;
    (void)x86p_decode_cached(probe, 0x1000u, kMovEax1, sizeof kMovEax1, &tmp);
    (void)x86p_decode_cached(probe, 0x1000u + i, kMovEax1, sizeof kMovEax1, &tmp);
    if (probe->collisions == 1u) {
      other = 0x1000u + i;
    }
    free(probe);
  }
  CHECK(other != 0u); /* a direct-mapped table of finite size must have one */
  if (other) {
    CHECK(x86p_decode_cached(c, other, kMovEax2, sizeof kMovEax2, &a));
    CHECK_EQ_U(c->collisions, 1u);
    CHECK_EQ_U(a.operand[1].imm, 2u);
    /* And the displaced address is now the cold one, not silently the hit. */
    CHECK(x86p_decode_cached(c, 0x1000u, kMovEax1, sizeof kMovEax1, &a));
    CHECK_EQ_U(c->hits, 0u);
    CHECK_EQ_U(a.operand[1].imm, 1u);
  }
  free(c);
}

static void test_a_null_cache_decodes_every_time(void) {
  X86pInsn a;
  CHECK(x86p_decode_cached(NULL, 0x1000u, kMovEax1, sizeof kMovEax1, &a));
  CHECK_EQ_U(a.operand[1].imm, 1u);
  CHECK_EQ_U(x86p_decode_cache_lookups(NULL), 0u);
}

static void test_undecodable_bytes_are_not_cached(void) {
  X86pDecodeCache *c = fresh();
  /* 0x0F 0x0B is UD2 in later CPUs; 0xFF 0xFF is not an instruction at all. */
  static const uint8_t kJunk[] = {0xFF, 0xFF};
  X86pInsn a;
  if (x86p_decode_cached(c, 0x2000u, kJunk, sizeof kJunk, &a)) {
    /* Some encodings that look like junk do decode; then this test has nothing
       to say and says so rather than passing vacuously. */
    printf("    note: 0xFF 0xFF decoded, so the refusal path is untested here\n");
  } else {
    CHECK_EQ_U(c->hits, 0u);
    CHECK(!x86p_decode_cached(c, 0x2000u, kJunk, sizeof kJunk, &a));
    CHECK_EQ_U(c->hits, 0u); /* a failed decode must never become a hit */
  }
  free(c);
}

/* The cache is reached through x86p_step_cached, so the wiring is tested as
   well as the module: an execution through the cache must leave the same
   machine as one without it. */
static void test_stepping_matches_the_uncached_path(void) {
  X86pDecodeCache *c = fresh();
  uint8_t image[64];
  X86pMem mem;
  X86pCpu cached, plain;
  unsigned i;

  /* A two-instruction LOOP, because a straight line never revisits an address
     and so could not hit the cache at all: MOV EAX,1 then JMP back to it. */
  memset(image, 0x90, sizeof image);
  memcpy(image, kMovEax1, sizeof kMovEax1);
  image[5] = 0xEB; /* JMP rel8 */
  image[6] = 0xF9; /* -7, back to 0x1000 */
  memset(&mem, 0, sizeof mem);
  mem.host = image;
  mem.lo = 0x1000u;
  mem.size = sizeof image;

  memset(&cached, 0, sizeof cached);
  memset(&plain, 0, sizeof plain);
  cached.eip = plain.eip = 0x1000u;

  for (i = 0; i < 8u; i++) {
    CHECK_EQ_U(x86p_step_cached(&cached, &mem, c, NULL), kX86pStepOk);
    CHECK_EQ_U(x86p_step(&plain, &mem, NULL), kX86pStepOk);
    CHECK_EQ_U(cached.eip, plain.eip);
    CHECK_EQ_U(x86p_reg_read(&cached, 0, 4), x86p_reg_read(&plain, 0, 4));
  }
  /* Eight steps around a two-instruction loop: after the first pass every
     lookup is a repeat, so a cache that never hit would be broken. */
  CHECK(c->hits > 0u);
  free(c);
}

static void test_readable_span_stops_at_the_end(void) {
  uint8_t image[16];
  X86pMem mem;
  memset(image, 0x90, sizeof image);
  memset(&mem, 0, sizeof mem);
  mem.host = image;
  mem.lo = 0x1000u;
  mem.size = sizeof image;
  CHECK_EQ_U(x86p_mem_readable_span(&mem, 0x1000u, 15u), 15u);
  CHECK_EQ_U(x86p_mem_readable_span(&mem, 0x1008u, 15u), 8u);
  CHECK_EQ_U(x86p_mem_readable_span(&mem, 0x100Fu, 15u), 1u);
  CHECK_EQ_U(x86p_mem_readable_span(&mem, 0x1010u, 15u), 0u);
  CHECK_EQ_U(x86p_mem_readable_span(&mem, 0x0FFFu, 15u), 0u);
  CHECK_EQ_U(x86p_mem_readable_span(NULL, 0x1000u, 15u), 0u);
}

int main(void) {
  RUN(test_second_lookup_is_a_hit);
  RUN(test_rewritten_bytes_are_decoded_again);
  RUN(test_a_displaced_slot_decodes_again);
  RUN(test_a_null_cache_decodes_every_time);
  RUN(test_undecodable_bytes_are_not_cached);
  RUN(test_stepping_matches_the_uncached_path);
  RUN(test_readable_span_stops_at_the_end);
  printf("%d check(s), %d failure(s)\n", g_checks, g_failed);
  return g_test_failed || g_failed ? 1 : 0;
}
