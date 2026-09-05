/* SETcc and LEAVE x64 emission, differentially checked against the test-only
   interpreter oracle. The product library itself never links that oracle. */
#include "code_memory.h"
#include "cpu_compare.h"
#include "exec.h"
#include "jit_engine.h"
#include "jit_x64.h"
#include "x87.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  kGuestBase = 0x00010000u,
  kGuestSize = 4096u,
  kDataOffset = 0x400u,
};

typedef struct Fixture {
  uint8_t guest[kGuestSize];
  uint8_t before[kGuestSize];
  uint8_t expected[kGuestSize];
  X86pMem mem;
  JcCodeRegion code;
  int published;
} Fixture;

static int g_checks;
static int g_failures;
static unsigned g_x87_value_cases;
static unsigned g_x87_precision_refusals;

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(condition)) {                                                                                                \
      g_failures++;                                                                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                      \
    }                                                                                                                  \
  } while (0)

static void put_u32(uint8_t *at, uint32_t value) {
  at[0] = (uint8_t)value;
  at[1] = (uint8_t)(value >> 8);
  at[2] = (uint8_t)(value >> 16);
  at[3] = (uint8_t)(value >> 24);
}

static void put_u64(uint8_t *at, uint64_t value) {
  put_u32(at, (uint32_t)value);
  put_u32(at + 4u, (uint32_t)(value >> 32));
}

static int fixture_init(Fixture *fixture) {
  char reason[256] = {0};

  memset(fixture, 0, sizeof *fixture);
  fixture->mem.host = fixture->guest;
  fixture->mem.lo = kGuestBase;
  fixture->mem.size = kGuestSize;
  if (jc_code_region_create(65536u, &fixture->code, reason, (unsigned)sizeof reason) != kJcCodeOk) {
    printf("REFUSED: code memory: %s\n", reason);
    return 0;
  }
  return 1;
}

static X86pJitStatus translate(Fixture *fixture, X86pJitBlock *block, char *reason, unsigned reason_len) {
  X86pJitStatus status;

  if (fixture->published && jc_code_begin_write(&fixture->code) != kJcCodeOk) {
    snprintf(reason, reason_len, "could not reopen code region for writing");
    return kX86pJitOutOfSpace;
  }
  status =
      x86p_jit_translate(&fixture->mem, kGuestBase, fixture->code.write, fixture->code.size, block, reason, reason_len);
  if (jc_code_publish(&fixture->code, status == kX86pJitOk ? block->host_bytes : 0u) != kJcCodeOk) {
    snprintf(reason, reason_len, "could not publish code region");
    return kX86pJitOutOfSpace;
  }
  fixture->published = 1;
  if (status == kX86pJitOk) {
    block->entry = fixture->code.exec;
  }
  return status;
}

static uint32_t condition_flags(unsigned bits) {
  static const uint32_t masks[] = {X86P_CF, X86P_PF, X86P_ZF, X86P_SF, X86P_OF};
  uint32_t result = X86P_EFLAGS_FIXED;
  unsigned i;

  for (i = 0u; i < sizeof masks / sizeof masks[0]; i++) {
    if ((bits & (1u << i)) != 0u) {
      result |= masks[i];
    }
  }
  return result;
}

static X86pCpu explicit_cpu(unsigned bits) {
  X86pCpu cpu;

  x86p_cpu_reset(&cpu);
  cpu.eip = kGuestBase;
  cpu.reg[kX86pEax] = 0x10203040u;
  cpu.reg[kX86pEcx] = 0x50607080u;
  cpu.reg[kX86pEdx] = 0x90A0B0C0u;
  cpu.reg[kX86pEbx] = 0xD0E0F000u;
  cpu.reg[kX86pEsp] = kGuestBase + 0x800u;
  cpu.flags.kind = (uint8_t)kX86pFlagsExplicit;
  cpu.flags.a = condition_flags(bits);
  cpu.flags.b = 0x13579BDFu;
  cpu.flags.r = 0x2468ACE0u;
  cpu.flags.w = 4u;
  cpu.flags.carry_in = 1u;
  return cpu;
}

static void append_stopper(uint8_t *at) {
  at[0] = 0x0Fu;
  at[1] = 0x53u;
  at[2] = 0xC0u; /* RCPPS XMM0, XMM0: stable named refusal. */
}

static int flags_equal(const X86pFlags *left, const X86pFlags *right) {
  return left->a == right->a && left->b == right->b && left->r == right->r && left->kind == right->kind &&
         left->w == right->w && left->carry_in == right->carry_in;
}

