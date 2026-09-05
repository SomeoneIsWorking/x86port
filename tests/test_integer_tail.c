/*
 * test_integer_tail.c -- the decimal adjusts, the bit tests, the double
 * shifts, SAHF/LAHF and the rest, against the host CPU.
 *
 * These are the instructions the SDM describes with the most "undefined"s.
 * Every architectural result and defined flag is a pass/fail contract.
 * Undefined flag values are measured with a denominator, but differences are
 * observations about the current CPU rather than portable failures.
 *
 * The oracle assembles each instruction into an executable page and runs it on
 * real registers with a controlled incoming EFLAGS. If it never runs -- a
 * non-x86 host, a failed mmap -- it REFUSES rather than passing on the cases
 * it could still do hermetically, because a green run that verified nothing is
 * worse than a red one.
 */
#include "cpu.h"
#include "decode.h"
#include "exec.h"
#include "flags.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned long g_checks;
static unsigned long g_failed;
static unsigned long g_oracle_runs;
static unsigned long g_reported;
static unsigned long g_undefined_flag_cases;
static unsigned long g_undefined_flag_differences;

#define BASE 0x00040000u
#define MEMSZ 512u
#define MEM_OPERAND_OFF 0x100u

static uint8_t g_mem[MEMSZ];
/* The bytes the host's scratch held before it ran, and the model's copy of
   them. Both are needed: the host mutates its scratch in place. */
#define SHARED_WINDOW 128u
static uint8_t g_seed[SHARED_WINDOW];
static uint8_t g_shadow[SHARED_WINDOW];

/* The architectural bits this model claims. DF is excluded: it lives in
   X86pCpu, not in the flags word, and none of these instructions touch it. */
#define COMPARED_FLAGS (X86P_CF | X86P_PF | X86P_AF | X86P_ZF | X86P_SF | X86P_OF)

#if defined(__x86_64__)

/* The guest register file, in the order the stub loads and stores it. */
typedef struct Regs {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t eflags;
} Regs;

/*
 * DAA, DAS, AAA, AAS, AAM, AAD and SALC DO NOT EXIST IN 64-BIT MODE. Their
 * opcodes were reclaimed for the REX prefixes, so executing one here raises
 * #UD -- which is how this test first ran, and is worth knowing: the guest is
 * a 32-bit program and these are exactly the instructions whose flag behaviour
 * cannot be looked up, so "the host cannot run them" would have meant the
 * least verifiable family in the file was the only unverified one.
 *
 * So the oracle enters 32-BIT COMPATIBILITY MODE for them, by far-calling
 * through Linux's 32-bit user code selector. That needs everything the CPU
 * touches while in compat mode -- the code, the register slot, and the stack --
 * to live below 4 GB, which is what MAP_32BIT is for.
 */
#define USER32_CS 0x23u
#define USER32_DS 0x2Bu

typedef enum HostMode { kModeLong = 0, kModeCompat32 } HostMode;

typedef struct LowPage {
  /* One complete page keeps generated code separate from writable oracle
     state, so each run can enforce W^X with mprotect. */
  uint8_t code[4096];
  uint8_t scratch[256];
  Regs slot;
  uint64_t saved_rsp;
  uint32_t farptr[2]; /* offset, then selector: the m16:32 an indirect far call reads */
  /*
   * DS, ES and SS as this 64-bit process has them -- which is NULL. A 64-bit
   * Linux process runs with null data selectors because 64-bit mode does not
   * consult them; compat mode does, and every memory reference through a null
   * selector is a #GP. That cost one segfault to learn, at the first
   * instruction inside compat mode, with the address correct and mapped.
   */
  uint16_t seg_save[3];
  uint8_t stack[1024];
} LowPage;

static LowPage *g_low;
static int g_low_failed;

static int page_ready(void) {
  if (g_low_failed) {
    return 0;
  }
  if (!g_low) {
    void *p = mmap(NULL, sizeof(LowPage), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p >= 0x80000000u) {
      /* Without a low mapping the compat path cannot run at all, and running
         only the long-mode half while reporting a pass would be the silent
         partial verification this repo refuses. */
      g_low_failed = 1;
      return 0;
    }
    g_low = (LowPage *)p;
  }
  return 1;
}

#define g_scratch (g_low->scratch)

/* A tiny assembler over the low page's code buffer, so the two stubs below
   read as the instruction sequences they are. */
