#include "cpu.h"
#include "memory_sparse.h"

#include <string.h>

static X86pMemWriteObserver g_write_observer;
static void *g_write_observer_user;

/* Query one backing span. The identity-mapping desktop contract uses integer
 * address addition because adding to a null C pointer is undefined. */
static uint32_t backing_span(const X86pMem *m, uint32_t addr, uint32_t max, unsigned access, uint8_t **out) {
  uint32_t offset, room;
  if (!m || !max || access & ~(kX86pMemRead | kX86pMemWrite)) {
    return 0;
  }
  if (m->sparse) {
    return x86p_sparse_span_access(m->sparse, addr, max, access, out);
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
  if (m->perms) {
    /*
     * Exact permissions without a host VM. The span STOPS where the permission
     * changes, because returning past that point would report bytes this
     * mapping has not agreed to -- the callers above loop over spans precisely
     * so a change mid-range is a shorter answer rather than a wrong one.
     *
     * It does NOT stop at every page boundary. `x86p_mem_resolve` demands the
     * whole range in one span, and a one-page ceiling refused every multi-page
     * request, silently turning the bulk copy and fill behind REP MOVS/STOS
     * into element-at-a-time work on exactly the mappings that have a table.
     *
     * A zero byte is an unmapped page, so `!have` refuses even the access == 0
     * query, which asks whether backing exists at all.
     */
    const uint32_t page_size = 1u << m->page_shift;
    uint32_t page = offset >> m->page_shift;
    uint32_t granted = page_size - (offset & (page_size - 1u));
    const unsigned first = m->perms[page];
    if (!first || (first & access) != access) {
      return 0;
    }
    while (granted < room) {
      /* In range: granted < room means offset + granted is still inside the
         mapping, so the page holding it has an entry. */
      const unsigned have = m->perms[++page];
      if (!have || (have & access) != access) {
        break;
      }
      granted += page_size;
    }
    if (room > granted) {
      room = granted;
    }
  }
  if (out) {
    /* Identity mappings may have a null base.
     * NOLINTNEXTLINE(performance-no-int-to-ptr) */
    *out = (uint8_t *)((uintptr_t)m->host + (uintptr_t)offset);
  }
  return room;
}

static uint32_t accessible_span(const X86pMem *m, uint32_t addr, uint32_t max, unsigned access) {
  uint32_t total = 0;
  while (total < max) {
    uint32_t n = backing_span(m, addr, max - total, access, NULL);
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

uint32_t x86p_mem_readable_span(const X86pMem *m, uint32_t addr, uint32_t max) {
  return accessible_span(m, addr, max, kX86pMemRead);
}

int x86p_mem_accessible(const X86pMem *m, uint32_t addr, uint32_t n, unsigned access) {
  return n && addr <= UINT32_MAX - (n - 1u) && accessible_span(m, addr, n, access) == n;
}

int x86p_mem_resolve(const X86pMem *m, uint32_t addr, uint32_t n, uint8_t **out) {
  uint8_t *host = NULL;
  if (m && m->sparse) {
    return x86p_sparse_resolve(m->sparse, addr, n, kX86pMemRead, out);
  }
  if (!out || !n || backing_span(m, addr, n, kX86pMemRead, &host) != n) {
    return 0;
  }
  *out = host;
  return 1;
}

int x86p_mem_ok(const X86pMem *m, uint32_t addr, int w) {
  return (w == 1 || w == 2 || w == 4) && x86p_mem_accessible(m, addr, (uint32_t)w, kX86pMemRead);
}

/*
 * The whole span in one walk, when it is one span.
 *
 * Both bulk calls below have to prove the WHOLE range accessible before they
 * touch anything -- the write so a partial failure cannot leave the guest
 * half-mutated or the observer misinformed, the read so a caller that gets a
 * refusal is not looking at a partly filled buffer. They proved it with
 * x86p_mem_accessible, which walks the permission structure, and then walked
 * it a second time through backing_span to do the copying.
 *
 * One walk answers both questions when the range does not straddle anything:
 * a span covering all n bytes IS the proof, and it comes with the pointer.
 * Measured on the browser's Dead Zone route, where this is the x87 operand
 * path: x86p_x87_read_value fell from 13.59% of the guest worker to 10.95%
 * and backing_span from 5.66% to 3.50%.
 *
 * A range that DOES straddle costs one walk more than before, because the
 * old two-walk path still runs after this one declines. That case is the
 * sparse mode crossing separate host allocations; the contiguous mode a
 * browser uses answers here.
 */
static int one_span(const X86pMem *m, uint32_t addr, uint32_t n, unsigned access, uint8_t **host) {
  if (!n || addr > UINT32_MAX - (n - 1u)) {
    return 0;
  }
  if (m && m->sparse) {
    return x86p_sparse_resolve(m->sparse, addr, n, access, host);
  }
  return backing_span(m, addr, n, access, host) == n;
}

int x86p_mem_read_bytes(const X86pMem *m, uint32_t addr, void *dst, uint32_t n) {
  uint8_t *output = dst;
  uint8_t *host = NULL;
  if (!dst) {
    return 0;
  }
  if (one_span(m, addr, n, kX86pMemRead, &host)) {
    memcpy(output, host, n);
    return 1;
  }
  if (!x86p_mem_accessible(m, addr, n, kX86pMemRead)) {
    return 0;
  }
  while (n) {
    uint8_t *host = NULL;
    uint32_t count = backing_span(m, addr, n, kX86pMemRead, &host);
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
  uint8_t *host = NULL;
  if (!src) {
    return 0;
  }
  if (one_span(m, addr, n, kX86pMemWrite, &host)) {
    /* The span is the proof that every byte is writable, so the observer is
       told before the first byte moves exactly as it is below. */
    if (g_write_observer) {
      g_write_observer(addr, n, g_write_observer_user);
    }
    memcpy(host, input, n);
    return 1;
  }
  if (!x86p_mem_accessible(m, addr, n, kX86pMemWrite)) {
    return 0;
  }
  if (g_write_observer) {
    g_write_observer(addr, n, g_write_observer_user);
  }
  while (n) {
    uint8_t *host = NULL;
    uint32_t count = backing_span(m, addr, n, kX86pMemWrite, &host);
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

/* Whether every byte of a `w`-byte fill unit is the same. */
static int unit_repeats_one_byte(const uint8_t *unit, uint32_t w) {
  for (uint32_t i = 1u; i < w; ++i) {
    if (unit[i] != unit[0]) {
      return 0;
    }
  }
  return 1;
}

int x86p_mem_fill(const X86pMem *m, uint32_t addr, const uint8_t *unit, uint32_t w, uint32_t count) {
  uint8_t *destination = NULL;
  uint64_t bytes = (uint64_t)w * count;
  if (!unit || !count || (w != 1u && w != 2u && w != 4u) || bytes > UINT32_MAX) {
    return 0;
  }
  /* The same refusals as the bulk copy: a sparse mapping can alias, and an
     observer is entitled to see one notification per guest store. */
  if (!m || m->sparse || g_write_observer || !x86p_mem_resolve(m, addr, (uint32_t)bytes, &destination)) {
    return 0;
  }
  /* A unit of one repeated byte -- every byte fill and every zeroing STOSD --
     is a memset. Any other unit is written once and then doubled from what is
     already written: a per-element memcpy of a runtime width is a libc call
     per element, which a 4 MB STOSD pays a million times. */
  if (unit_repeats_one_byte(unit, w)) {
    memset(destination, unit[0], (size_t)bytes);
    return 1;
  }
  memcpy(destination, unit, w);
  for (size_t filled = w; filled < bytes;) {
    const size_t chunk = filled < bytes - filled ? filled : (size_t)(bytes - filled);
    memcpy(destination + filled, destination, chunk);
    filled += chunk;
  }
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
