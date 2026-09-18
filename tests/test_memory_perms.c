/*
 * Exact page permissions on the CONTIGUOUS memory mode.
 *
 * The desktop leaves X86pMem::perms NULL and lets the host VM enforce
 * permissions, so these checks are about the other case: a host with no VM of
 * its own, which must get exact permissions without paying the sparse mode's
 * per-access binary search.
 *
 * Every check goes through the shipping helpers -- x86p_mem_accessible,
 * read/write, the span walkers -- not a reimplementation of the rule, so a
 * change that breaks the emitted code's agreement with them shows up here.
 */
#include "cpu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kPageShift = 12, kPage = 1u << kPageShift, kPages = 4, kLo = 0x10000000u };

static unsigned checks, failures;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

int main(void) {
  static uint8_t bytes[kPages * kPage];
  uint8_t perms[kPages];
  X86pMem memory = {0};
  uint32_t value = 0;
  uint8_t scratch[8];

  memory.host = bytes;
  memory.lo = kLo;
  memory.size = sizeof bytes;
  memory.perms = perms;
  memory.page_shift = kPageShift;

  perms[0] = kX86pMemRead | kX86pMemWrite;
  perms[1] = kX86pMemRead;
  perms[2] = 0u; /* not mapped at all */
  perms[3] = kX86pMemWrite;

  /* The permitted page behaves exactly as it would with no table at all. */
  check(x86p_mem_write(&memory, kLo, 4, 0x12345678u), "write to a read-write page");
  check(x86p_mem_read(&memory, kLo, 4, &value) && value == 0x12345678u, "read back what was written");

  /* Read-only: the read works, the write is refused and changes nothing. */
  bytes[kPage] = 0xab;
  check(x86p_mem_read(&memory, kLo + kPage, 1, &value) && value == 0xabu, "read a read-only page");
  check(!x86p_mem_write(&memory, kLo + kPage, 1, 0x11u), "write to a read-only page is refused");
  check(bytes[kPage] == 0xab, "the refused write touched nothing");

  /* Write-only: the mirror image, so a table that returns the same answer for
     both permissions cannot pass. */
  check(x86p_mem_write(&memory, kLo + 3u * kPage, 1, 0x5au), "write to a write-only page");
  check(!x86p_mem_read(&memory, kLo + 3u * kPage, 1, &value), "read from a write-only page is refused");

  /* Zero means unmapped: refused both ways, and refused even by the
     access == 0 query that asks only whether backing exists. */
  check(!x86p_mem_read(&memory, kLo + 2u * kPage, 1, &value), "read from an unmapped page is refused");
  check(!x86p_mem_write(&memory, kLo + 2u * kPage, 1, 1u), "write to an unmapped page is refused");
  check(!x86p_mem_accessible(&memory, kLo + 2u * kPage, 1, 0u), "an unmapped page has no backing");
  check(x86p_mem_accessible(&memory, kLo + 2u * kPage - 1u, 1, 0u), "the page before it does");

  /*
   * STRADDLING. An access whose bytes fall in two pages needs BOTH permitted,
   * and this is the case a one-page check would get wrong -- it is also
   * exactly what the emitted guard's second load is for.
   */
  check(!x86p_mem_write(&memory, kLo + kPage - 2u, 4, 0u), "a write straddling into a read-only page is refused");
  check(!x86p_mem_read(&memory, kLo + 2u * kPage - 1u, 2, &value),
        "a read straddling into an unmapped page is refused");
  perms[1] = kX86pMemRead | kX86pMemWrite;
  check(x86p_mem_write(&memory, kLo + kPage - 2u, 4, 0x89abcdefu), "the same straddling write succeeds once permitted");
  check(x86p_mem_read(&memory, kLo + kPage - 2u, 4, &value) && value == 0x89abcdefu, "and reads back whole");

  /* The bulk walkers stop at a permission change rather than running past it. */
  check(!x86p_mem_read_bytes(&memory, kLo + 2u * kPage - 4u, scratch, 8u),
        "a bulk read crossing into an unmapped page is refused");
  check(x86p_mem_read_bytes(&memory, kLo + kPage - 4u, scratch, 8u),
        "a bulk read crossing two permitted pages succeeds");

  /*
   * The bulk COPY and FILL behind REP MOVS/STOS need the whole range resolved
   * to one host pointer. A span that stopped at every page boundary refused
   * every request longer than a page, so these are what keeps a table-backed
   * mapping from silently losing them to element-at-a-time work.
   */
  {
    uint8_t *resolved = NULL;
    perms[2] = kX86pMemRead | kX86pMemWrite;
    check(x86p_mem_resolve(&memory, kLo + kPage - 4u, 2u * kPage, &resolved) && resolved == bytes + kPage - 4u,
          "a range spanning three permitted pages resolves whole");
    check(x86p_mem_fill(&memory, kLo + kPage - 4u, (const uint8_t[]){0x5a}, 1u, 2u * kPage),
          "and the bulk fill takes it");
    check(bytes[kPage - 4u] == 0x5a && bytes[3u * kPage - 5u] == 0x5a && bytes[3u * kPage - 4u] != 0x5a,
          "filling exactly the requested bytes and no more");
    check(x86p_mem_copy_disjoint(&memory, kLo, kLo + 2u * kPage, kPage), "and the bulk copy takes a whole page");
    perms[2] = 0u;
    check(!x86p_mem_resolve(&memory, kLo + kPage - 4u, 2u * kPage, &resolved),
          "the same range stops refusing to resolve only because a page in the middle lost access");
  }

  /* And with no table, permissions are the host's business again: the same
     write that was refused above is allowed, which proves these checks are
     measuring the table and not something else. */
  memory.perms = NULL;
  memory.page_shift = 0u;
  check(x86p_mem_write(&memory, kLo + 2u * kPage, 1, 1u), "with no table, an unmapped page is writable again");

  printf(
      "memory page permissions: %s -- %u check(s), %u failure(s)\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures != 0;
}
