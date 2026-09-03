/*
 * test_jit_profile -- the block-entry histogram counts, ranks, and reports its
 * own blind spot.
 *
 * The failure this guards against is the one every profiler has: it silently
 * under-counts once its table is full and the top-N then lies by omission. So
 * the drop path is tested explicitly -- a full table must keep counting keys it
 * already holds, must refuse new ones, and must say how many it refused -- and
 * the ranking is checked against a known-hot key, not just "some order".
 */
#include "jit_profile.h"

#include <stdio.h>
#include <stdlib.h>

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

static void test_counts_and_ranks_by_entries(void) {
  X86pJitProfile *p = x86p_jit_profile_create(64u);
  X86pJitProfileEntry top[4];
  uint32_t n, i;
  CHECK(p != NULL);

  for (i = 0; i < 100u; i++) {
    x86p_jit_profile_hit(p, 0x00401000u);
  }
  for (i = 0; i < 30u; i++) {
    x86p_jit_profile_hit(p, 0x00402000u);
  }
  for (i = 0; i < 5u; i++) {
    x86p_jit_profile_hit(p, 0x00403000u);
  }

  CHECK(x86p_jit_profile_total_hits(p) == 135u);
  CHECK(x86p_jit_profile_distinct(p) == 3u);
  CHECK(x86p_jit_profile_dropped_keys(p) == 0u);

  n = x86p_jit_profile_top(p, top, 4u);
  CHECK(n == 3u);
  CHECK(top[0].guest_eip == 0x00401000u && top[0].entries == 100u);
  CHECK(top[1].guest_eip == 0x00402000u && top[1].entries == 30u);
  CHECK(top[2].guest_eip == 0x00403000u && top[2].entries == 5u);

  /* A smaller n returns exactly the hottest n, still ranked. */
  n = x86p_jit_profile_top(p, top, 2u);
  CHECK(n == 2u);
  CHECK(top[0].entries == 100u && top[1].entries == 30u);

  x86p_jit_profile_destroy(p);
}

static void test_a_full_table_drops_new_keys_but_keeps_counting_held_ones(void) {
  /* slot_hint 64 -> capacity 64 distinct keys. */
  X86pJitProfile *p = x86p_jit_profile_create(64u);
  X86pJitProfileEntry top[8];
  uint32_t i, n;
  CHECK(p != NULL);

  for (i = 0; i < 64u; i++) {
    x86p_jit_profile_hit(p, 0x00400000u + i * 0x100u);
  }
  CHECK(x86p_jit_profile_distinct(p) == 64u);
  CHECK(x86p_jit_profile_dropped_keys(p) == 0u);

  /* The 65th distinct key cannot be inserted; it is dropped and counted. */
  x86p_jit_profile_hit(p, 0x00500000u);
  x86p_jit_profile_hit(p, 0x00500000u);
  CHECK(x86p_jit_profile_distinct(p) == 64u);
  CHECK(x86p_jit_profile_dropped_keys(p) == 2u);

  /* A key already in the table is still counted after the table filled. */
  for (i = 0; i < 10u; i++) {
    x86p_jit_profile_hit(p, 0x00400000u);
  }
  n = x86p_jit_profile_top(p, top, 8u);
  CHECK(n == 8u);
  CHECK(top[0].guest_eip == 0x00400000u && top[0].entries == 11u);

  CHECK(x86p_jit_profile_total_hits(p) == 64u + 2u + 10u);

  x86p_jit_profile_destroy(p);
}

static void test_empty_and_degenerate(void) {
  X86pJitProfileEntry top[2];
  X86pJitProfile *p = x86p_jit_profile_create(0u); /* clamped up, not refused */
  CHECK(p != NULL);
  CHECK(x86p_jit_profile_top(p, top, 2u) == 0u);
  CHECK(x86p_jit_profile_distinct(p) == 0u);

  /* NULL profile is a no-op, not a crash: the hot path guards on it. */
  x86p_jit_profile_hit(NULL, 0x1234u);
  CHECK(x86p_jit_profile_total_hits(NULL) == 0u);
  CHECK(x86p_jit_profile_top(NULL, top, 2u) == 0u);

  x86p_jit_profile_destroy(p);
  x86p_jit_profile_destroy(NULL);
}

int main(void) {
  test_counts_and_ranks_by_entries();
  test_a_full_table_drops_new_keys_but_keeps_counting_held_ones();
  test_empty_and_degenerate();
  printf("test_jit_profile: %d check(s), %d failure(s)\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