static int compare_one_success_result(
    Fixture *fixture, X86pCpu initial, uint32_t instruction_len, int flags_unchanged, X86pCpu *result) {
  X86pCpu oracle = initial;
  X86pCpu jit = initial;
  X86pFlags flags_before = initial.flags;
  X86pJitBlock block;
  X86pJitStatus translation;
  X86pJitExit exit;
  char reason[256] = {0};

  memcpy(fixture->before, fixture->guest, sizeof fixture->before);
  CHECK(x86p_step(&oracle, &fixture->mem, NULL) == kX86pStepOk);
  memcpy(fixture->expected, fixture->guest, sizeof fixture->expected);
  memcpy(fixture->guest, fixture->before, sizeof fixture->guest);

  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitOk);
  if (translation != kX86pJitOk) {
    printf("  translation: %s\n", reason);
    return 0;
  }
  CHECK(block.insns == 1u);
  CHECK(block.guest_len == instruction_len);
  CHECK(block.stopper != NULL && strcmp(block.stopper, "RCPPS") == 0);
  exit = x86p_jit_enter(&block, &jit);
  CHECK(exit == kX86pJitExitUnsupported);
  CHECK(x86p_cpu_diff(&oracle, &jit, NULL, NULL) == 0u);
  CHECK(memcmp(fixture->expected, fixture->guest, sizeof fixture->guest) == 0);
  if (flags_unchanged) {
    CHECK(flags_equal(&jit.flags, &flags_before));
  }
  if (result) {
    *result = jit;
  }
  return 1;
}

static void compare_one_success(Fixture *fixture, X86pCpu initial, uint32_t instruction_len, int flags_unchanged) {
  (void)compare_one_success_result(fixture, initial, instruction_len, flags_unchanged, NULL);
}

static void
compare_expected_exit(Fixture *fixture, X86pCpu initial, X86pStepStatus expected_step, X86pJitExit expected_exit);

static void test_setcc_all_conditions_and_byte_destinations(Fixture *fixture) {
  unsigned condition;
  unsigned flags;

  for (condition = 0u; condition < (unsigned)kX86pCondCount; condition++) {
    for (flags = 0u; flags < 32u; flags++) {
      X86pCpu initial;
      unsigned reg = condition & 7u;
      uint32_t address = kGuestBase + kDataOffset;

      memset(fixture->guest, 0x90, sizeof fixture->guest);
      fixture->guest[0] = 0x0Fu;
      fixture->guest[1] = (uint8_t)(0x90u + condition);
      fixture->guest[2] = (uint8_t)(0xC0u + reg);
      append_stopper(fixture->guest + 3u);
      initial = explicit_cpu(flags);
      compare_one_success(fixture, initial, 3u, 1);

      memset(fixture->guest, 0x90, sizeof fixture->guest);
      fixture->guest[0] = 0x0Fu;
      fixture->guest[1] = (uint8_t)(0x90u + condition);
      fixture->guest[2] = 0x05u; /* absolute byte [disp32] */
      put_u32(fixture->guest + 3u, address);
      append_stopper(fixture->guest + 7u);
      fixture->guest[kDataOffset] = 0xA5u;
      initial = explicit_cpu(flags);
      compare_one_success(fixture, initial, 7u, 1);
    }
  }
}

static void test_setcc_memory_fault_and_addr16_refusal(Fixture *fixture) {
  X86pCpu initial = explicit_cpu(4u); /* ZF set */
  X86pCpu oracle = initial;
  X86pCpu jit = initial;
  X86pJitBlock block;
  X86pJitStatus translation;
  X86pJitExit exit;
  uint32_t address = kGuestBase + kGuestSize;
  char reason[256] = {0};

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x0Fu;
  fixture->guest[1] = 0x94u;
  fixture->guest[2] = 0x05u;
  put_u32(fixture->guest + 3u, address);
  append_stopper(fixture->guest + 7u);
  memcpy(fixture->before, fixture->guest, sizeof fixture->before);
  CHECK(x86p_step(&oracle, &fixture->mem, NULL) == kX86pStepMemoryFault);
  memcpy(fixture->guest, fixture->before, sizeof fixture->guest);
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitOk);
  if (translation == kX86pJitOk) {
    exit = x86p_jit_enter(&block, &jit);
    CHECK(exit == kX86pJitExitMemoryFault);
    CHECK(x86p_cpu_diff(&oracle, &jit, NULL, NULL) == 0u);
    CHECK(memcmp(fixture->before, fixture->guest, sizeof fixture->guest) == 0);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x67u; /* 16-bit address form remains a named refusal. */
  fixture->guest[1] = 0x0Fu;
  fixture->guest[2] = 0x94u;
  fixture->guest[3] = 0x00u;
  reason[0] = '\0';
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitUnsupportedAtEntry);
  CHECK(strstr(reason, "SETZ") != NULL);
}

