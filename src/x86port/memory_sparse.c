#include "memory_sparse.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct Mapping {
  uint32_t guest;
  uint32_t size;
  uint8_t *host;
} Mapping;

struct X86pSparseMem {
  Mapping *mappings;
  size_t count;
  size_t capacity;
};

X86pSparseMem *x86p_sparse_create(void) {
  return calloc(1u, sizeof(X86pSparseMem));
}

void x86p_sparse_destroy(X86pSparseMem *sparse) {
  if (sparse) {
    free(sparse->mappings);
    free(sparse);
  }
}

/* First range starting strictly above guest; its predecessor can contain it. */
static size_t upper_bound(const X86pSparseMem *sparse, uint32_t guest) {
  size_t lo = 0u, hi = sparse->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (sparse->mappings[mid].guest <= guest) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return lo;
}

int x86p_sparse_map(X86pSparseMem *sparse, uint32_t guest, void *host, uint32_t size) {
  size_t at;
  if (!sparse || !host || !size || guest > UINT32_MAX - (size - 1u) || (uintptr_t)host > UINTPTR_MAX - (size - 1u)) {
    return 0;
  }
  at = upper_bound(sparse, guest);
  if (at && guest - sparse->mappings[at - 1u].guest < sparse->mappings[at - 1u].size) {
    return 0;
  }
  if (at < sparse->count && sparse->mappings[at].guest - guest < size) {
    return 0;
  }
  if (sparse->count == sparse->capacity) {
    size_t capacity = sparse->capacity ? sparse->capacity * 2u : 16u;
    Mapping *mappings;
    if (capacity < sparse->capacity || capacity > SIZE_MAX / sizeof *mappings) {
      return 0;
    }
    mappings = realloc(sparse->mappings, capacity * sizeof *mappings);
    if (!mappings) {
      return 0;
    }
    sparse->mappings = mappings;
    sparse->capacity = capacity;
  }
  memmove(sparse->mappings + at + 1u, sparse->mappings + at, (sparse->count - at) * sizeof(Mapping));
  sparse->mappings[at] = (Mapping){guest, size, host};
  ++sparse->count;
  return 1;
}

int x86p_sparse_unmap(X86pSparseMem *sparse, uint32_t guest, uint32_t size) {
  size_t at;
  if (!sparse || !(at = upper_bound(sparse, guest))) {
    return 0;
  }
  --at;
  if (sparse->mappings[at].guest != guest || sparse->mappings[at].size != size) {
    return 0;
  }
  --sparse->count;
  memmove(sparse->mappings + at, sparse->mappings + at + 1u, (sparse->count - at) * sizeof(Mapping));
  return 1;
}

uint32_t x86p_sparse_span(const X86pSparseMem *sparse, uint32_t guest, uint32_t max, uint8_t **host) {
  size_t at;
  const Mapping *mapping;
  uint32_t offset, available;
  if (!sparse || !max || !(at = upper_bound(sparse, guest))) {
    return 0;
  }
  mapping = &sparse->mappings[at - 1u];
  offset = guest - mapping->guest;
  if (offset >= mapping->size) {
    return 0;
  }
  available = mapping->size - offset;
  if (host) {
    *host = mapping->host + offset;
  }
  return available < max ? available : max;
}

int x86p_sparse_guest_address(const X86pSparseMem *sparse, const void *host, uint32_t size, uint32_t *guest) {
  size_t i;
  int found = 0;
  uint32_t result = 0;
  uintptr_t address = (uintptr_t)host;
  if (!sparse || !host || !size || !guest || address > UINTPTR_MAX - (size - 1u)) {
    return 0;
  }
  for (i = 0; i < sparse->count; ++i) {
    const Mapping *mapping = &sparse->mappings[i];
    uintptr_t base = (uintptr_t)mapping->host;
    uintptr_t offset = address - base;
    if (address >= base && offset < mapping->size && size <= mapping->size - offset) {
      if (found) {
        return 0;
      }
      found = 1;
      result = mapping->guest + (uint32_t)offset;
    }
  }
  if (found) {
    *guest = result;
  }
  return found;
}
