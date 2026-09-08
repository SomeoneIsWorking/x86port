#include "cpuid.h"
/* cpu.c -- see cpu.h for the two traps this file exists to get right. */
#include "bit_ops.h"
#include "cpu.h"
#include "diagnostic.h"

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
  x86p_diagnostic_fatalf("cpu", "x86p_reg_read: width %d is not 1, 2 or 4", w);
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
  x86p_diagnostic_fatalf("cpu", "x86p_reg_write: width %d is not 1, 2 or 4", w);
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

void x86p_cpu_sahf(X86pCpu *cpu) {
  const uint32_t ah = x86p_reg_read(cpu, kX86pEax, 2) >> 8;
  const uint32_t keep = x86p_eflags(&cpu->flags) & ~UINT32_C(0xFF);
  x86p_flags_set_explicit(&cpu->flags, keep | (ah & 0xD5u) | 0x02u);
}

void x86p_cpu_lahf(X86pCpu *cpu) {
  const uint32_t f = x86p_eflags(&cpu->flags);
  const uint32_t ax = x86p_reg_read(cpu, kX86pEax, 2);
  x86p_reg_write(cpu, kX86pEax, 2, (ax & 0xFFu) | (((f & 0xD5u) | 0x02u) << 8));
}

void x86p_cpu_cpuid(X86pCpu *cpu) {
  X86pCpuidResult r;
  x86p_cpuid(cpu->reg[kX86pEax], cpu->reg[kX86pEcx], &r);
  cpu->reg[kX86pEax] = r.eax;
  cpu->reg[kX86pEbx] = r.ebx;
  cpu->reg[kX86pEcx] = r.ecx;
  cpu->reg[kX86pEdx] = r.edx;
}

void x86p_cpu_rdtsc(X86pCpu *cpu) {
  const uint64_t value = x86p_rdtsc_next(&cpu->tsc);
  cpu->reg[kX86pEax] = (uint32_t)value;
  cpu->reg[kX86pEdx] = (uint32_t)(value >> 32);
}

void x86p_cpu_double_shift32(X86pCpu *cpu, void *destination, uint32_t source, uint32_t count, uint32_t left) {
  uint32_t value, result, flags = x86p_eflags(&cpu->flags);
  int defined;
  memcpy(&value, destination, sizeof value);
  if (x86p_double_shift((int)left, value, source, count, 4, &result, &flags, &defined)) {
    memcpy(destination, &result, sizeof result);
    x86p_flags_set_explicit(&cpu->flags, flags);
  }
  /* Every masked count is defined for the admitted 32-bit operand width. */
}

/* LOOP preserves flags and the upper half of ECX for address-size 16. */
int x86p_cpu_loop(X86pCpu *cpu, uint32_t width, int zf_condition) {
  const uint32_t mask = width == 16 ? 0xFFFFu : UINT32_MAX;
  const uint32_t old = cpu->reg[kX86pEcx];
  const uint32_t count = (old - 1u) & mask;
  cpu->reg[kX86pEcx] = (old & ~mask) | count;
  return count != 0 &&
         (zf_condition == 0 || (zf_condition > 0 ? x86p_flag_zf(&cpu->flags) : !x86p_flag_zf(&cpu->flags)));
}
