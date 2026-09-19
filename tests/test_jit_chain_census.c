/*
 * The chain census counts three populations and they must not run into each
 * other: an entry the previous block knew about, an entry it did not, and an
 * entry whose predecessor was never recorded. The third exists because every
 * machine-code backend records nothing, and a run against one of those must
 * not read as a run where nothing was chainable.
 */
#include "jit_chain_census.h"

#include <stdio.h>

static int failures;

static void expect(int condition, const char *what) {
  if (condition) {
    return;
  }
  printf("FAIL: %s\n", what);
  failures++;
}

static void expect_u64(uint64_t got, uint64_t want, const char *what) {
  if (got == want) {
    return;
  }
  printf("FAIL: %s -- got %llu, want %llu\n", what, (unsigned long long)got, (unsigned long long)want);
  failures++;
}

int main(void) {
  X86pJitChainCensus *c = x86p_jit_chain_census_create(64u);
  uint32_t targets[2] = {0x2000u, 0x3000u};
  uint32_t many[X86P_JIT_CHAIN_TARGETS + 2u];
  unsigned i;

  expect(c != NULL, "a census could be created");
  if (!c) {
    return 1;
  }

  x86p_jit_chain_census_note_block(c, 0x1000u, targets, 2u, 0u);

  /* The first entry of a run has no predecessor, and saying it was not
     chainable would be a claim the data cannot support. */
  x86p_jit_chain_census_note_entry(c, 0u, 0x1000u, 0);
  expect_u64(x86p_jit_chain_census_unrecorded(c), 1u, "an entry with no predecessor is unrecorded");
  expect_u64(x86p_jit_chain_census_chainable(c), 0u, "an entry with no predecessor is not chainable");

  /* Both recorded successors count, and only those. */
  x86p_jit_chain_census_note_entry(c, 0x1000u, 0x2000u, 1);
  x86p_jit_chain_census_note_entry(c, 0x1000u, 0x3000u, 1);
  expect_u64(x86p_jit_chain_census_chainable(c), 2u, "both recorded successors are chainable");
  x86p_jit_chain_census_note_entry(c, 0x1000u, 0x4000u, 1);
  expect_u64(x86p_jit_chain_census_chainable(c), 2u, "an address the block never named is not chainable");
  expect_u64(x86p_jit_chain_census_unrecorded(c), 1u, "an unknown target is not counted as unrecorded");

  /* A predecessor nobody recorded is its own answer. */
  x86p_jit_chain_census_note_entry(c, 0x9999u, 0x2000u, 1);
  expect_u64(x86p_jit_chain_census_unrecorded(c), 2u, "an unrecorded predecessor is counted as such");

  expect_u64(x86p_jit_chain_census_entries(c), 5u, "every entry is in the denominator");
  expect_u64(x86p_jit_chain_census_blocks(c), 1u, "one block was recorded");

  /* A retranslation replaces the record; the old successors are gone. */
  targets[0] = 0x5000u;
  x86p_jit_chain_census_note_block(c, 0x1000u, targets, 1u, 0u);
  x86p_jit_chain_census_note_entry(c, 0x1000u, 0x2000u, 1);
  expect_u64(x86p_jit_chain_census_chainable(c), 2u, "a replaced record does not keep its old successors");
  x86p_jit_chain_census_note_entry(c, 0x1000u, 0x5000u, 1);
  expect_u64(x86p_jit_chain_census_chainable(c), 3u, "the replacement's successor is chainable");

  /* More successors than the cap: the extras are reported, not dropped in
     silence. */
  for (i = 0u; i < X86P_JIT_CHAIN_TARGETS + 2u; i++) {
    many[i] = 0x10000u + i;
  }
  x86p_jit_chain_census_note_block(c, 0x6000u, many, X86P_JIT_CHAIN_TARGETS, 2u);
  expect_u64(x86p_jit_chain_census_overflowed(c), 2u, "successors past the cap are counted");
  x86p_jit_chain_census_note_entry(c, 0x6000u, 0x10000u, 1);
  expect_u64(x86p_jit_chain_census_chainable(c), 4u, "a kept successor of a full block is chainable");

  x86p_jit_chain_census_destroy(c);

  /* A table that fills drops keys and says so. */
  c = x86p_jit_chain_census_create(64u);
  expect(c != NULL, "a second census could be created");
  if (!c) {
    return 1;
  }
  for (i = 0u; i < 1000u; i++) {
    uint32_t one = 0x40000u + i;
    x86p_jit_chain_census_note_block(c, one, &one, 1u, 0u);
  }
  expect(x86p_jit_chain_census_dropped_keys(c) > 0u, "a full table reports its drops");
  expect(x86p_jit_chain_census_blocks(c) < 1000u, "a full table stopped inserting");
  x86p_jit_chain_census_destroy(c);

  printf("jit chain census: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
