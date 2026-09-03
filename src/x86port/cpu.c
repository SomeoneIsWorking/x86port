/* cpu.c -- see cpu.h for the two traps this file exists to get right. */
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void x86p_cpu_reset(X86pCpu *cpu) {
  if (!cpu) {
    return;
  }
  memset(cpu, 0, sizeof *cpu);
  /* An all-zero FPU is a stack whose registers are all tagged VALID and whose
     control word selects single precision -- neither of which is the state a
     process starts in. The FPU owns its own reset. */
  x86p_x87_reset(&cpu->x87);
}

/* ---- memory ------------------------------------------------------------- */

/*
 * The host address of a guest address, as INTEGER arithmetic.
 *
 * `host` is the host address of guest address `lo`, and a consumer that maps
 * the guest at the host's own low addresses has a host of 0 for guest 0 --
 * pc/xmen2 does exactly that when the platform lets it map the low 4 GB, so
 * the identity mapping is a real configuration and not a mistake. Adding an
 * offset to a null pointer is undefined behaviour even when the arithmetic is
 * obvious, so the addition happens on uintptr_t and the result is converted
 * once.
 *
 * This is also why "is this mapping configured" is asked as `size == 0` rather
 * than as a null host: a null host with a real size is a legitimate identity
 * mapping, and refusing it would make every access to a correctly configured
 * guest fail as a fault.
 */
static uint8_t *x86p_mem_at(const X86pMem *m, uint32_t addr) {
  return (uint8_t *)((uintptr_t)m->host + (uintptr_t)(addr - m->lo));
}

/* The bounds question alone, for any span. Separated from x86p_mem_ok so that
   the width restriction there -- 1, 2 or 4, which is what an integer operand
   can be -- stays a statement about operands and not about the mapping. */
static int span_ok(const X86pMem *m, uint32_t addr, uint32_t n) {
  uint32_t off;
  if (!m || m->size == 0 || n == 0) {
    return 0;
  }
  if (addr < m->lo) {
    return 0;
  }
  off = addr - m->lo;
  if (off >= m->size) {
    return 0;
  }
  /* Computed as a subtraction, never as `off + n <= size`, which overflows for
     an offset near 2^32 and reports a wildly out-of-range access as fine. */
  if (n > m->size - off) {
    return 0;
  }
  /* And the access must not wrap the guest address space, which is a separate
     question from fitting in the mapping. */
  if (addr > UINT32_MAX - (n - 1)) {
    return 0;
  }
  return 1;
}

int x86p_mem_read_bytes(const X86pMem *m, uint32_t addr, void *dst, uint32_t n) {
  if (!dst || !span_ok(m, addr, n)) {
    return 0;
  }
  memcpy(dst, x86p_mem_at(m, addr), n);
  return 1;
}

uint32_t x86p_mem_readable_span(const X86pMem *m, uint32_t addr, uint32_t max) {
  uint32_t off, room;
  if (!m || m->size == 0u || max == 0u || addr < m->lo) {
    return 0u;
  }
  off = addr - m->lo;
  if (off >= m->size) {
    return 0u;
  }
  room = m->size - off;
  return room < max ? room : max;
}

static X86pMemWriteObserver g_write_observer;
static void *g_write_observer_user;

void x86p_mem_set_write_observer(X86pMemWriteObserver fn, void *user) {
  g_write_observer = fn;
  g_write_observer_user = user;
}

int x86p_mem_write_bytes(const X86pMem *m, uint32_t addr, const void *src, uint32_t n) {
  if (!src || !span_ok(m, addr, n)) {
    return 0;
  }
  if (g_write_observer) {
    g_write_observer(addr, n, g_write_observer_user);
  }
  memcpy(x86p_mem_at(m, addr), src, n);
  return 1;
}

int x86p_mem_ok(const X86pMem *m, uint32_t addr, int w) {
  uint32_t off;
  if (!m || m->size == 0 || (w != 1 && w != 2 && w != 4)) {
    return 0;
  }
  if (addr < m->lo) {
    return 0;
  }
  off = addr - m->lo;
  if (off >= m->size) {
    return 0;
  }
  /* Computed as a subtraction, never as `off + w <= size`, which overflows for
     an offset near 2^32 and reports a wildly out-of-range access as fine. */
  if ((uint32_t)w > m->size - off) {
    return 0;
  }
  /* And the access must not wrap the guest address space, which is a separate
     question from fitting in the mapping. */
  if (addr > UINT32_MAX - (uint32_t)(w - 1)) {
    return 0;
  }
  return 1;
}

