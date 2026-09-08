#include "cpu.h"
#include "memory_sparse.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;

static void check(int value, const char *what) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", what);
  }
}

typedef struct Observer {
  X86pMem *memory;
  uint32_t address;
  uint32_t size;
  uint32_t before;
  unsigned count;
} Observer;

static void observe(uint32_t address, uint32_t size, void *context) {
  Observer *observer = context;
  observer->address = address;
  observer->size = size;
  ++observer->count;
  check(x86p_mem_read(observer->memory, address, 4, &observer->before), "observer sees complete prewrite span");
}

static void spans_and_faults(void) {
  uint8_t first[4096] = {0}, second[4096] = {0}, high[8] = {0};
  uint8_t bytes[8], expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t *pointer = high;
  uint32_t value = 0, address = 0;
  X86pSparseMem *sparse = x86p_sparse_create();
  X86pMem mem = {.sparse = sparse};
  Observer observer = {.memory = &mem};
  check(sparse != NULL, "create sparse owner");
  if (!sparse) {
    return;
  }
  /* Insert deliberately out of guest order. */
  check(x86p_sparse_map(sparse, 0x2000, second, sizeof second), "map second page");
  check(x86p_sparse_map(sparse, 0x1000, first, sizeof first), "map first page");
  check(x86p_sparse_map(sparse, UINT32_MAX - 7u, high, sizeof high), "map final guest byte");
  check(!x86p_sparse_map(sparse, 0x1fff, high, sizeof high), "overlapping mapping refuses");
  check(!x86p_sparse_map(sparse, UINT32_MAX - 3u, high, sizeof high), "wrapping mapping refuses");
  check(!x86p_sparse_map(sparse, 0x4000, NULL, 8), "null backing refuses");
  check(!x86p_sparse_map(sparse, 0x4000, high, 0), "empty mapping refuses");
  check(x86p_mem_readable_span(&mem, 0x1ffe, 4096) == 4096, "readable span crosses adjacent guest pages");
  check(x86p_mem_readable_span(&mem, 0x2ffe, 4) == 2, "readable span ends at first hole");
  check(x86p_mem_readable_span(&mem, UINT32_MAX - 1u, 4) == 2, "readable span cannot wrap to guest zero");
  check(!x86p_mem_resolve(&mem, 0x1ffe, 4, &pointer) && pointer == high,
        "native span crossing allocations refuses without changing output");
  check(x86p_mem_resolve(&mem, 0x1001, 10, &pointer) && pointer == first + 1, "native span resolves exact backing");
  check(x86p_sparse_guest_address(sparse, first + 3, 8, &address) && address == 0x1003,
        "native pointer converts to guest address");
  check(!x86p_sparse_guest_address(sparse, first + 4094, 4, &address), "reverse span crossing allocation refuses");
  check(x86p_mem_write_bytes(&mem, 0x1ffc, expected, sizeof expected), "eight-byte write crosses allocations");
  check(x86p_mem_read_bytes(&mem, 0x1ffc, bytes, sizeof bytes) && !memcmp(bytes, expected, sizeof bytes),
        "eight-byte read preserves all bytes across allocations");
  check(x86p_mem_read(&mem, 0x1ffe, 4, &value) && value == 0x06050403, "unaligned scalar read crosses pages");
  x86p_mem_set_write_observer(observe, &observer);
  check(x86p_mem_write(&mem, 0x1ffe, 4, 0xaabbccdd), "unaligned scalar write crosses pages");
  check(observer.count == 1 && observer.address == 0x1ffe && observer.size == 4 && observer.before == 0x06050403,
        "observer fires once before any split store");
  memset(bytes, 0xa5, sizeof bytes);
  check(!x86p_mem_read_bytes(&mem, 0x2ffc, bytes, sizeof bytes), "hole-crossing read refuses");
  check(bytes[0] == 0xa5 && bytes[7] == 0xa5, "failed read preserves entire destination");
  check(!x86p_mem_write(&mem, 0x2ffe, 4, 0xffffffff), "hole-crossing write refuses");
  check(second[4094] == 0 && second[4095] == 0 && observer.count == 1,
        "failed write changes no prefix and calls no observer");
  x86p_mem_set_write_observer(NULL, NULL);
  check(x86p_mem_write(&mem, UINT32_MAX, 1, 0x42) && high[7] == 0x42, "final byte accessible");
  check(!x86p_mem_write(&mem, UINT32_MAX, 2, 0x5555) && high[7] == 0x42, "wrapped scalar cannot modify last byte");
  check(!x86p_sparse_unmap(sparse, 0x1001, 4095), "partial unmap refuses");
  check(x86p_sparse_unmap(sparse, 0x2000, sizeof second), "exact unmap succeeds");
  check(!x86p_mem_ok(&mem, 0x1ffe, 4), "unmap exposes hole to all guest accessors");
  check(x86p_sparse_map(sparse, 0x2000, second, sizeof second), "removed range can be remapped");
  check(x86p_sparse_map(sparse, 0xa0000000, second, sizeof second), "explicit host alias maps");
  check(!x86p_sparse_guest_address(sparse, second, 1, &address), "ambiguous host alias reverse lookup refuses");
  x86p_sparse_destroy(sparse);
}

