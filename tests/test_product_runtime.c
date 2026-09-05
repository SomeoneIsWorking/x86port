#include "cpu.h"
#include "jit_engine.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  kGuestBase = 0x00010000u,
  kGuestSize = 4096u,
};

static void put_u32(uint8_t *at, uint32_t value) {
  at[0] = (uint8_t)value;
  at[1] = (uint8_t)(value >> 8);
  at[2] = (uint8_t)(value >> 16);
  at[3] = (uint8_t)(value >> 24);
}

static int expect(int condition, const char *message) {
  if (condition) {
    return 1;
  }
  printf("FAIL: %s\n", message);
  return 0;
}

static X86pJitEngine *create_engine(const X86pMem *mem) {
  char reason[256] = {0};
  X86pJitEngine *engine = x86p_jit_engine_create(mem, 1u << 16, 128u, reason, (unsigned)sizeof reason);
  if (engine == NULL) {
    printf("FAIL: create engine: %s\n", reason);
  }
  return engine;
}

static int translated_program_runs(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xB8,
      0x2A,
      0x00,
      0x00,
      0x00, /* MOV EAX, 42 */
      0xEB,
      0xFE, /* JMP $ */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  status = x86p_jit_engine_run(engine, cpu, 8u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunBudget, "translated program did not consume its block budget") &&
         expect(cpu->reg[kX86pEax] == 42u, "translated MOV did not update EAX") &&
         expect(cpu->eip == kGuestBase + 5u, "translated loop stopped at the wrong EIP") &&
         expect(stats.blocks_entered > 0u, "no translated block entered") &&
         expect(stats.translate_refusals == 0u, "supported program was refused");
}

static int unsupported_program_is_refused(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  /* RCPPS remains a named unsupported instruction in both backends. */
  guest[0] = 0x0Fu;
  guest[1] = 0x53u;
  guest[2] = 0xC0u;
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + 3u);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  cpu->reg[kX86pEsp] = kGuestBase + 0x800u;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "unsupported instruction did not return the product refusal") &&
         expect(cpu->eip == kGuestBase, "unsupported instruction changed EIP") &&
         expect(cpu->reg[kX86pEsp] == kGuestBase + 0x800u, "unsupported instruction changed guest state") &&
         expect(strstr(reason, "RCPPS") != NULL, "entry refusal omitted the decoded mnemonic") &&
         expect(strstr(reason, "0f 53 c0") != NULL, "entry refusal omitted the instruction bytes") &&
         expect(stats.translate_refusals == 1u, "unsupported translation was not counted");
}

static int setz_executes_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xB8,
      0x2A,
      0x00,
      0x00,
      0x00, /* MOV EAX, 42 */
      0x0F,
      0x94,
      0xC2, /* SETZ DL */
      0x0F,
      0x53,
      0xC0, /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  cpu->reg[kX86pEdx] = 0x112233FFu;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-SETZ refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 8u, "product did not execute through SETZ") &&
         expect(cpu->reg[kX86pEax] == 42u, "translated prefix did not execute before refusal") &&
         expect(cpu->reg[kX86pEdx] == 0x11223300u, "SETZ DL did not preserve the upper register bytes") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-SETZ refusal omitted the decoded mnemonic") &&
         expect(strstr(reason, "0f 53 c0") != NULL, "post-SETZ refusal omitted the instruction bytes") &&
         expect(stats.translate_refusals == 2u, "post-SETZ refusal was not counted");
}

static int cdq_and_div_execute_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xB8,
      0x2A,
      0x00,
      0x00,
      0x00, /* MOV EAX, 42 */
      0x99, /* CDQ */
      0xB9,
      0x07,
      0x00,
      0x00,
      0x00, /* MOV ECX, 7 */
      0xF7,
      0xF1, /* DIV ECX */
      0x0F,
      0x53,
      0xC0, /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  cpu->reg[kX86pEdx] = 0xFFFFFFFFu;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-DIV refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 13u, "product did not execute through CDQ and DIV") &&
         expect(cpu->reg[kX86pEax] == 6u, "DIV did not write the quotient") &&
         expect(cpu->reg[kX86pEdx] == 0u, "DIV did not write the remainder") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-DIV refusal omitted the decoded mnemonic") &&
         expect(strstr(reason, "0f 53 c0") != NULL, "post-DIV refusal omitted the instruction bytes") &&
         expect(stats.translate_refusals == 3u, "post-DIV refusal was not counted");
}