typedef struct Asm {
  uint8_t *p;
  unsigned n;
} Asm;

static void emit(Asm *a, const void *bytes, unsigned n) {
  memcpy(a->p + a->n, bytes, n);
  a->n += n;
}
#define E(a, ...)                                                                                                      \
  do {                                                                                                                 \
    static const uint8_t b__[] = {__VA_ARGS__};                                                                        \
    emit((a), b__, (unsigned)sizeof b__);                                                                              \
  } while (0)

static void emit_u32(Asm *a, uint32_t v) {
  emit(a, &v, 4u);
}

static int make_code_writable(void) {
  return mprotect(g_low->code, sizeof g_low->code, PROT_READ | PROT_WRITE) == 0;
}

static int execute_code(void) {
  union {
    void *object;
    void (*function)(void);
  } target = {.object = g_low->code};
  __builtin___clear_cache((char *)g_low->code, (char *)g_low->code + sizeof g_low->code);
  if (mprotect(g_low->code, sizeof g_low->code, PROT_READ | PROT_EXEC) != 0) {
    return 0;
  }
  target.function();
  return 1;
}

/* MOV r/m16, Sreg (8C /r) and MOV Sreg, r/m16 (8E /r), against an absolute
   address. `sreg` is the segment register's encoding: ES 0, SS 2, DS 3. */
static void store_seg(Asm *a, unsigned sreg, uint32_t addr) {
  const uint8_t op[3] = {0x8Cu, (uint8_t)(0x04u | (sreg << 3)), 0x25u};
  emit(a, op, 3u);
  emit_u32(a, addr);
}
static void load_seg(Asm *a, unsigned sreg, uint32_t addr) {
  const uint8_t op[3] = {0x8Eu, (uint8_t)(0x04u | (sreg << 3)), 0x25u};
  emit(a, op, 3u);
  emit_u32(a, addr);
}
enum { S_ES = 0, S_SS = 2, S_DS = 3 };

/* mov <reg32>, [abs32] and mov [abs32], <reg32>, in the encoding that works in
   BOTH modes: a modrm with mod=00, rm=100 (SIB), base=101 (none) -- so the
   disp32 is an absolute address, not RIP-relative. */
static void mov_from_abs(Asm *a, unsigned reg, uint32_t addr) {
  const uint8_t op[3] = {0x8Bu, (uint8_t)(0x04u | (reg << 3)), 0x25u};
  emit(a, op, 3u);
  emit_u32(a, addr);
}
static void mov_to_abs(Asm *a, unsigned reg, uint32_t addr) {
  const uint8_t op[3] = {0x89u, (uint8_t)(0x04u | (reg << 3)), 0x25u};
  emit(a, op, 3u);
  emit_u32(a, addr);
}
/* push dword [abs32] (FF /6) and pop dword [abs32] (8F /0). */
static void push_abs(Asm *a, uint32_t addr) {
  E(a, 0xFFu, 0x34u, 0x25u);
  emit_u32(a, addr);
}
static void pop_abs(Asm *a, uint32_t addr) {
  E(a, 0x8Fu, 0x04u, 0x25u);
  emit_u32(a, addr);
}

enum { R_AX = 0, R_CX = 1, R_DX = 2, R_BX = 3, R_SI = 6, R_DI = 7 };
static const unsigned kGpr[] = {R_AX, R_CX, R_DX, R_BX, R_SI, R_DI};
static const unsigned kGprSlot[] = {0u, 4u, 8u, 12u, 24u, 28u};

/* Load the guest GPRs and EFLAGS, run the instruction, store them back. Used
   verbatim by both modes -- the encodings are identical in long mode and in
   compat mode, which is the whole reason this can be one function. */
static void emit_body(Asm *a, uint32_t slot, const uint8_t *code, unsigned len) {
  unsigned i;
  for (i = 0; i < sizeof kGpr / sizeof kGpr[0]; i++) {
    mov_from_abs(a, kGpr[i], slot + kGprSlot[i]);
  }
  push_abs(a, slot + 32u);
  E(a, 0x9Du); /* POPFD / POPFQ */
  emit(a, code, len);
  E(a, 0x9Cu); /* PUSHFD / PUSHFQ, before anything else writes flags */
  pop_abs(a, slot + 32u);
  for (i = 0; i < sizeof kGpr / sizeof kGpr[0]; i++) {
    mov_to_abs(a, kGpr[i], slot + kGprSlot[i]);
  }
}