static void compare_contiguous(void) {
  uint8_t contiguous[96], a[32], b[32], c[32];
  X86pSparseMem *sparse = x86p_sparse_create();
  X86pMem flat = {.host = contiguous, .lo = 0x80000000, .size = sizeof contiguous};
  X86pMem scattered = {.sparse = sparse};
  unsigned offset, width;
  check(sparse != NULL, "create differential mapping");
  if (!sparse) {
    return;
  }
  check(x86p_sparse_map(sparse, flat.lo, a, sizeof a), "map differential first span");
  check(x86p_sparse_map(sparse, flat.lo + 32, b, sizeof b), "map differential second span");
  check(x86p_sparse_map(sparse, flat.lo + 64, c, sizeof c), "map differential third span");
  for (offset = 0; offset <= sizeof contiguous; ++offset) {
    for (width = 1; width <= 4; width *= 2) {
      uint8_t observed[96];
      uint32_t left = 0x55555555, right = left;
      uint32_t address = flat.lo + offset;
      memset(contiguous, 0x65, sizeof contiguous);
      memset(a, 0x65, sizeof a);
      memset(b, 0x65, sizeof b);
      memset(c, 0x65, sizeof c);
      check(x86p_mem_write(&flat, address, (int)width, offset) ==
                x86p_mem_write(&scattered, address, (int)width, offset),
            "scalar write admission equals contiguous");
      check(x86p_mem_read(&flat, address, (int)width, &left) ==
                    x86p_mem_read(&scattered, address, (int)width, &right) &&
                left == right,
            "scalar read admission and result equal contiguous");
      check(x86p_mem_read_bytes(&scattered, flat.lo, observed, sizeof observed) &&
                !memcmp(contiguous, observed, sizeof observed),
            "all memory bytes equal contiguous");
    }
  }
  x86p_sparse_destroy(sparse);
}

static void permissions_and_partial_lifetimes(void) {
  uint8_t backing[12288] = {0};
  X86pSparseMem *sparse = x86p_sparse_create();
  X86pMem memory = {.sparse = sparse};
  uint8_t *pointer = NULL;
  uint32_t value = 0;
  check(sparse != NULL, "create protected mapping owner");
  if (!sparse) {
    return;
  }
  check(x86p_sparse_map_access(sparse, 0x4000, backing, sizeof backing, 0), "reserve inaccessible backing");
  check(!x86p_mem_read(&memory, 0x4000, 1, &value) && !x86p_mem_write(&memory, 0x4000, 1, 1),
        "reservation refuses reads and writes");
  check(x86p_mem_accessible(&memory, 0x4000, sizeof backing, 0), "reservation retains mapping ownership");
  check(x86p_sparse_protect(sparse, 0x4000, sizeof backing, kX86pMemRead | kX86pMemWrite), "commit reservation");
  check(x86p_mem_write(&memory, 0x4fff, 4, 0x12345678), "write cross-page baseline");
  check(x86p_sparse_protect(sparse, 0x5000, 4096, 0), "decommit middle page");
  check(!x86p_mem_read(&memory, 0x4fff, 4, &value), "decommitted cross-page read refuses");
  check(!x86p_mem_write(&memory, 0x4fff, 4, 0xffffffff) && backing[4095] == 0x78,
        "decommitted cross-page store preserves prefix");
  check(x86p_mem_accessible(&memory, 0x4000, 4096, kX86pMemWrite) &&
            x86p_mem_accessible(&memory, 0x6000, 4096, kX86pMemRead),
        "middle protection preserves neighboring pages");
  check(x86p_sparse_protect(sparse, 0x5000, 4096, kX86pMemRead), "restore read-only page");
  check(x86p_mem_read(&memory, 0x4fff, 4, &value) && value == 0x12345678, "protection retains original page contents");
  check(!x86p_mem_write(&memory, 0x4fff, 4, 0xffffffff) && backing[4095] == 0x78,
        "read-only crossing store changes no bytes");
  check(x86p_mem_resolve(&memory, 0x4000, sizeof backing, &pointer) && pointer == backing,
        "native span crosses fragments from same original allocation");
  check(!x86p_sparse_protect(sparse, 0x6000, 8192, 0), "protection across hole refuses atomically");
  check(x86p_mem_accessible(&memory, 0x6000, 4096, kX86pMemRead), "failed protection preserves valid prefix");
  check(x86p_sparse_unmap_range(sparse, 0x5000, 4096), "release middle page");
  check(!x86p_sparse_protect(sparse, 0x5000, 4096, kX86pMemRead), "cannot recommit a released hole");
  check(x86p_sparse_host_in_use(sparse, backing, sizeof backing), "neighboring fragments retain allocation");
  check(x86p_sparse_unmap_range(sparse, 0x4000, sizeof backing),
        "release whole allocation also tolerates an earlier released subrange");
  check(!x86p_sparse_host_in_use(sparse, backing, sizeof backing), "last release returns backing lifetime");
  check(x86p_sparse_map_access(sparse, 0x4000, backing, sizeof backing, kX86pMemWrite), "map write-only region");
  check(x86p_mem_write(&memory, 0x4000, 1, 0x93) && !x86p_mem_read(&memory, 0x4000, 1, &value),
        "write-only access separates read and write permission");
  x86p_sparse_destroy(sparse);
}

int main(void) {
  spans_and_faults();
  compare_contiguous();
  permissions_and_partial_lifetimes();
  printf("sparse memory: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
