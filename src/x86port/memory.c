#include "cpu.h"
#include "memory_sparse.h"

#include <string.h>

static X86pMemWriteObserver g_write_observer;
static void *g_write_observer_user;

/* Query one backing span. The identity-mapping desktop contract uses integer
 * address addition because adding to a null C pointer is undefined. */
static uint32_t backing_span(const X86pMem *m, uint32_t addr, uint32_t max, uint8_t **out) {
  uint32_t offset, room;
  if (!m || !max) {
    return 0;
  }
  if (m->sparse) {
    return x86p_sparse_span(m->sparse, addr, max, out);
  }
  if (!m->size || addr < m->lo) {
    return 0;
  }
  offset = addr - m->lo;
  if (offset >= m->size) {
    return 0;
  }
  room = m->size - offset;
  if (room > max) {
    room = max;
  }
  if (addr > UINT32_MAX - (room - 1u)) {
    room = UINT32_MAX - addr + 1u;
  }
  if (out) {
    /* Identity mappings may have a null base.
     * NOLINTNEXTLINE(performance-no-int-to-ptr) */
    *out = (uint8_t *)((uintptr_t)m->host + (uintptr_t)offset);
  }
  return room;
}

uint32_t x86p_mem_readable_span(const X86pMem *m, uint32_t addr, uint32_t max) {
  uint32_t total = 0;
  while (total < max) {
    uint32_t n = backing_span(m, addr, max - total, NULL);
    if (!n) {
      break;
    }
    total += n;
    if (n > UINT32_MAX - addr) {
      break;
    }
    addr += n;
  }
  return total;
}

static int span_ok(const X86pMem *m, uint32_t addr, uint32_t n) {
  return n && addr <= UINT32_MAX - (n - 1u) && x86p_mem_readable_span(m, addr, n) == n;
}

int x86p_mem_resolve(const X86pMem *m, uint32_t addr, uint32_t n, uint8_t **out) {
  uint8_t *host = NULL;
  if (!out || !n || backing_span(m, addr, n, &host) != n) {
    return 0;
  }
  *out = host;
  return 1;
}

int x86p_mem_ok(const X86pMem *m, uint32_t addr, int w) {
  return (w == 1 || w == 2 || w == 4) && span_ok(m, addr, (uint32_t)w);
}

int x86p_mem_read_bytes(const X86pMem *m, uint32_t addr, void *dst, uint32_t n) {
  uint8_t *output = dst;
  if (!dst || !span_ok(m, addr, n)) {
    return 0;
  }
  while (n) {
    uint8_t *host = NULL;
    uint32_t count = backing_span(m, addr, n, &host);
    memcpy(output, host, count);
    output += count;
    addr += count;
    n -= count;
  }
  return 1;
}

void x86p_mem_set_write_observer(X86pMemWriteObserver fn, void *user) {
  g_write_observer = fn;
  g_write_observer_user = user;
}

int x86p_mem_write_bytes(const X86pMem *m, uint32_t addr, const void *src, uint32_t n) {
  const uint8_t *input = src;
  if (!src || !span_ok(m, addr, n)) {
    return 0;
  }
  if (g_write_observer) {
    g_write_observer(addr, n, g_write_observer_user);
  }
  while (n) {
    uint8_t *host = NULL;
    uint32_t count = backing_span(m, addr, n, &host);
    memcpy(host, input, count);
    input += count;
    addr += count;
    n -= count;
  }
  return 1;
}

int x86p_mem_copy_disjoint(const X86pMem *m, uint32_t dst, uint32_t src, uint32_t n) {
  uint8_t *source = NULL, *destination = NULL;
  /* Sparse aliases can make guest-disjoint regions host-overlap. Refuse the
   * bulk optimization there; the string owner retains ordered element copies. */
  if (!m || m->sparse || g_write_observer || !x86p_mem_resolve(m, src, n, &source) ||
      !x86p_mem_resolve(m, dst, n, &destination)) {
    return 0;
  }
  if ((uint64_t)src < (uint64_t)dst + n && (uint64_t)dst < (uint64_t)src + n) {
    return 0;
  }
  memcpy(destination, source, n);
  return 1;
}

int x86p_mem_read(const X86pMem *m, uint32_t addr, int w, uint32_t *out) {
  uint8_t bytes[4];
  uint32_t value = 0;
  int i;
  if (!out || (w != 1 && w != 2 && w != 4) || !x86p_mem_read_bytes(m, addr, bytes, (uint32_t)w)) {
    return 0;
  }
  for (i = w - 1; i >= 0; --i) {
    value = (value << 8) | bytes[i];
  }
  *out = value;
  return 1;
}

int x86p_mem_write(const X86pMem *m, uint32_t addr, int w, uint32_t value) {
  uint8_t bytes[4];
  int i;
  if (w != 1 && w != 2 && w != 4) {
    return 0;
  }
  for (i = 0; i < w; ++i) {
    bytes[i] = (uint8_t)value;
    value >>= 8;
  }
  return x86p_mem_write_bytes(m, addr, bytes, (uint32_t)w);
}