/*
 * Build the whole stub, in whichever mode the instruction needs, and run it.
 *
 * ESP and EBP are deliberately not guest-controlled: the stub runs on a real
 * stack, and a guest ESP would send PUSHFD and the return itself into
 * unmapped memory. The instructions here are register-and-flags instructions;
 * the stack-touching ones (PUSHAD, POPAD, ENTER) are exercised through the
 * model against a mapped guest stack in the interpreter tests instead.
 */
static int run_host(const uint8_t *code, unsigned len, Regs *r, HostMode mode) {
  Asm a;
  uint32_t slot;

  if (!page_ready() || !make_code_writable()) {
    return 0;
  }
  slot = (uint32_t)(uintptr_t)&g_low->slot;
  g_low->slot = *r;
  a.p = g_low->code;
  a.n = 0u;

  E(&a, 0x53u, 0x55u); /* push rbx, rbp -- callee-saved and about to be clobbered */

  if (mode == kModeLong) {
    emit_body(&a, slot, code, len);
  } else {
    unsigned far_fixup;
    const uint32_t segs = (uint32_t)(uintptr_t)g_low->seg_save;
    /* Give the data segments real 32-bit descriptors for the duration. */
    store_seg(&a, S_DS, segs + 0u);
    store_seg(&a, S_ES, segs + 2u);
    store_seg(&a, S_SS, segs + 4u);
    E(&a, 0xB8u); /* mov eax, USER32_DS */
    emit_u32(&a, USER32_DS);
    E(&a, 0x8Eu, 0xD8u); /* mov ds, ax */
    E(&a, 0x8Eu, 0xC0u); /* mov es, ax */
    E(&a, 0x8Eu, 0xD0u); /* mov ss, ax */
    /* Park RSP somewhere this function can find it with a 32-bit address, then
       move the stack itself below 4 GB: in compat mode the stack pointer IS
       ESP, and the host's real stack is far above that. */
    E(&a, 0x48u, 0xB8u); /* movabs rax, imm64 */
    {
      uint64_t addr = (uint64_t)(uintptr_t)&g_low->saved_rsp;
      emit(&a, &addr, 8u);
    }
    E(&a, 0x48u, 0x89u, 0x20u); /* mov [rax], rsp */
    E(&a, 0xBCu);               /* mov esp, imm32 -- zero-extends, so RSP lands low */
    emit_u32(&a, (uint32_t)(uintptr_t)(g_low->stack + sizeof g_low->stack));

    /* lcall far [farptr]: FF /3 with an absolute m16:32. The target offset is
       filled in once the 32-bit body's address is known. */
    E(&a, 0xFFu, 0x1Cu, 0x25u);
    emit_u32(&a, (uint32_t)(uintptr_t)g_low->farptr);

    /* Back in long mode: restore the real stack. */
    E(&a, 0x48u, 0xB8u);
    {
      uint64_t addr = (uint64_t)(uintptr_t)&g_low->saved_rsp;
      emit(&a, &addr, 8u);
    }
    E(&a, 0x48u, 0x8Bu, 0x20u); /* mov rsp, [rax] */
    /* SS last, and only after RSP is a 64-bit address again: a null SS is
       legal in 64-bit mode, which is why it can be restored at all. */
    load_seg(&a, S_DS, segs + 0u);
    load_seg(&a, S_ES, segs + 2u);
    load_seg(&a, S_SS, segs + 4u);
    E(&a, 0x5Du, 0x5Bu, 0xC3u); /* pop rbp, rbx; ret */

    /* The compat-mode body, at whatever offset we have reached. */
    far_fixup = a.n;
    g_low->farptr[0] = (uint32_t)(uintptr_t)(g_low->code + far_fixup);
    g_low->farptr[1] = USER32_CS;
    emit_body(&a, slot, code, len);
    E(&a, 0xCBu); /* lret, back to the 64-bit caller above */

    if (!execute_code()) {
      return 0;
    }
    *r = g_low->slot;
    g_oracle_runs++;
    return 1;
  }

  E(&a, 0x5Du, 0x5Bu, 0xC3u); /* pop rbp, rbx; ret */
  if (!execute_code()) {
    return 0;
  }
  *r = g_low->slot;
  g_oracle_runs++;
  return 1;
}