int x86p_mem_read(const X86pMem *m, uint32_t addr, int w, uint32_t *out) {
  const uint8_t *p;
  uint32_t v = 0;
  int i;
  if (!out || !x86p_mem_ok(m, addr, w)) {
    return 0;
  }
  p = x86p_mem_at(m, addr);
  /* Assembled explicitly, little-endian. A cast through a host pointer happens
     to be right on x86 and is a bug waiting for the ARM64 host S042 plans
     for -- and it would also be an unaligned access the guest is allowed to
     make and C is not. */
  for (i = w - 1; i >= 0; i--) {
    v = (v << 8) | p[i];
  }
  *out = v;
  return 1;
}

int x86p_mem_write(const X86pMem *m, uint32_t addr, int w, uint32_t value) {
  uint8_t *p;
  int i;
  if (!x86p_mem_ok(m, addr, w)) {
    return 0;
  }
  if (g_write_observer) {
    g_write_observer(addr, (uint32_t)w, g_write_observer_user);
  }
  p = x86p_mem_at(m, addr);
  for (i = 0; i < w; i++) {
    p[i] = (uint8_t)(value & 0xFFu);
    value >>= 8;
  }
  return 1;
}

/* ---- registers ---------------------------------------------------------- */

/*
 * Byte registers 4-7 are the HIGH bytes of EAX..EBX, not registers 4-7. This
 * is the single most consequential three-line function in the file: getting it
 * wrong reads a stack pointer where the guest asked for a character, and it
 * looks correct.
 */
int x86p_byte_reg(int index, int *shift) {
  if (index < 4) {
    *shift = 0;
    return index;
  }
  *shift = 8;
  return index - 4;
}

uint32_t x86p_reg_read(const X86pCpu *cpu, int index, int w) {
  if (!cpu || index < 0) {
    return 0;
  }
  if (w == 1) {
    int shift;
    int r;
    if (index >= 8) {
      return 0;
    }
    r = x86p_byte_reg(index, &shift);
    return (cpu->reg[r] >> shift) & 0xFFu;
  }
  if (index >= (int)kX86pRegCount) {
    return 0;
  }
  if (w == 2) {
    return cpu->reg[index] & 0xFFFFu;
  }
  if (w == 4) {
    return cpu->reg[index];
  }
  fprintf(stderr, "x86p_reg_read: width %d is not 1, 2 or 4\n", w);
  abort();
}

void x86p_reg_write(X86pCpu *cpu, int index, int w, uint32_t value) {
  if (!cpu || index < 0) {
    return;
  }
  if (w == 1) {
    int shift, r;
    if (index >= 8) {
      return;
    }
    r = x86p_byte_reg(index, &shift);
    /* PRESERVES the other 24 bits. Writing the whole register here is the most
       natural mistake in an interpreter, looks right at every call site, and
       corrupts a value the guest is still using. */
    cpu->reg[r] = (cpu->reg[r] & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
    return;
  }
  if (index >= (int)kX86pRegCount) {
    return;
  }
  if (w == 2) {
    /* Likewise: MOV AX, 0 leaves the top half of EAX alone. */
    cpu->reg[index] = (cpu->reg[index] & 0xFFFF0000u) | (value & 0xFFFFu);
    return;
  }
  if (w == 4) {
    cpu->reg[index] = value;
    return;
  }
  fprintf(stderr, "x86p_reg_write: width %d is not 1, 2 or 4\n", w);
  abort();
}

const char *x86p_reg_name(int index, int w) {
  static const char *k8[] = {"AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH"};
  static const char *k16[] = {"AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"};
  static const char *k32[] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
  if (index < 0 || index >= 8) {
    return "?";
  }
  switch (w) {
  case 1:
    return k8[index];
  case 2:
    return k16[index];
  case 4:
    return k32[index];
  default:
    return "?";
  }
}

/* ---- stack -------------------------------------------------------------- */

int x86p_push32(X86pCpu *cpu, const X86pMem *m, uint32_t value) {
  uint32_t esp;
  if (!cpu) {
    return 0;
  }
  esp = cpu->reg[kX86pEsp] - 4u;
  /* The store is attempted BEFORE ESP moves, so a faulting push leaves the
     stack pointer where it was rather than past a value it never stored --
     which would corrupt every subsequent frame while the fault report blamed
     the address. */
  if (!x86p_mem_write(m, esp, 4, value)) {
    return 0;
  }
  cpu->reg[kX86pEsp] = esp;
  return 1;
}

int x86p_pop32(X86pCpu *cpu, const X86pMem *m, uint32_t *out) {
  uint32_t v;
  if (!cpu || !out) {
    return 0;
  }
  if (!x86p_mem_read(m, cpu->reg[kX86pEsp], 4, &v)) {
    return 0;
  }
  cpu->reg[kX86pEsp] += 4u;
  *out = v;
  return 1;
}