static void test_leave_success_and_fault(Fixture *fixture) {
  X86pCpu initial = explicit_cpu(0x1Fu);
  X86pCpu oracle;
  X86pCpu jit;
  X86pJitBlock block;
  X86pJitStatus translation;
  X86pJitExit exit;
  char reason[256] = {0};

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xC9u;
  append_stopper(fixture->guest + 1u);
  initial.reg[kX86pEbp] = kGuestBase + kDataOffset;
  initial.reg[kX86pEsp] = kGuestBase + 0x800u;
  put_u32(fixture->guest + kDataOffset, 0x89ABCDEFu);
  compare_one_success(fixture, initial, 1u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xC9u;
  append_stopper(fixture->guest + 1u);
  initial.reg[kX86pEbp] = kGuestBase + kGuestSize - 3u;
  initial.reg[kX86pEsp] = kGuestBase + 0x800u;
  oracle = initial;
  jit = initial;
  memcpy(fixture->before, fixture->guest, sizeof fixture->before);
  CHECK(x86p_step(&oracle, &fixture->mem, NULL) == kX86pStepMemoryFault);
  CHECK(oracle.reg[kX86pEsp] == initial.reg[kX86pEbp]);
  CHECK(oracle.reg[kX86pEbp] == initial.reg[kX86pEbp]);
  memcpy(fixture->guest, fixture->before, sizeof fixture->guest);
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitOk);
  if (translation == kX86pJitOk) {
    exit = x86p_jit_enter(&block, &jit);
    CHECK(exit == kX86pJitExitMemoryFault);
    CHECK(x86p_cpu_diff(&oracle, &jit, NULL, NULL) == 0u);
    CHECK(memcmp(fixture->before, fixture->guest, sizeof fixture->guest) == 0);
  }
}

static void test_cdq_sign_edges(Fixture *fixture) {
  static const uint32_t values[] = {
      0u,
      1u,
      0x00007FFFu,
      0x00008000u,
      0x7FFFFFFEu,
      0x7FFFFFFFu,
      0x80000000u,
      0x80000001u,
      0xFFFF7FFFu,
      0xFFFFFFFEu,
      0xFFFFFFFFu,
      0xAAAAAAAAu,
  };
  unsigned index;

  for (index = 0u; index < sizeof values / sizeof values[0]; index++) {
    X86pCpu initial = explicit_cpu(index & 31u);

    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0x99u;
    append_stopper(fixture->guest + 1u);
    initial.reg[kX86pEax] = values[index];
    initial.reg[kX86pEdx] = 0x13579BDFu;
    compare_one_success(fixture, initial, 1u, 1);
  }
}