/* The starting state is printed with every failure: without it a divergence
   report says a flag differs and not which input made it differ, and these are
   the instructions whose rule has to be DERIVED from the silicon rather than
   read out of the manual. */
static void report(const char *what, const Regs *start, const char *field, uint32_t host, uint32_t model) {
  g_failed++;
  if (g_reported++ < 24u) {
    printf("    FAIL %-10s in: eax=%08x edx=%08x ecx=%08x fl=%04x | %s host=%08x model=%08x differ=%08x\n",
           what,
           start->eax,
           start->edx,
           start->ecx,
           start->eflags,
           field,
           host,
           model,
           host ^ model);
  }
}

/*
 * Run one instruction both ways from the same starting state and compare.
 *
 * `uses_memory` says the encoding reads or writes [scratch]; the guest sees it
 * at BASE+MEM_OPERAND_OFF and the host at g_scratch, so both are seeded with
 * the same bytes and both are compared afterwards.
 */
/*
 * `expect_refusal` says this input is outside the instruction's defined domain
 * -- a 16-bit double shift by sixteen or more -- and the model is required to
 * REFUSE it rather than produce a value. That is a check, not a skip: a model
 * that silently invented a plausible result here would pass a test that merely
 * ignored these inputs, and the whole point of naming the domain in
 * x86p_double_shift was to make the refusal observable.
 */
static void compare(const char *what,
                    const uint8_t *code,
                    unsigned len,
                    const Regs *start,
                    HostMode mode,
                    uint32_t defined_flags,
                    int expect_refusal,
                    unsigned memlen) {
  Regs host = *start;
  X86pCpu cpu;
  X86pMem mem;
  X86pStepReport rep;
  uint32_t mf, hf;
  unsigned i;
  static const int kSlot[] = {kX86pEax, kX86pEcx, kX86pEdx, kX86pEbx, -1, -1, kX86pEsi, kX86pEdi};
  static const char *kSlotName[] = {"EAX", "ECX", "EDX", "EBX", "", "", "ESI", "EDI"};

  if (memlen) {
    /*
     * The guest's memory window IS the host's scratch buffer, at the same
     * numeric address, so EBX holds one value and it means the same thing to
     * the model and to the CPU. Without that the two runs would address two
     * different buffers and the comparison would only be as good as the
     * bookkeeping keeping them in step -- which is precisely what a bit-string
     * offset of -33 is likely to break.
     */
    memcpy(g_seed, g_low->scratch, memlen);
  }
  if (!run_host(code, len, &host, mode)) {
    return;
  }

  memset(g_mem, 0, sizeof g_mem);
  memcpy(g_mem, code, len);
  mem.host = g_mem;
  mem.lo = BASE;
  mem.size = MEMSZ;
  if (memlen) {
    memcpy(g_shadow, g_seed, memlen);
    mem.host = g_shadow;
    mem.lo = (uint32_t)(uintptr_t)g_low->scratch;
    mem.size = memlen;
  }

  x86p_cpu_reset(&cpu);
  cpu.eip = BASE;
  cpu.reg[kX86pEax] = start->eax;
  cpu.reg[kX86pEcx] = start->ecx;
  cpu.reg[kX86pEdx] = start->edx;
  cpu.reg[kX86pEbx] = start->ebx;
  cpu.reg[kX86pEsi] = start->esi;
  cpu.reg[kX86pEdi] = start->edi;
  x86p_flags_set_explicit(&cpu.flags, start->eflags);

  g_checks++;
  if (memlen) {
    /* The window holds the operand, not the instruction, so the instruction is
       decoded from the caller's bytes and executed directly. */
    X86pInsn insn;
    memset(&insn, 0, sizeof insn);
    if (x86p_decode(code, len, &insn) == 0u) {
      g_failed++;
      printf("    FAIL %-10s the decoder refused the bytes\n", what);
      return;
    }
    rep.status = x86p_execute_decoded(&cpu, &mem, &insn, NULL);
  } else {
    rep.status = x86p_step(&cpu, &mem, &rep);
  }
  if (rep.status != kX86pStepOk) {
    if (expect_refusal) {
      return; /* the required outcome */
    }
    g_failed++;
    if (g_reported++ < 20u) {
      printf("    FAIL %-10s in: eax=%08x fl=%04x | the model refused: %s\n",
             what,
             start->eax,
             start->eflags,
             x86p_step_status_name(rep.status));
    }
    return;
  }

  if (expect_refusal) {
    g_failed++;
    if (g_reported++ < 24u) {
      printf("    FAIL %-10s in: eax=%08x ecx=%08x | undefined input ACCEPTED; the model invented a result\n",
             what,
             start->eax,
             start->ecx);
    }
    return;
  }

  for (i = 0; i < 8u; i++) {
    uint32_t hv, mv;
    if (kSlot[i] < 0) {
      continue;
    }
    hv = ((const uint32_t *)&host)[i];
    mv = cpu.reg[kSlot[i]];
    g_checks++;
    if (hv != mv) {
      report(what, start, kSlotName[i], hv, mv);
    }
  }

  if (memlen) {
    g_checks++;
    if (memcmp(g_low->scratch, g_shadow, memlen) != 0) {
      unsigned k;
      g_failed++;
      if (g_reported++ < 24u) {
        printf("    FAIL %-10s in: eax=%08x ebx=%08x | the operand bytes differ at", what, start->eax, start->ebx);
        for (k = 0; k < memlen; k++) {
          if (g_low->scratch[k] != g_shadow[k]) {
            printf(" [%u] host=%02x model=%02x", k, g_low->scratch[k], g_shadow[k]);
          }
        }
        printf("\n");
      }
    }
  }

  hf = host.eflags & COMPARED_FLAGS;
  mf = x86p_eflags(&cpu.flags) & COMPARED_FLAGS;
  if ((COMPARED_FLAGS & ~defined_flags) != 0u) {
    g_undefined_flag_cases++;
    if ((hf & ~defined_flags) != (mf & ~defined_flags)) {
      g_undefined_flag_differences++;
    }
  }
  g_checks++;
  if ((hf & defined_flags) != (mf & defined_flags)) {
    report(what, start, "defined EFLAGS", hf & defined_flags, mf & defined_flags);
  }
}

