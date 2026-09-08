#include "memory_sparse.h"
#include "cpu.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct Mapping {
  uint32_t guest;
  uint32_t size;
  uint8_t *host;
  unsigned access;
  uint64_t allocation;
} Mapping;

struct X86pSparseMem {
  Mapping *mappings;
  size_t count;
  size_t capacity;
  uint64_t next_allocation;
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

int x86p_sparse_range_available(const X86pSparseMem *sparse, uint32_t guest, uint32_t size) {
  size_t at;
  if (!sparse || !size || guest > UINT32_MAX - (size - 1u)) {
    return 0;
  }
  at = upper_bound(sparse, guest);
  return !(at && guest - sparse->mappings[at - 1u].guest < sparse->mappings[at - 1u].size) &&
         !(at < sparse->count && sparse->mappings[at].guest - guest < size);
}

int x86p_sparse_map(X86pSparseMem *sparse, uint32_t guest, void *host, uint32_t size) {
  return x86p_sparse_map_access(sparse, guest, host, size, kX86pMemRead | kX86pMemWrite);
}

int x86p_sparse_map_access(X86pSparseMem *sparse, uint32_t guest, void *host, uint32_t size, unsigned access) {
  size_t at;
  if (!sparse || !host || !size || access & ~(kX86pMemRead | kX86pMemWrite) || sparse->next_allocation == UINT64_MAX ||
      guest > UINT32_MAX - (size - 1u) || (uintptr_t)host > UINTPTR_MAX - (size - 1u)) {
    return 0;
  }
  if (!x86p_sparse_range_available(sparse, guest, size)) {
    return 0;
  }
  at = upper_bound(sparse, guest);
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
  sparse->mappings[at] = (Mapping){guest, size, host, access, ++sparse->next_allocation};
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
  return x86p_sparse_span_access(sparse, guest, max, kX86pMemRead, host);
}

uint32_t
x86p_sparse_span_access(const X86pSparseMem *sparse, uint32_t guest, uint32_t max, unsigned access, uint8_t **host) {
  size_t at;
  const Mapping *mapping;
  uint32_t offset, available;
  if (!sparse || !max || !(at = upper_bound(sparse, guest))) {
    return 0;
  }
  mapping = &sparse->mappings[at - 1u];
  offset = guest - mapping->guest;
  if (offset >= mapping->size || (mapping->access & access) != access) {
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

static int whole_span(const X86pSparseMem *sparse, uint32_t guest, uint32_t size) {
  uint64_t cursor = guest, end = cursor + size;
  if (!size || end > (UINT64_C(1) << 32)) {
    return 0;
  }
  while (cursor < end) {
    uint32_t count = x86p_sparse_span_access(sparse, (uint32_t)cursor, (uint32_t)(end - cursor), 0, NULL);
    if (!count) {
      return 0;
    }
    cursor += count;
  }
  return 1;
}

/* Only the two boundary ranges can split, so count+2 is a complete bounded
 * transaction buffer. Publish it only after every candidate is constructed. */
static int change_range(X86pSparseMem *sparse, uint32_t guest, uint32_t size, unsigned access, int remove) {
  Mapping *changed;
  size_t i, count = 0, capacity;
  uint64_t end = (uint64_t)guest + size;
  if (!sparse || access & ~(kX86pMemRead | kX86pMemWrite) || !size || end > (UINT64_C(1) << 32) ||
      (!remove && !whole_span(sparse, guest, size)) || sparse->count > SIZE_MAX / sizeof(Mapping) - 2u) {
    return 0;
  }
  capacity = sparse->count + 2u;
  changed = malloc(capacity * sizeof *changed);
  if (!changed) {
    return 0;
  }
  for (i = 0; i < sparse->count; ++i) {
    Mapping mapping = sparse->mappings[i];
    uint64_t mapping_end = (uint64_t)mapping.guest + mapping.size;
    if (mapping_end <= guest || mapping.guest >= end) {
      changed[count++] = mapping;
      continue;
    }
    if (mapping.guest < guest) {
      Mapping prefix = mapping;
      prefix.size = guest - mapping.guest;
      changed[count++] = prefix;
    }
    if (!remove) {
      uint32_t first = mapping.guest < guest ? guest : mapping.guest;
      Mapping middle = mapping;
      middle.guest = first;
      middle.host += first - mapping.guest;
      middle.size = (uint32_t)((mapping_end < end ? mapping_end : end) - first);
      middle.access = access;
      changed[count++] = middle;
    }
    if (mapping_end > end) {
      Mapping suffix = mapping;
      suffix.guest = (uint32_t)end;
      suffix.host += end - mapping.guest;
      suffix.size = (uint32_t)(mapping_end - end);
      changed[count++] = suffix;
    }
  }
  free(sparse->mappings);
  sparse->mappings = changed;
  sparse->count = count;
  sparse->capacity = capacity;
  return 1;
}

int x86p_sparse_protect(X86pSparseMem *sparse, uint32_t guest, uint32_t size, unsigned access) {
  return change_range(sparse, guest, size, access, 0);
}

int x86p_sparse_unmap_range(X86pSparseMem *sparse, uint32_t guest, uint32_t size) {
  return change_range(sparse, guest, size, 0, 1);
}

int x86p_sparse_host_in_use(const X86pSparseMem *sparse, const void *host, size_t size) {
  uintptr_t start = (uintptr_t)host;
  size_t i;
  if (!sparse || !host || !size || start > UINTPTR_MAX - (size - 1u)) {
    return 0;
  }
  for (i = 0; i < sparse->count; ++i) {
    const Mapping *mapping = &sparse->mappings[i];
    uintptr_t mapped = (uintptr_t)mapping->host;
    if (mapped >= start ? mapped - start < size : start - mapped < mapping->size) {
      return 1;
    }
  }
  return 0;
}

int x86p_sparse_resolve(const X86pSparseMem *sparse, uint32_t guest, uint32_t size, unsigned access, uint8_t **host) {
  size_t at;
  uint64_t cursor = guest, end = cursor + size, allocation;
  uint8_t *start = NULL;
  if (!sparse || !host || !size || end > (UINT64_C(1) << 32) || !(at = upper_bound(sparse, guest))) {
    return 0;
  }
  allocation = sparse->mappings[at - 1u].allocation;
  while (cursor < end && at <= sparse->count) {
    const Mapping *mapping = &sparse->mappings[at - 1u];
    uint32_t offset = (uint32_t)cursor - mapping->guest;
    uint32_t available;
    if (cursor < mapping->guest || offset >= mapping->size || mapping->allocation != allocation ||
        (mapping->access & access) != access) {
      return 0;
    }
    if (!start) {
      start = mapping->host + offset;
    } else if (mapping->host + offset != start + (cursor - guest)) {
      return 0;
    }
    available = mapping->size - offset;
    cursor += available < end - cursor ? available : end - cursor;
    ++at;
  }
  if (cursor != end) {
    return 0;
  }
  *host = start;
  return 1;
}