static void test_mul32_register_memory_alias_and_fault(Fixture *fixture) {
  static const struct {
    uint32_t eax;
    uint32_t operand;
    uint8_t reg;
  } cases[] = {
      {0u, 0u, kX86pEcx},
      {1u, 0xFFFFFFFFu, kX86pEcx},
      {0xFFFFFFFFu, 2u, kX86pEcx},
      {0xFFFFFFFFu, 0xFFFFFFFFu, kX86pEcx},
      {0x00010000u, 0x00010000u, kX86pEax},
      {3u, 0x80000000u, kX86pEdx},
  };
  X86pCpu initial;
  uint32_t address = kGuestBase + kDataOffset;
  unsigned index;

  for (index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0xF7u;
    fixture->guest[1] = (uint8_t)(0xE0u + cases[index].reg); /* MUL r32 */
    append_stopper(fixture->guest + 2u);
    initial = explicit_cpu(index & 31u);
    initial.reg[kX86pEax] = cases[index].eax;
    initial.reg[cases[index].reg] = cases[index].operand;
    compare_one_success(fixture, initial, 2u, 0);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0x25u; /* MUL dword [disp32] */
  put_u32(fixture->guest + 2u, address);
  append_stopper(fixture->guest + 6u);
  put_u32(fixture->guest + kDataOffset, 0x80000001u);
  initial = explicit_cpu(19u);
  initial.reg[kX86pEax] = 3u;
  compare_one_success(fixture, initial, 6u, 0);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0x25u;
  put_u32(fixture->guest + 2u, kGuestBase + kGuestSize - 3u);
  append_stopper(fixture->guest + 6u);
  initial = explicit_cpu(23u);
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void
compare_expected_exit(Fixture *fixture, X86pCpu initial, X86pStepStatus expected_step, X86pJitExit expected_exit) {
  X86pCpu oracle = initial;
  X86pCpu jit = initial;
  X86pJitBlock block;
  X86pJitStatus translation;
  X86pJitExit exit;
  char reason[256] = {0};

  memcpy(fixture->before, fixture->guest, sizeof fixture->before);
  CHECK(x86p_step(&oracle, &fixture->mem, NULL) == expected_step);
  memcpy(fixture->expected, fixture->guest, sizeof fixture->expected);
  memcpy(fixture->guest, fixture->before, sizeof fixture->guest);
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitOk);
  if (translation != kX86pJitOk) {
    printf("  translation: %s\n", reason);
    return;
  }
  exit = x86p_jit_enter(&block, &jit);
  CHECK(exit == expected_exit);
  CHECK(x86p_cpu_diff(&oracle, &jit, NULL, NULL) == 0u);
  CHECK(memcmp(fixture->expected, fixture->guest, sizeof fixture->guest) == 0);
}

static void test_div32_success_and_faults(Fixture *fixture) {
  static const struct {
    uint32_t high;
    uint32_t low;
    uint32_t divisor;
  } success[] = {
      {0u, 0u, 1u},
      {0u, 0xFFFFFFFFu, 1u},
      {0u, 0xFFFFFFFFu, 0xFFFFFFFFu},
      {1u, 0u, 2u},
      {0x12345677u, 0xFFFFFFFFu, 0x12345678u},
  };
  X86pCpu initial;
  uint32_t address = kGuestBase + kDataOffset;
  unsigned index;

  for (index = 0u; index < sizeof success / sizeof success[0]; index++) {
    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0xF7u;
    fixture->guest[1] = 0xF1u; /* DIV ECX */
    append_stopper(fixture->guest + 2u);
    initial = explicit_cpu(index & 31u);
    initial.reg[kX86pEdx] = success[index].high;
    initial.reg[kX86pEax] = success[index].low;
    initial.reg[kX86pEcx] = success[index].divisor;
    compare_one_success(fixture, initial, 2u, 1);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0x35u; /* DIV dword [disp32] */
  put_u32(fixture->guest + 2u, address);
  append_stopper(fixture->guest + 6u);
  put_u32(fixture->guest + kDataOffset, 7u);
  initial = explicit_cpu(17u);
  initial.reg[kX86pEdx] = 0u;
  initial.reg[kX86pEax] = 42u;
  compare_one_success(fixture, initial, 6u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0xF1u;
  append_stopper(fixture->guest + 2u);
  initial = explicit_cpu(9u);
  initial.reg[kX86pEdx] = 0x01234567u;
  initial.reg[kX86pEax] = 0x89ABCDEFu;
  initial.reg[kX86pEcx] = 0u;
  compare_expected_exit(fixture, initial, kX86pStepDivideError, kX86pJitExitDivideError);

  initial.reg[kX86pEdx] = 7u;
  initial.reg[kX86pEax] = 0u;
  initial.reg[kX86pEcx] = 7u;
  compare_expected_exit(fixture, initial, kX86pStepDivideError, kX86pJitExitDivideError);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0x35u;
  put_u32(fixture->guest + 2u, kGuestBase + kGuestSize - 3u);
  append_stopper(fixture->guest + 6u);
  initial = explicit_cpu(23u);
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void test_idiv32_success_and_faults(Fixture *fixture) {
  static const struct {
    uint32_t high;
    uint32_t low;
    uint32_t divisor;
  } success[] = {
      {0u, 0u, 1u},
      {0u, 7u, 0xFFFFFFFDu},
      {0xFFFFFFFFu, 0xFFFFFFF9u, 3u},
      {0xFFFFFFFFu, 0x80000000u, 1u},
      {0u, 0x7FFFFFFFu, 1u},
      {0u, 0x7FFFFFFFu, 0xFFFFFFFFu},
  };
  X86pCpu initial;
  uint32_t address = kGuestBase + kDataOffset;
  unsigned index;

  for (index = 0u; index < sizeof success / sizeof success[0]; index++) {
    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0xF7u;
    fixture->guest[1] = 0xF9u; /* IDIV ECX */
    append_stopper(fixture->guest + 2u);
    initial = explicit_cpu((index + 7u) & 31u);
    initial.reg[kX86pEdx] = success[index].high;
    initial.reg[kX86pEax] = success[index].low;
    initial.reg[kX86pEcx] = success[index].divisor;
    compare_one_success(fixture, initial, 2u, 1);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0x3Du; /* IDIV dword [disp32] */
  put_u32(fixture->guest + 2u, address);
  append_stopper(fixture->guest + 6u);
  put_u32(fixture->guest + kDataOffset, 0xFFFFFFFDu);
  initial = explicit_cpu(11u);
  initial.reg[kX86pEdx] = 0xFFFFFFFFu;
  initial.reg[kX86pEax] = 0xFFFFFFF9u;
  compare_one_success(fixture, initial, 6u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0xF9u;
  append_stopper(fixture->guest + 2u);
  initial = explicit_cpu(5u);
  initial.reg[kX86pEdx] = 0x76543210u;
  initial.reg[kX86pEax] = 0x89ABCDEFu;
  initial.reg[kX86pEcx] = 0u;
  compare_expected_exit(fixture, initial, kX86pStepDivideError, kX86pJitExitDivideError);

  initial.reg[kX86pEdx] = 0u;
  initial.reg[kX86pEax] = 0x80000000u;
  initial.reg[kX86pEcx] = 1u;
  compare_expected_exit(fixture, initial, kX86pStepDivideError, kX86pJitExitDivideError);

  initial.reg[kX86pEdx] = 0x80000000u;
  initial.reg[kX86pEax] = 0u;
  initial.reg[kX86pEcx] = 0xFFFFFFFFu;
  compare_expected_exit(fixture, initial, kX86pStepDivideError, kX86pJitExitDivideError);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF7u;
  fixture->guest[1] = 0x3Du;
  put_u32(fixture->guest + 2u, kGuestBase + kGuestSize - 3u);
  append_stopper(fixture->guest + 6u);
  initial = explicit_cpu(29u);
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void test_imul32_register_memory_edges_and_fault(Fixture *fixture) {
  static const struct {
    uint32_t left;
    uint32_t right;
  } cases[] = {
      {0u, 0u},
      {1u, 1u},
      {0xFFFFFFFFu, 1u},
      {0xFFFFFFFFu, 0xFFFFFFFFu},
      {0x7FFFFFFFu, 1u},
      {0x80000000u, 1u},
      {0x7FFFFFFFu, 2u},
      {0x80000000u, 0xFFFFFFFFu},
      {0x00010000u, 0x00010000u},
      {0x80000000u, 0u},
      {0x55555555u, 3u},
      {0xAAAAAAAAu, 3u},
  };
  X86pCpu initial;
  unsigned index;

  for (index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0x0Fu;
    fixture->guest[1] = 0xAFu;
    fixture->guest[2] = 0xC8u; /* IMUL ECX, EAX */
    append_stopper(fixture->guest + 3u);
    initial = explicit_cpu(index & 31u);
    initial.reg[kX86pEcx] = cases[index].left;
    initial.reg[kX86pEax] = cases[index].right;
    compare_one_success(fixture, initial, 3u, 0);

    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0x0Fu;
    fixture->guest[1] = 0xAFu;
    fixture->guest[2] = 0x4Du;
    fixture->guest[3] = 0x0Cu; /* IMUL ECX, dword [EBP+0x0C] */
    append_stopper(fixture->guest + 4u);
    put_u32(fixture->guest + kDataOffset, cases[index].right);
    initial = explicit_cpu((31u - index) & 31u);
    initial.reg[kX86pEcx] = cases[index].left;
    initial.reg[kX86pEbp] = kGuestBase + kDataOffset - 0x0Cu;
    compare_one_success(fixture, initial, 4u, 0);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x0Fu;
  fixture->guest[1] = 0xAFu;
  fixture->guest[2] = 0x4Du;
  fixture->guest[3] = 0x0Cu;
  append_stopper(fixture->guest + 4u);
  initial = explicit_cpu(19u);
  initial.reg[kX86pEbp] = kGuestBase + kGuestSize - 0x0Eu;
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void test_imul32_three_operand_forms(Fixture *fixture) {
  X86pCpu initial;

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x6Bu;
  fixture->guest[1] = 0xF6u;
  fixture->guest[2] = 0x38u; /* IMUL ESI, ESI, 56: aliased source/destination. */
  append_stopper(fixture->guest + 3u);
  initial = explicit_cpu(3u);
  initial.reg[kX86pEsi] = 0x80000000u;
  compare_one_success(fixture, initial, 3u, 0);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x6Bu;
  fixture->guest[1] = 0xD8u;
  fixture->guest[2] = 0xFFu; /* IMUL EBX, EAX, -1: imm8 sign extension. */
  append_stopper(fixture->guest + 3u);
  initial = explicit_cpu(13u);
  initial.reg[kX86pEax] = 0x80000000u;
  compare_one_success(fixture, initial, 3u, 0);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x69u;
  fixture->guest[1] = 0xD8u;
  put_u32(fixture->guest + 2u, 0x80000000u);
  append_stopper(fixture->guest + 6u);
  initial = explicit_cpu(21u);
  initial.reg[kX86pEax] = 2u;
  compare_one_success(fixture, initial, 6u, 0);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x6Bu;
  fixture->guest[1] = 0x75u;
  fixture->guest[2] = 0x0Cu;
  fixture->guest[3] = 0x38u; /* IMUL ESI, dword [EBP+0x0C], 56. */
  append_stopper(fixture->guest + 4u);
  put_u32(fixture->guest + kDataOffset, 3u);
  initial = explicit_cpu(27u);
  initial.reg[kX86pEbp] = kGuestBase + kDataOffset - 0x0Cu;
  compare_one_success(fixture, initial, 4u, 0);

  initial = explicit_cpu(15u);
  initial.reg[kX86pEbp] = kGuestBase + kGuestSize - 0x0Eu;
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void test_rep_cmpsb_termination_direction_and_fault_progress(Fixture *fixture) {
  X86pCpu initial;

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF3u;
  fixture->guest[1] = 0xA6u; /* REPE CMPSB */
  append_stopper(fixture->guest + 2u);
  fixture->guest[kDataOffset + 0u] = 1u;
  fixture->guest[kDataOffset + 1u] = 2u;
  fixture->guest[kDataOffset + 2u] = 3u;
  fixture->guest[kDataOffset + 0x100u] = 1u;
  fixture->guest[kDataOffset + 0x101u] = 2u;
  fixture->guest[kDataOffset + 0x102u] = 4u;
  initial = explicit_cpu(0u);
  initial.reg[kX86pEcx] = 5u;
  initial.reg[kX86pEsi] = kGuestBase + kDataOffset;
  initial.reg[kX86pEdi] = kGuestBase + kDataOffset + 0x100u;
  compare_one_success(fixture, initial, 2u, 0);

  initial = explicit_cpu(4u);
  initial.reg[kX86pEcx] = 0u;
  initial.reg[kX86pEsi] = kGuestBase + kGuestSize;
  initial.reg[kX86pEdi] = kGuestBase + kGuestSize;
  compare_one_success(fixture, initial, 2u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF3u;
  fixture->guest[1] = 0xA6u;
  append_stopper(fixture->guest + 2u);
  fixture->guest[kDataOffset + 2u] = 7u;
  fixture->guest[kDataOffset + 1u] = 6u;
  fixture->guest[kDataOffset + 0x102u] = 7u;
  fixture->guest[kDataOffset + 0x101u] = 5u;
  initial = explicit_cpu(0u);
  initial.df = 1u;
  initial.reg[kX86pEcx] = 4u;
  initial.reg[kX86pEsi] = kGuestBase + kDataOffset + 2u;
  initial.reg[kX86pEdi] = kGuestBase + kDataOffset + 0x102u;
  compare_one_success(fixture, initial, 2u, 0);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xF3u;
  fixture->guest[1] = 0xA6u;
  append_stopper(fixture->guest + 2u);
  fixture->guest[kGuestSize - 1u] = 0x55u;
  fixture->guest[kDataOffset] = 0x55u;
  initial = explicit_cpu(0u);
  initial.reg[kX86pEcx] = 3u;
  initial.reg[kX86pEsi] = kGuestBase + kGuestSize - 1u;
  initial.reg[kX86pEdi] = kGuestBase + kDataOffset;
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);

  initial = explicit_cpu(0u);
  initial.reg[kX86pEcx] = 3u;
  initial.reg[kX86pEsi] = kGuestBase + kDataOffset;
  initial.reg[kX86pEdi] = kGuestBase + kGuestSize;
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void test_xchg32_register_memory_alias_and_fault(Fixture *fixture) {
  X86pCpu initial;
  uint32_t address = kGuestBase + kDataOffset;

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x94u; /* XCHG EAX, ESP */
  append_stopper(fixture->guest + 1u);
  initial = explicit_cpu(31u);
  compare_one_success(fixture, initial, 1u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x87u;
  fixture->guest[1] = 0xC9u; /* XCHG ECX, ECX */
  append_stopper(fixture->guest + 2u);
  initial = explicit_cpu(10u);
  compare_one_success(fixture, initial, 2u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x87u;
  fixture->guest[1] = 0x0Du; /* XCHG dword [disp32], ECX */
  put_u32(fixture->guest + 2u, address);
  append_stopper(fixture->guest + 6u);
  put_u32(fixture->guest + kDataOffset, 0xA1B2C3D4u);
  initial = explicit_cpu(22u);
  compare_one_success(fixture, initial, 6u, 1);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0x87u;
  fixture->guest[1] = 0x0Du;
  put_u32(fixture->guest + 2u, kGuestBase + kGuestSize - 3u);
  append_stopper(fixture->guest + 6u);
  initial = explicit_cpu(6u);
  compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
}

static void expect_x87_precision_refusal(Fixture *fixture, X86pCpu initial) {
  X86pCpu cpu = initial;
  X86pInsn decoded;
  X86pJitEngine *engine;
  X86pJitEngineStats stats;
  char reason[256] = {0};

  CHECK(!x86p_x87_precision_is_exact());
  if (x86p_decode(fixture->guest, X86P_MAX_INSN_LEN, &decoded) == 0u) {
    CHECK(0 && "precision-refusal fixture must decode");
    return;
  }
  CHECK(decoded.op == kX86pInsnX87);
  CHECK(!x86p_jit_can_translate(&decoded));
  memcpy(fixture->before, fixture->guest, sizeof fixture->before);
  engine = x86p_jit_engine_create(&fixture->mem, 65536u, 256u, reason, sizeof reason);
  CHECK(engine != NULL);
  if (!engine) {
    printf("  precision-refusal fixture: %s\n", reason);
    return;
  }
  CHECK(x86p_jit_engine_run(engine, &cpu, 1u, reason, sizeof reason) == kX86pRunUnsupported);
  CHECK(strstr(reason, decoded.mnemonic) != NULL);
  CHECK(x86p_cpu_diff(&cpu, &initial, NULL, NULL) == 0u);
  CHECK(memcmp(fixture->guest, fixture->before, sizeof fixture->guest) == 0);
  x86p_jit_engine_stats(engine, &stats);
  CHECK(stats.blocks_entered == 0u);
  CHECK(stats.blocks_translated == 0u);
  CHECK(stats.guest_insns_translated == 0u);
  CHECK(stats.translate_refusals == 1u);
  x86p_jit_engine_destroy(engine);
  g_x87_precision_refusals++;
}

static void compare_x87_value_success(Fixture *fixture, X86pCpu initial, uint32_t length) {
  g_x87_value_cases++;
  if (x86p_x87_precision_is_exact()) {
    compare_one_success(fixture, initial, length, 1);
  } else {
    expect_x87_precision_refusal(fixture, initial);
  }
}

static void test_x87_constant_loads_and_full_stack(Fixture *fixture) {
  static const uint8_t second_opcode[] = {0xE8u, 0xE9u, 0xEAu, 0xEBu, 0xECu, 0xEDu, 0xEEu};
  X86pCpu initial;
  unsigned index;

  for (index = 0u; index < sizeof second_opcode / sizeof second_opcode[0]; index++) {
    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0xD9u;
    fixture->guest[1] = second_opcode[index];
    append_stopper(fixture->guest + 2u);
    initial = explicit_cpu(index & 31u);
    compare_x87_value_success(fixture, initial, 2u);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xD9u;
  fixture->guest[1] = 0xEEu; /* FLDZ on a full stack: stack fault, no push. */
  append_stopper(fixture->guest + 2u);
  initial = explicit_cpu(12u);
  for (index = 0u; index < X86P_X87_REGS; index++) {
    CHECK(x86p_x87_push(&initial.x87, (long double)(index + 1u)) != 0);
  }
  compare_x87_value_success(fixture, initial, 2u);
}

static void test_x87_memory_compare_status_pop_nan_and_fault(Fixture *fixture) {
  static const uint64_t operands[] = {
      UINT64_C(0xC010000000000000), /* -4.0: ST(0) is greater, C0/C2/C3 clear. */
      UINT64_C(0x3FF0000000000000), /*  1.0: equal, C3 set. */
      UINT64_C(0x4010000000000000), /*  4.0: ST(0) is less, C0 set. */
      UINT64_C(0x7FF8000000000001), /* quiet NaN: unordered and IE set. */
  };
  X86pCpu initial;
  X86pJitBlock block;
  X86pJitStatus translation;
  char reason[256] = {0};
  unsigned index;

  for (index = 0u; index < sizeof operands / sizeof operands[0]; index++) {
    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0xDCu;
    fixture->guest[1] = 0x5Du;
    fixture->guest[2] = 0xF8u; /* FCOMP qword [EBP-8]: the X-Men 2 frontier encoding. */
    append_stopper(fixture->guest + 3u);
    put_u64(fixture->guest + kDataOffset, operands[index]);
    initial = explicit_cpu(index & 31u);
    initial.reg[kX86pEbp] = kGuestBase + kDataOffset + 8u;
    initial.x87.status = X86P_X87_ZE | X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
    CHECK(x86p_x87_push(&initial.x87, 1.0L) != 0);
    /* Full CPU comparison includes C0/C2/C3/IE, tags, TOP and the one pop. */
    compare_x87_value_success(fixture, initial, 3u);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xD8u;
  fixture->guest[1] = 0x15u; /* FCOM dword [disp32]: compare without pop. */
  put_u32(fixture->guest + 2u, kGuestBase + kDataOffset);
  append_stopper(fixture->guest + 6u);
  put_u32(fixture->guest + kDataOffset, UINT32_C(0x3F800000));
  initial = explicit_cpu(9u);
  CHECK(x86p_x87_push(&initial.x87, 1.0L) != 0);
  compare_x87_value_success(fixture, initial, 6u);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xDCu;
  fixture->guest[1] = 0x1Du; /* FCOMP qword [disp32] with an empty ST(0). */
  put_u32(fixture->guest + 2u, kGuestBase + kDataOffset);
  append_stopper(fixture->guest + 6u);
  put_u64(fixture->guest + kDataOffset, UINT64_C(0x3FF0000000000000));
  initial = explicit_cpu(14u);
  compare_x87_value_success(fixture, initial, 6u);

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xDCu;
  fixture->guest[1] = 0x1Du;
  put_u32(fixture->guest + 2u, kGuestBase + kGuestSize - 7u);
  append_stopper(fixture->guest + 6u);
  initial = explicit_cpu(20u);
  CHECK(x86p_x87_push(&initial.x87, 2.0L) != 0);
  /* The failed read must not compare or pop; the full-state diff proves both. */
  g_x87_value_cases++;
  if (x86p_x87_precision_is_exact()) {
    compare_expected_exit(fixture, initial, kX86pStepMemoryFault, kX86pJitExitMemoryFault);
  } else {
    expect_x87_precision_refusal(fixture, initial);
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xD8u;
  fixture->guest[1] = 0xD9u; /* FCOMP ST(1): deliberately outside this memory milestone. */
  append_stopper(fixture->guest + 2u);
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitUnsupportedAtEntry);
  CHECK(strstr(reason, "FCOMP") != NULL);
}

static void test_x87_store_status_ax_projection_and_memory_refusal(Fixture *fixture) {
  X86pCpu initial;
  X86pCpu result;
  X86pInsn decoded;
  X86pJitBlock block;
  X86pJitStatus translation;
  char reason[256] = {0};
  unsigned top;

  for (top = 0u; top < X86P_X87_REGS; top++) {
    uint16_t expected;

    memset(fixture->guest, 0x90, sizeof fixture->guest);
    fixture->guest[0] = 0xDFu;
    fixture->guest[1] = 0xE0u; /* FNSTSW AX: the X-Men 2 frontier encoding. */
    append_stopper(fixture->guest + 2u);
    CHECK(x86p_decode(fixture->guest, 2u, &decoded) == 2u);
    CHECK(decoded.operands == 1);
    CHECK(decoded.operand[0].kind == kX86pOperandReg);
    CHECK(decoded.operand[0].reg == kX86pEax);
    CHECK(decoded.operand[0].size == 2);
    initial = explicit_cpu(top & 31u);
    initial.reg[kX86pEax] = UINT32_C(0xA5A50000) | top;
    initial.x87.status = (uint16_t)(X86P_X87_IE | X86P_X87_ZE | X86P_X87_SF | X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 |
                                    X86P_X87_C3 | (7u << X86P_X87_TOP_SHIFT));
    initial.x87.top = (uint8_t)top;
    result = initial;
    expected = x86p_x87_status(&initial.x87);
    if (compare_one_success_result(fixture, initial, 2u, 1, &result)) {
      X86pCpu x87_expected = result;
      CHECK(result.reg[kX86pEax] == (UINT32_C(0xA5A50000) | expected));
      x87_expected.x87 = initial.x87;
      CHECK(x86p_cpu_diff(&x87_expected, &result, NULL, NULL) == 0u);
    }
  }

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xDDu;
  fixture->guest[1] = 0x3Du; /* FNSTSW word [disp32]: not part of the AX milestone. */
  put_u32(fixture->guest + 2u, kGuestBase + kDataOffset);
  append_stopper(fixture->guest + 6u);
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitUnsupportedAtEntry);
  CHECK(strstr(reason, "FNSTSW") != NULL);
}

static void test_x87_clear_exceptions_exact_state_and_neighbor_refusal(Fixture *fixture) {
  X86pCpu initial = explicit_cpu(27u);
  X86pCpu result = initial;
  X86pInsn decoded;
  X86pJitBlock block;
  X86pJitStatus translation;
  char reason[256] = {0};
  long double registers[X86P_X87_REGS];
  uint8_t tags[X86P_X87_REGS];
  unsigned index;

  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xDBu;
  fixture->guest[1] = 0xE2u; /* FNCLEX: the X-Men 2 frontier encoding. */
  append_stopper(fixture->guest + 2u);
  CHECK(x86p_decode(fixture->guest, 2u, &decoded) == 2u);
  CHECK(decoded.op == kX86pInsnX87);
  CHECK(decoded.x87 == (uint8_t)kX86pX87InsnClearExc);
  CHECK(decoded.operands == 0);

  for (index = 0u; index < 3u; index++) {
    CHECK(x86p_x87_push(&initial.x87, (long double)(index + 1u)) != 0);
  }
  initial.x87.control = UINT16_C(0x0B7F);
  initial.x87.status = UINT16_C(0xFFFF);
  memcpy(registers, initial.x87.reg, sizeof registers);
  memcpy(tags, initial.x87.tag, sizeof tags);
  if (compare_one_success_result(fixture, initial, 2u, 1, &result)) {
    CHECK(result.x87.status == (uint16_t)(initial.x87.status & (uint16_t)~X86P_X87_FNCLEX_MASK));
    CHECK(result.x87.top == initial.x87.top);
    CHECK(result.x87.control == initial.x87.control);
    CHECK(memcmp(result.x87.tag, tags, sizeof tags) == 0);
    for (index = 0u; index < X86P_X87_REGS; index++) {
      CHECK(result.x87.reg[index] == registers[index]);
    }
  }

  /* DB E3 is neighboring FNINIT, not a second spelling of FNCLEX. Until it
     has its own emitter it must remain a named product refusal. */
  memset(fixture->guest, 0x90, sizeof fixture->guest);
  fixture->guest[0] = 0xDBu;
  fixture->guest[1] = 0xE3u;
  append_stopper(fixture->guest + 2u);
  reason[0] = '\0';
  translation = translate(fixture, &block, reason, (unsigned)sizeof reason);
  CHECK(translation == kX86pJitUnsupportedAtEntry);
  CHECK(strstr(reason, "FNINIT") != NULL);
}

int main(void) {
  Fixture fixture;

  if (!x86p_jit_available()) {
    printf("NO x86-64 BACKEND: SETcc/LEAVE differential claims nothing\n");
    return 77;
  }
  if (!fixture_init(&fixture)) {
    return 1;
  }
  test_setcc_all_conditions_and_byte_destinations(&fixture);
  test_setcc_memory_fault_and_addr16_refusal(&fixture);
  test_leave_success_and_fault(&fixture);
  test_cdq_sign_edges(&fixture);
  test_mul32_register_memory_alias_and_fault(&fixture);
  test_div32_success_and_faults(&fixture);
  test_idiv32_success_and_faults(&fixture);
  test_imul32_register_memory_edges_and_fault(&fixture);
  test_imul32_three_operand_forms(&fixture);
  test_rep_cmpsb_termination_direction_and_fault_progress(&fixture);
  test_xchg32_register_memory_alias_and_fault(&fixture);
  test_x87_constant_loads_and_full_stack(&fixture);
  test_x87_memory_compare_status_pop_nan_and_fault(&fixture);
  test_x87_store_status_ax_projection_and_memory_refusal(&fixture);
  test_x87_clear_exceptions_exact_state_and_neighbor_refusal(&fixture);
  jc_code_region_destroy(&fixture.code);
  CHECK(g_x87_value_cases == 15u);
  CHECK(g_x87_precision_refusals == (x86p_x87_precision_is_exact() ? 0u : g_x87_value_cases));
  printf("x87 value forms: %u cases, %u explicit precision refusals; status-only execution checked independently\n",
         g_x87_value_cases,
         g_x87_precision_refusals);
  printf("%d check(s), %d failure(s): SETcc, LEAVE, CDQ, MUL, DIV/IDIV, IMUL, REP CMPSB, XCHG, and x87\n",
         g_checks,
         g_failures);
  return g_failures == 0 ? 0 : 1;
}