static int idiv_executes_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xB8,
      0xF9,
      0xFF,
      0xFF,
      0xFF, /* MOV EAX, -7 */
      0x99, /* CDQ */
      0xB9,
      0x03,
      0x00,
      0x00,
      0x00, /* MOV ECX, 3 */
      0xF7,
      0xF9, /* IDIV ECX */
      0x0F,
      0x53,
      0xC0, /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-IDIV refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 13u, "product did not execute through IDIV") &&
         expect(cpu->reg[kX86pEax] == 0xFFFFFFFEu, "IDIV did not write the signed quotient") &&
         expect(cpu->reg[kX86pEdx] == 0xFFFFFFFFu, "IDIV did not write the signed remainder") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-IDIV refusal omitted the decoded mnemonic") &&
         expect(stats.translate_refusals == 4u, "post-IDIV refusal was not counted");
}

static int imul_memory_executes_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xB9,
      0x03,
      0x00,
      0x00,
      0x00, /* MOV ECX, 3 */
      0x0F,
      0xAF,
      0x4D,
      0x0C, /* IMUL ECX, dword [EBP+0x0C] */
      0x0F,
      0x53,
      0xC0, /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  put_u32(guest + 0x400u, 7u);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  cpu->reg[kX86pEbp] = kGuestBase + 0x400u - 0x0Cu;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-IMUL refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 9u, "product did not execute through IMUL") &&
         expect(cpu->reg[kX86pEcx] == 21u, "IMUL did not write its low 32-bit product") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-IMUL refusal omitted the decoded mnemonic") &&
         expect(stats.translate_refusals == 5u, "post-IMUL refusal was not counted");
}

static int imul_immediate_alias_executes_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xBE,
      0x03,
      0x00,
      0x00,
      0x00, /* MOV ESI, 3 */
      0x6B,
      0xF6,
      0x38, /* IMUL ESI, ESI, 56 */
      0x0F,
      0x53,
      0xC0, /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-IMUL-immediate refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 8u, "product did not execute through IMUL immediate") &&
         expect(cpu->reg[kX86pEsi] == 168u, "IMUL immediate did not preserve source aliasing") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-IMUL-immediate refusal omitted the mnemonic") &&
         expect(stats.translate_refusals == 6u, "post-IMUL-immediate refusal was not counted");
}

static int xchg_eax_esp_executes_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0x94, /* XCHG EAX, ESP */
      0x0F,
      0x53,
      0xC0 /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;

  memcpy(guest, program, sizeof program);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  cpu->reg[kX86pEax] = 0x12345678u;
  cpu->reg[kX86pEsp] = 0x89ABCDEFu;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-XCHG refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 1u, "product did not execute through XCHG") &&
         expect(cpu->reg[kX86pEax] == 0x89ABCDEFu, "XCHG did not write EAX") &&
         expect(cpu->reg[kX86pEsp] == 0x12345678u, "XCHG did not write ESP") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-XCHG refusal omitted the mnemonic") &&
         expect(stats.translate_refusals == 7u, "post-XCHG refusal was not counted");
}