/* An instruction whose only operand is a fixed encoding. */
static void sweep_simple(const char *what, const uint8_t *code, unsigned len, HostMode mode, uint32_t defined_flags) {
  static const uint32_t kEax[] = {0x00000000u,
                                  0x00000009u,
                                  0x0000000Au,
                                  0x0000000Fu,
                                  0x00000099u,
                                  0x0000009Au,
                                  0x000000FFu,
                                  0x00001234u,
                                  0x000019FFu,
                                  0x0000FF99u,
                                  0x12345678u,
                                  0xFFFFFFFFu};
  /* Every combination of the two flags these instructions READ -- CF and AF --
     with OF both set and clear, because the question the first sweep could not
     answer was what an instruction that performs NO adjustment does to OF, and
     every OF-set case it tried also set AF. */
  static const uint32_t kFlags[] = {0x00000002u,
                                    0x00000003u,
                                    0x00000012u,
                                    0x00000013u,
                                    0x00000882u,
                                    0x00000883u,
                                    0x00000892u,
                                    0x00000893u,
                                    0x000008D5u,
                                    0x00000855u};
  unsigned a, f;
  for (a = 0; a < sizeof kEax / sizeof kEax[0]; a++) {
    for (f = 0; f < sizeof kFlags / sizeof kFlags[0]; f++) {
      Regs r;
      memset(&r, 0, sizeof r);
      r.eax = kEax[a];
      r.ecx = 0x0000001Fu;
      r.ebx = BASE + MEM_OPERAND_OFF;
      r.edx = 0xDEADBEEFu;
      r.eflags = kFlags[f];
      compare(what, code, len, &r, mode, defined_flags, 0, 0u);
    }
  }
}

/*
 * 16-BIT ADDRESSING, through LEA -- the 0x67 prefix's [BX+SI] family.
 *
 * LEA rather than a load, because the whole question is the ADDRESS: a 16-bit
 * effective address wraps within its sixteen bits before any segment base is
 * added, so [BX+SI] with BX=0xF000 and SI=0x2000 names 0x1000 and not
 * 0x11000. Reading through it would need memory below 64K, which
 * vm.mmap_min_addr forbids -- and would test the same arithmetic behind an
 * access that cannot be set up. LEA computes it and writes it down.
 *
 * The register values carry GARBAGE IN THEIR HIGH HALVES on purpose. A model
 * that summed the full 32-bit registers and then truncated agrees with one
 * that truncates the parts on every input where the high halves are zero,
 * which is every input a careless sweep would pick.
 *
 * Compat mode, necessarily: in 64-bit mode 0x67 selects 32-bit addressing and
 * these bytes mean something else entirely.
 */
