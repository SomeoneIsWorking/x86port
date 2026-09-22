/*
 * jit_x64_harness.c -- see jit_x64_harness.h.
 */
#include "jit_x64_harness.h"

#include "code_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/*
 * The guest mapping sits at the END of a page, with an unreadable page after
 * it.
 *
 * A static array would let an over-wide HOST read run past the guest mapping
 * into memory that happens to be readable, and the store that follows would
 * still write the right number of bytes -- so a one-byte guest load emitted as
 * a four-byte host load passes every comparison while reading three bytes it
 * has no right to. Measured: that mutation SURVIVED until this guard page
 * existed. Here the same code takes SIGSEGV at the mapping's last address.
 */

uint8_t *jit_x64_harness_guest_init(void) {
#if defined(_WIN32)
  SYSTEM_INFO info;
  size_t page;
  size_t span;
  DWORD old_protection;
  uint8_t *base;
  GetSystemInfo(&info);
  page = (size_t)info.dwPageSize;
  span = page * 2u;
  base = (uint8_t *)VirtualAlloc(NULL, span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!base) {
    printf("FATAL: could not reserve %zu bytes for the guest mapping and its guard\n", span);
    exit(1);
  }
  if (!VirtualProtect(base + page, page, PAGE_NOACCESS, &old_protection)) {
    printf("FATAL: could not make the guard page unreadable; an over-wide read would go unnoticed\n");
    exit(1);
  }
#else
  long page = sysconf(_SC_PAGESIZE);
  size_t span;
  uint8_t *base;
  if (page <= 0) {
    printf("FATAL: cannot determine the page size, so no guard page can be placed\n");
    exit(1);
  }
  span = (size_t)page * 2u;
  base = (uint8_t *)mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    printf("FATAL: could not map %zu bytes for the guest mapping and its guard\n", span);
    exit(1);
  }
  if (mprotect(base + page, (size_t)page, PROT_NONE) != 0) {
    printf("FATAL: could not make the guard page unreadable; an over-wide read would go unnoticed\n");
    exit(1);
  }
#endif
  if (GUEST_SIZE > (size_t)page) {
    printf("FATAL: the guest mapping is larger than a page and cannot end on one\n");
    exit(1);
  }
  return base + page - GUEST_SIZE;
}

X86pMem jit_x64_harness_mem(uint8_t *guest) {
  X86pMem m = {0};
  m.host = guest;
  m.lo = GUEST_BASE;
  m.size = GUEST_SIZE;
  return m;
}

/* ---- host code memory ---------------------------------------------------- */

static JcCodeRegion g_code_region;
static int g_code_published;

void *jit_x64_harness_code_alloc(size_t n) {
  char reason[192] = {0};
  memset(&g_code_region, 0, sizeof g_code_region);
  g_code_published = 0;
  if (jc_code_region_create(n, &g_code_region, reason, (unsigned)sizeof reason) != kJcCodeOk) {
    printf("    REFUSED: code memory: %s\n", reason);
    return NULL;
  }
  return g_code_region.write;
}

void jit_x64_harness_code_free(void *code, size_t n) {
  (void)code;
  (void)n;
  jc_code_region_destroy(&g_code_region);
  g_code_published = 0;
}

X86pJitStatus jit_x64_harness_translate(const X86pMem *mem,
                                        uint32_t eip,
                                        void *code,
                                        size_t code_cap,
                                        X86pJitBlock *block,
                                        char *reason,
                                        unsigned reason_len) {
  X86pJitStatus status;
  if (code != g_code_region.write) {
    snprintf(reason, reason_len, "code pointer is not the active writable region");
    return kX86pJitOutOfSpace;
  }
  if (g_code_published && jc_code_begin_write(&g_code_region) != kJcCodeOk) {
    snprintf(reason, reason_len, "could not reopen code region for writing");
    return kX86pJitOutOfSpace;
  }
  g_code_published = 0;
  status = x86p_jit_translate(mem, eip, code, code_cap, block, reason, reason_len);
  if (status != kX86pJitOk) {
    return status;
  }
  if (jc_code_publish(&g_code_region, block->host_bytes) != kJcCodeOk) {
    snprintf(reason, reason_len, "could not publish translated code");
    return kX86pJitOutOfSpace;
  }
  block->entry = g_code_region.exec;
  g_code_published = 1;
  return status;
}