static int fnclex_executes_before_a_named_refusal(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  static const uint8_t program[] = {
      0xDB,
      0xE2, /* FNCLEX */
      0x0F,
      0x53,
      0xC0, /* RCPPS XMM0, XMM0: stable named refusal. */
  };
  char reason[256] = {0};
  X86pJitEngineStats stats;
  X86pJitRunStatus status;
  long double first = 0.0L;
  long double second = 0.0L;
  uint8_t tags[X86P_X87_REGS];

  memcpy(guest, program, sizeof program);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + (uint32_t)sizeof program);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  cpu->reg[kX86pEax] = UINT32_C(0xA5A5A5A5);
  cpu->flags.kind = (uint8_t)kX86pFlagsExplicit;
  cpu->flags.a = UINT32_C(0x000008D5);
  cpu->x87.control = UINT16_C(0x0B7F);
  if (!x86p_x87_push(&cpu->x87, 1.25L) || !x86p_x87_push(&cpu->x87, -3.5L)) {
    return expect(0, "could not prepare the FNCLEX x87 stack");
  }
  cpu->x87.status = UINT16_C(0xFFFF);
  memcpy(tags, cpu->x87.tag, sizeof tags);

  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  x86p_jit_engine_stats(engine, &stats);

  return expect(status == kX86pRunUnsupported, "post-FNCLEX refusal did not return the product status") &&
         expect(cpu->eip == kGuestBase + 2u, "product did not execute through FNCLEX") &&
         expect(cpu->x87.status == (uint16_t)(UINT16_C(0xFFFF) & (uint16_t)~X86P_X87_FNCLEX_MASK),
                "FNCLEX cleared the wrong status bits") &&
         expect(cpu->x87.top == 6u, "FNCLEX changed TOP") &&
         expect(cpu->x87.control == UINT16_C(0x0B7F), "FNCLEX changed the control word") &&
         expect(memcmp(cpu->x87.tag, tags, sizeof tags) == 0, "FNCLEX changed the tag word") &&
         expect(x86p_x87_get(&cpu->x87, 0, &first) && first == -3.5L, "FNCLEX changed ST(0)") &&
         expect(x86p_x87_get(&cpu->x87, 1, &second) && second == 1.25L, "FNCLEX changed ST(1)") &&
         expect(cpu->reg[kX86pEax] == UINT32_C(0xA5A5A5A5), "FNCLEX changed a general register") &&
         expect(cpu->flags.kind == (uint8_t)kX86pFlagsExplicit && cpu->flags.a == UINT32_C(0x000008D5),
                "FNCLEX changed integer flags") &&
         expect(strstr(reason, "RCPPS") != NULL, "post-FNCLEX refusal omitted the mnemonic") &&
         expect(stats.translate_refusals == 8u, "post-FNCLEX refusal was not counted");
}

static int translation_failures_are_distinct(X86pJitEngine *engine, X86pCpu *cpu, uint8_t *guest) {
  char reason[256] = {0};
  X86pJitRunStatus status;

  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase + kGuestSize;
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  if (!expect(status == kX86pRunFetchFault, "unmapped EIP was not reported as a fetch fault")) {
    return 0;
  }

  memset(guest, 0x66, X86P_MAX_INSN_LEN);
  x86p_jit_engine_invalidate(engine, kGuestBase, kGuestBase + X86P_MAX_INSN_LEN);
  x86p_cpu_reset(cpu);
  cpu->eip = kGuestBase;
  reason[0] = '\0';
  status = x86p_jit_engine_run(engine, cpu, 1u, reason, (unsigned)sizeof reason);
  return expect(status == kX86pRunDecodeFailed, "invalid instruction bytes were not reported as a decode failure");
}

int main(void) {
  uint8_t guest[kGuestSize] = {0};
  X86pMem mem = {.host = guest, .lo = kGuestBase, .size = kGuestSize};
  X86pCpu cpu;
  X86pJitEngine *engine = create_engine(&mem);
  int ok;

  if (engine == NULL) {
    return 1;
  }
  ok = translated_program_runs(engine, &cpu, guest);
  if (ok) {
    ok = unsupported_program_is_refused(engine, &cpu, guest);
  }
  if (ok) {
    ok = setz_executes_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = cdq_and_div_execute_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = idiv_executes_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = imul_memory_executes_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = imul_immediate_alias_executes_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = xchg_eax_esp_executes_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = fnclex_executes_before_a_named_refusal(engine, &cpu, guest);
  }
  if (ok) {
    ok = translation_failures_are_distinct(engine, &cpu, guest);
  }
  x86p_jit_engine_destroy(engine);
  return ok ? 0 : 1;
}