static void sweep_addr16(const char *what, const uint8_t *code, unsigned len) {
  static const uint32_t kVals[] = {0x00000000u,
                                   0x00000001u,
                                   0x0000FFFFu,
                                   0x00008000u,
                                   0x00007FFFu,
                                   0xDEAD0001u,
                                   0x1234F000u,
                                   0xFFFF2000u,
                                   0xAAAAFFFFu,
                                   0x0000ABCDu};
  unsigned b, i;
  for (b = 0; b < sizeof kVals / sizeof kVals[0]; b++) {
    for (i = 0; i < sizeof kVals / sizeof kVals[0]; i++) {
      Regs r;
      memset(&r, 0, sizeof r);
      r.ebx = kVals[b];
      r.esi = kVals[i];
      r.edi = kVals[(i + 3u) % (sizeof kVals / sizeof kVals[0])];
      r.eax = 0xCCCCCCCCu;
      r.eflags = 0x00000002u;
      compare(what, code, len, &r, kModeCompat32, COMPARED_FLAGS, 0, 0u);
    }
  }
}

#define INSN(id, ...) static const uint8_t id[] = {__VA_ARGS__};

/* Two-operand sweeps for the shifts and bit tests, over values chosen to cross
   every boundary these instructions have: a sign bit, a zero result, a count
   at each end of its range. */
typedef enum FlagRule { kFlagsDoubleShift, kFlagsBitTest } FlagRule;

static uint32_t two_defined_flags(FlagRule rule, uint32_t count, unsigned bits) {
  if (rule == kFlagsBitTest) {
    return X86P_CF;
  }
  count &= 0x1Fu;
  if (count == 0u) {
    return COMPARED_FLAGS;
  }
  {
    uint32_t flags = X86P_PF | X86P_ZF | X86P_SF;
    if (count < bits) {
      flags |= X86P_CF;
    }
    if (count == 1u) {
      flags |= X86P_OF;
    }
    return flags;
  }
}

static void
sweep_two(const char *what, const uint8_t *code, unsigned len, unsigned undefined_from, FlagRule flag_rule) {
  static const uint32_t kVals[] = {0x00000000u, 0x00000001u, 0x80000000u, 0xFFFFFFFFu, 0x12345678u, 0xA5A5A5A5u};
  static const uint32_t kCounts[] = {0u, 1u, 7u, 15u, 16u, 31u, 32u, 33u};
  unsigned d, s, k;
  for (d = 0; d < sizeof kVals / sizeof kVals[0]; d++) {
    for (s = 0; s < sizeof kVals / sizeof kVals[0]; s++) {
      for (k = 0; k < sizeof kCounts / sizeof kCounts[0]; k++) {
        Regs r;
        memset(&r, 0, sizeof r);
        r.eax = kVals[d];
        r.edx = kVals[s];
        r.ecx = kCounts[k];
        r.ebx = BASE + MEM_OPERAND_OFF;
        r.eflags = 0x00000002u;
        const unsigned bits = undefined_from != 0u ? undefined_from : 32u;
        compare(what,
                code,
                len,
                &r,
                kModeLong,
                two_defined_flags(flag_rule, kCounts[k], bits),
                undefined_from != 0u && (kCounts[k] & 31u) >= undefined_from,
                0u);
      }
    }
  }
}

/*
 * The bit-string forms: BT/BTS/BTR/BTC with a memory destination and a
 * REGISTER offset, which is the one addressing mode in this file that is not a
 * modrm. The offset is signed and unbounded, so the instruction reaches a bit
 * whole operands away from the address the modrm computed -- forwards and
 * backwards. A model that masked the offset to the operand width instead is
 * right for every offset under 32 and wrong past it, which is exactly the
 * range a guest walking a bitmap uses.
 */
static void sweep_bit_string(const char *what, const uint8_t *code, unsigned len) {
  static const int32_t kOffs[] = {0,  1,  7,   8,   31,  32,  33,   63,   64,  127,
                                  -1, -8, -32, -33, -64, -65, -127, -128, 255, -256};
  unsigned i;
  if (!page_ready()) {
    return;
  }
  for (i = 0; i < sizeof kOffs / sizeof kOffs[0]; i++) {
    Regs r;
    unsigned k;
    for (k = 0; k < SHARED_WINDOW; k++) {
      g_low->scratch[k] = (uint8_t)(0x11u * (k + 1u) ^ (k << 3));
    }
    memset(&r, 0, sizeof r);
    /* The base sits in the MIDDLE of the window so a negative offset still
       lands inside it; every offset above is within +/- 32 bytes of it. */
    r.ebx = (uint32_t)(uintptr_t)(g_low->scratch + SHARED_WINDOW / 2u);
    r.eax = (uint32_t)kOffs[i];
    r.eflags = 0x00000002u;
    compare(what, code, len, &r, kModeLong, X86P_CF, 0, SHARED_WINDOW);
  }
}

int main(void) {
  INSN(k_daa, 0x27u)
  INSN(k_das, 0x2Fu)
  INSN(k_aaa, 0x37u)
  INSN(k_aas, 0x3Fu)
  INSN(k_aam, 0xD4u, 0x0Au)
  INSN(k_aam7, 0xD4u, 0x07u)
  INSN(k_aad, 0xD5u, 0x0Au)
  INSN(k_aad7, 0xD5u, 0x07u)
  INSN(k_sahf, 0x9Eu)
  INSN(k_lahf, 0x9Fu)
  INSN(k_stc, 0xF9u)
  INSN(k_clc, 0xF8u)
  INSN(k_cmc, 0xF5u)
  INSN(k_salc, 0xD6u)
  INSN(k_shld_cl, 0x0Fu, 0xA5u, 0xD0u) /* SHLD EAX, EDX, CL */
  INSN(k_shrd_cl, 0x0Fu, 0xADu, 0xD0u) /* SHRD EAX, EDX, CL */
  INSN(k_shld_i, 0x0Fu, 0xA4u, 0xD0u, 0x0Bu)
  INSN(k_shrd_i, 0x0Fu, 0xACu, 0xD0u, 0x0Bu)
  INSN(k_shld16, 0x66u, 0x0Fu, 0xA5u, 0xD0u)
  INSN(k_shrd16, 0x66u, 0x0Fu, 0xADu, 0xD0u)
  INSN(k_bt, 0x0Fu, 0xA3u, 0xD0u)  /* BT  EAX, EDX */
  INSN(k_bts, 0x0Fu, 0xABu, 0xD0u) /* BTS EAX, EDX */
  INSN(k_btr, 0x0Fu, 0xB3u, 0xD0u)
  INSN(k_btc, 0x0Fu, 0xBBu, 0xD0u)
  INSN(k_bt_i, 0x0Fu, 0xBAu, 0xE0u, 0x11u) /* BT  EAX, 0x11 */
  INSN(k_bts_i, 0x0Fu, 0xBAu, 0xE8u, 0x11u)
  INSN(k_btr_i, 0x0Fu, 0xBAu, 0xF0u, 0x11u)
  INSN(k_btc_i, 0x0Fu, 0xBAu, 0xF8u, 0x11u)
  INSN(k_bt_m, 0x0Fu, 0xA3u, 0x03u) /* BT  [EBX], EAX */
  INSN(k_bts_m, 0x0Fu, 0xABu, 0x03u)
  INSN(k_btr_m, 0x0Fu, 0xB3u, 0x03u)
  INSN(k_btc_m, 0x0Fu, 0xBBu, 0x03u)
  /* LEA EAX, <16-bit address>. Every form that does not name BP, which the
     harness uses as its own frame register. */
  INSN(k_lea16_bxsi, 0x67u, 0x8Du, 0x00u)
  INSN(k_lea16_bxdi, 0x67u, 0x8Du, 0x01u)
  INSN(k_lea16_si, 0x67u, 0x8Du, 0x04u)
  INSN(k_lea16_di, 0x67u, 0x8Du, 0x05u)
  INSN(k_lea16_abs, 0x67u, 0x8Du, 0x06u, 0x34u, 0x12u)
  INSN(k_lea16_bx, 0x67u, 0x8Du, 0x07u)
  INSN(k_lea16_d8, 0x67u, 0x8Du, 0x40u, 0x7Fu)
  INSN(k_lea16_d8n, 0x67u, 0x8Du, 0x41u, 0x80u)
  INSN(k_lea16_d16, 0x67u, 0x8Du, 0x80u, 0x00u, 0x80u)
  INSN(k_lea16_bxd16, 0x67u, 0x8Du, 0x87u, 0xFFu, 0xFFu)

  printf("test the integer tail against the host CPU\n");

#if defined(__APPLE__)
  printf("SKIP: this oracle requires Linux x86-64 compatibility mode for 32-bit-only decimal-adjust instructions\n");
  return 77;
#endif

#define SIMPLE(x, flags) sweep_simple(#x, k_##x, (unsigned)sizeof k_##x, kModeLong, (flags))
/* The seven long mode dropped. Same sweep, entered through a 32-bit code
   segment; if that path cannot be set up the run refuses rather than quietly
   testing only the other half. */
#define COMPAT(x, flags) sweep_simple(#x, k_##x, (unsigned)sizeof k_##x, kModeCompat32, (flags))
  COMPAT(daa, COMPARED_FLAGS & ~X86P_OF);
  COMPAT(das, COMPARED_FLAGS & ~X86P_OF);
  COMPAT(aaa, X86P_CF | X86P_AF);
  COMPAT(aas, X86P_CF | X86P_AF);
  COMPAT(aam, X86P_PF | X86P_ZF | X86P_SF);
  COMPAT(aam7, X86P_PF | X86P_ZF | X86P_SF);
  COMPAT(aad, X86P_PF | X86P_ZF | X86P_SF);
  COMPAT(aad7, X86P_PF | X86P_ZF | X86P_SF);
  COMPAT(salc, COMPARED_FLAGS);
#undef COMPAT
  SIMPLE(sahf, COMPARED_FLAGS);
  SIMPLE(lahf, COMPARED_FLAGS);
  SIMPLE(stc, COMPARED_FLAGS);
  SIMPLE(clc, COMPARED_FLAGS);
  SIMPLE(cmc, COMPARED_FLAGS);
#undef SIMPLE
/* The 16-bit double shifts have a count of sixteen or more outside their
   defined domain; every other form here is defined for every masked count. */
#define SHIFT(x) sweep_two(#x, k_##x, (unsigned)sizeof k_##x, 0u, kFlagsDoubleShift)
#define SHIFT16(x) sweep_two(#x, k_##x, (unsigned)sizeof k_##x, 16u, kFlagsDoubleShift)
#define BIT(x) sweep_two(#x, k_##x, (unsigned)sizeof k_##x, 0u, kFlagsBitTest)
  SHIFT(shld_cl);
  SHIFT(shrd_cl);
  SHIFT(shld_i);
  SHIFT(shrd_i);
  SHIFT16(shld16);
  SHIFT16(shrd16);
  BIT(bt);
  BIT(bts);
  BIT(btr);
  BIT(btc);
  BIT(bt_i);
  BIT(bts_i);
  BIT(btr_i);
  BIT(btc_i);
#undef BIT
#undef SHIFT16
#undef SHIFT
#define STRING(x) sweep_bit_string(#x, k_##x, (unsigned)sizeof k_##x)
  STRING(bt_m);
  STRING(bts_m);
  STRING(btr_m);
  STRING(btc_m);
#undef STRING
#define ADDR16(x) sweep_addr16(#x, k_##x, (unsigned)sizeof k_##x)
  ADDR16(lea16_bxsi);
  ADDR16(lea16_bxdi);
  ADDR16(lea16_si);
  ADDR16(lea16_di);
  ADDR16(lea16_abs);
  ADDR16(lea16_bx);
  ADDR16(lea16_d8);
  ADDR16(lea16_d8n);
  ADDR16(lea16_d16);
  ADDR16(lea16_bxd16);
#undef ADDR16

  if (g_oracle_runs == 0u) {
    printf("REFUSED: the host oracle never ran, so nothing here was verified\n");
    return 1;
  }
  printf("%lu instruction(s) executed on the host CPU, %lu check(s), %lu failure(s)%s\n",
         g_oracle_runs,
         g_checks,
         g_failed,
         g_failed > 20u ? " (first 20 shown)" : "");
  printf("undefined flags differed in %lu/%lu comparison(s); these observations are not architectural failures\n",
         g_undefined_flag_differences,
         g_undefined_flag_cases);
  return g_failed ? 1 : 0;
}

#else

int main(void) {
  printf("REFUSED: this host is not x86-64, so nothing in the integer tail was verified.\n");
  return 1;
}

#endif
