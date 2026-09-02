/*
 * test_emit_x64 -- does the encoder emit the instruction it was asked for?
 *
 * NOT CHECKED BY READING THE BYTES. Hand-written expected byte strings test
 * that the encoder agrees with whoever wrote the test, and both can hold the
 * same misreading of the manual -- which is precisely how the RSP-needs-a-SIB
 * and RBP-needs-a-disp8 traps survive review. So every instruction here is
 * DECODED with Zydis, the same decoder the guest side already trusts, and the
 * decoded registers and displacement are compared against what was requested.
 * The oracle is an independent implementation of the encoding, not my opinion
 * of it.
 *
 * The sweep is exhaustive over registers rather than sampled. There are only
 * sixteen, the traps live at four of them (RSP, RBP, R12, R13), and a sample
 * that happens to miss those reports a clean run over a broken encoder --
 * which is the failure this file exists to prevent.
 */
#include "emit_x64.h"

#include <Zydis/Zydis.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;
static int g_test_failed;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                       \
    }                                                                                                                  \
  } while (0)

#define RUN(fn)                                                                                                        \
  do {                                                                                                                 \
    int before = g_failed;                                                                                             \
    printf("test %s\n", #fn);                                                                                          \
    fn();                                                                                                              \
    if (g_failed != before) {                                                                                          \
      g_test_failed++;                                                                                                 \
      printf("  FAIL\n");                                                                                              \
    } else {                                                                                                           \
      printf("  PASS\n");                                                                                              \
    }                                                                                                                  \
  } while (0)

/* How many instructions the oracle actually decoded. Printed at the end as the
   denominator: "all assertions passed" over zero decoded instructions is the
   report a broken harness produces, and it must not look like success. */
static unsigned long g_decoded;

typedef struct Decoded {
  int ok;
  ZydisDecodedInstruction insn;
  ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
} Decoded;

static Decoded decode64(const uint8_t *bytes, size_t len) {
  static ZydisDecoder decoder;
  static int inited;
  Decoded d;
  memset(&d, 0, sizeof d);
  if (!inited) {
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
      printf("    FATAL: could not initialise the 64-bit decoder; this suite has no oracle\n");
      return d;
    }
    inited = 1;
  }
  if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, len, &d.insn, d.ops))) {
    return d;
  }
  d.ok = 1;
  g_decoded++;
  return d;
}

/*
 * Decode one emitted instruction and require that it consumed EXACTLY the bytes
 * emitted.
 *
 * The length check is not incidental. An encoder that emits a spurious extra
 * byte still decodes to the right instruction -- Zydis simply stops early --
 * and the trailing byte then becomes the first byte of whatever follows. In a
 * block of translated code that is a garbage instruction in the middle of a
 * working sequence, which is about the least debuggable thing this module could
 * produce.
 */
static Decoded emit_and_decode(const X86pEmit *e) {
  Decoded d;
  if (!x86p_emit_ok(e)) {
    printf("    FAIL: emitter overflowed\n");
    g_checks++;
    g_failed++;
    memset(&d, 0, sizeof d);
    return d;
  }
  d = decode64(e->buf, e->len);
  g_checks++;
  if (!d.ok) {
    g_failed++;
    printf("    FAIL: %zu emitted byte(s) did not decode\n", e->len);
    return d;
  }
  g_checks++;
  if (d.insn.length != e->len) {
    g_failed++;
    printf("    FAIL: emitted %zu byte(s), decoder consumed %u\n", e->len, (unsigned)d.insn.length);
  }
  return d;
}

static int reg_id(ZydisRegister r) {
  return (int)ZydisRegisterGetId(r);
}

/* ---- moves -------------------------------------------------------------- */

static void test_mov_r32_r32_every_pair(void) {
  int dst;
  int src;
  for (dst = 0; dst < kX64RegCount; dst++) {
    for (src = 0; src < kX64RegCount; src++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_mov_r32_r32(&e, (X86pHostReg)dst, (X86pHostReg)src);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_MOV);
      CHECK(d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && reg_id(d.ops[0].reg.value) == dst);
      CHECK(d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER && reg_id(d.ops[1].reg.value) == src);
      /* 32-bit operands: a missing REX.W is not what makes this 32-bit, and a
         stray one would silently widen every guest register write to 64 bits. */
      CHECK(ZydisRegisterGetClass(d.ops[0].reg.value) == ZYDIS_REGCLASS_GPR32);
    }
  }
}

static void test_mov_r64_r64_is_64_bit(void) {
  int dst;
  for (dst = 0; dst < kX64RegCount; dst++) {
    uint8_t buf[16];
    X86pEmit e;
    Decoded d;
    x86p_emit_init(&e, buf, sizeof buf);
    x86p_emit_mov_r64_r64(&e, (X86pHostReg)dst, kX64Rdi);
    d = emit_and_decode(&e);
    if (!d.ok) {
      continue;
    }
    CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_MOV);
    CHECK(ZydisRegisterGetClass(d.ops[0].reg.value) == ZYDIS_REGCLASS_GPR64);
    CHECK(reg_id(d.ops[0].reg.value) == dst);
    CHECK(reg_id(d.ops[1].reg.value) == kX64Rdi);
  }
}

static void test_mov_r32_imm32(void) {
  static const uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu};
  int dst;
  size_t v;
  for (dst = 0; dst < kX64RegCount; dst++) {
    for (v = 0; v < sizeof values / sizeof values[0]; v++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_mov_r32_imm32(&e, (X86pHostReg)dst, values[v]);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_MOV);
      CHECK(reg_id(d.ops[0].reg.value) == dst);
      CHECK(d.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);
      CHECK((uint32_t)d.ops[1].imm.value.u == values[v]);
    }
  }
}

/* ---- memory: where all three traps live --------------------------------- */

/*
 * Displacements chosen at the encoding's decision boundaries, not at round
 * numbers: 0 selects mod 00 (except at RBP/R13), 127/-128 are the last disp8
 * values, and 128/-129 are the first that must widen to disp32. An encoder that
 * picks the wrong mod at a boundary is wrong for a narrow band of offsets --
 * and guest structure fields land exactly in that band.
 */
static const int32_t kDisps[] = {0, 1, -1, 127, -128, 128, -129, 4096, -100000};

static void test_load32_every_base_and_displacement(void) {
  int base;
  size_t i;
  for (base = 0; base < kX64RegCount; base++) {
    for (i = 0; i < sizeof kDisps / sizeof kDisps[0]; i++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_load32(&e, kX64Rax, (X86pHostReg)base, kDisps[i]);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_MOV);
      CHECK(reg_id(d.ops[0].reg.value) == kX64Rax);
      CHECK(d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY);
      /* The base must be the register asked for -- and NOT RIP, which is what
         RBP and R13 decode to when the zero displacement is left implicit. */
      CHECK(d.ops[1].mem.base != ZYDIS_REGISTER_RIP);
      CHECK(reg_id(d.ops[1].mem.base) == base);
      CHECK(d.ops[1].mem.index == ZYDIS_REGISTER_NONE);
      CHECK((int32_t)d.ops[1].mem.disp.value == kDisps[i]);
      CHECK(d.ops[1].size == 32u);
    }
  }
}

static void test_store32_every_base_and_displacement(void) {
  int base;
  size_t i;
  for (base = 0; base < kX64RegCount; base++) {
    for (i = 0; i < sizeof kDisps / sizeof kDisps[0]; i++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_store32(&e, (X86pHostReg)base, kDisps[i], kX64Rcx);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_MOV);
      CHECK(d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY);
      CHECK(d.ops[0].mem.base != ZYDIS_REGISTER_RIP);
      CHECK(reg_id(d.ops[0].mem.base) == base);
      CHECK((int32_t)d.ops[0].mem.disp.value == kDisps[i]);
      CHECK(d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER && reg_id(d.ops[1].reg.value) == kX64Rcx);
    }
  }
}

static void test_store8_imm_is_a_byte_store(void) {
  int base;
  for (base = 0; base < kX64RegCount; base++) {
    uint8_t buf[16];
    X86pEmit e;
    Decoded d;
    x86p_emit_init(&e, buf, sizeof buf);
    x86p_emit_store8_imm(&e, (X86pHostReg)base, 12, 0xABu);
    d = emit_and_decode(&e);
    if (!d.ok) {
      continue;
    }
    CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_MOV);
    CHECK(d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY);
    /* A BYTE store. Widening this to 32 bits would clobber the three X86pFlags
       fields that follow `kind`, which is a corruption no register comparison
       would attribute to the store. */
    CHECK(d.ops[0].size == 8u);
    CHECK(reg_id(d.ops[0].mem.base) == base);
    CHECK((int32_t)d.ops[0].mem.disp.value == 12);
    CHECK(d.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);
    CHECK((uint8_t)d.ops[1].imm.value.u == 0xABu);
  }
}

/* ---- arithmetic --------------------------------------------------------- */

static void test_alu_r32_r32_every_op(void) {
  static const ZydisMnemonic want[kX64AluCount] = {ZYDIS_MNEMONIC_ADD,
                                                   ZYDIS_MNEMONIC_OR,
                                                   ZYDIS_MNEMONIC_ADC,
                                                   ZYDIS_MNEMONIC_SBB,
                                                   ZYDIS_MNEMONIC_AND,
                                                   ZYDIS_MNEMONIC_SUB,
                                                   ZYDIS_MNEMONIC_XOR,
                                                   ZYDIS_MNEMONIC_CMP};
  int op;
  int dst;
  for (op = 0; op < kX64AluCount; op++) {
    for (dst = 0; dst < kX64RegCount; dst++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_alu_r32_r32(&e, (X86pHostAlu)op, (X86pHostReg)dst, kX64R11);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      /* The opcode is computed as op*8+1 rather than looked up, so this check
         is what proves that arithmetic matches the manual's ordering. */
      CHECK(d.insn.mnemonic == want[op]);
      CHECK(reg_id(d.ops[0].reg.value) == dst);
      CHECK(reg_id(d.ops[1].reg.value) == kX64R11);
    }
  }
}

static void test_alu_r32_imm32_every_op(void) {
  static const ZydisMnemonic want[kX64AluCount] = {ZYDIS_MNEMONIC_ADD,
                                                   ZYDIS_MNEMONIC_OR,
                                                   ZYDIS_MNEMONIC_ADC,
                                                   ZYDIS_MNEMONIC_SBB,
                                                   ZYDIS_MNEMONIC_AND,
                                                   ZYDIS_MNEMONIC_SUB,
                                                   ZYDIS_MNEMONIC_XOR,
                                                   ZYDIS_MNEMONIC_CMP};
  int op;
  int dst;
  for (op = 0; op < kX64AluCount; op++) {
    for (dst = 0; dst < kX64RegCount; dst++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_alu_r32_imm32(&e, (X86pHostAlu)op, (X86pHostReg)dst, 0x12345678u);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      CHECK(d.insn.mnemonic == want[op]);
      CHECK(reg_id(d.ops[0].reg.value) == dst);
      CHECK(d.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);
      CHECK((uint32_t)d.ops[1].imm.value.u == 0x12345678u);
    }
  }
}

static void test_test_r32_r32(void) {
  int a;
  for (a = 0; a < kX64RegCount; a++) {
    uint8_t buf[16];
    X86pEmit e;
    Decoded d;
    x86p_emit_init(&e, buf, sizeof buf);
    x86p_emit_test_r32_r32(&e, (X86pHostReg)a, (X86pHostReg)a);
    d = emit_and_decode(&e);
    if (!d.ok) {
      continue;
    }
    CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_TEST);
    CHECK(reg_id(d.ops[0].reg.value) == a);
    CHECK(reg_id(d.ops[1].reg.value) == a);
    CHECK(ZydisRegisterGetClass(d.ops[0].reg.value) == ZYDIS_REGCLASS_GPR32);
  }
}

/*
 * All sixteen conditions, decoded.
 *
 * The condition number is added straight to a base opcode, so an off-by-one
 * here emits a DIFFERENT, perfectly valid conditional move -- CMOVB where
 * CMOVBE was meant. That never faults and never looks wrong in a register
 * dump; it just takes the wrong branch on some inputs and not others. Only
 * decoding each of the sixteen catches it.
 */
static void test_cmovcc_every_condition(void) {
  static const ZydisMnemonic want[16] = {ZYDIS_MNEMONIC_CMOVO,
                                         ZYDIS_MNEMONIC_CMOVNO,
                                         ZYDIS_MNEMONIC_CMOVB,
                                         ZYDIS_MNEMONIC_CMOVNB,
                                         ZYDIS_MNEMONIC_CMOVZ,
                                         ZYDIS_MNEMONIC_CMOVNZ,
                                         ZYDIS_MNEMONIC_CMOVBE,
                                         ZYDIS_MNEMONIC_CMOVNBE,
                                         ZYDIS_MNEMONIC_CMOVS,
                                         ZYDIS_MNEMONIC_CMOVNS,
                                         ZYDIS_MNEMONIC_CMOVP,
                                         ZYDIS_MNEMONIC_CMOVNP,
                                         ZYDIS_MNEMONIC_CMOVL,
                                         ZYDIS_MNEMONIC_CMOVNL,
                                         ZYDIS_MNEMONIC_CMOVLE,
                                         ZYDIS_MNEMONIC_CMOVNLE};
  unsigned cc;
  int dst;
  for (cc = 0; cc < 16u; cc++) {
    for (dst = 0; dst < kX64RegCount; dst++) {
      uint8_t buf[16];
      X86pEmit e;
      Decoded d;
      x86p_emit_init(&e, buf, sizeof buf);
      x86p_emit_cmovcc_r32_r32(&e, cc, (X86pHostReg)dst, kX64R10);
      d = emit_and_decode(&e);
      if (!d.ok) {
        continue;
      }
      CHECK(d.insn.mnemonic == want[cc]);
      /* Destination is the REG field and source the RM field. Swapping them
         decodes to a valid CMOV in the wrong direction. */
      CHECK(reg_id(d.ops[0].reg.value) == dst);
      CHECK(reg_id(d.ops[1].reg.value) == kX64R10);
    }
  }
}

/* ---- structure ---------------------------------------------------------- */

static void test_push_pop_every_register(void) {
  int r;
  for (r = 0; r < kX64RegCount; r++) {
    uint8_t buf[16];
    X86pEmit e;
    Decoded d;

    x86p_emit_init(&e, buf, sizeof buf);
    x86p_emit_push_r64(&e, (X86pHostReg)r);
    d = emit_and_decode(&e);
    if (d.ok) {
      CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_PUSH);
      CHECK(reg_id(d.ops[0].reg.value) == r);
      CHECK(ZydisRegisterGetClass(d.ops[0].reg.value) == ZYDIS_REGCLASS_GPR64);
    }

    x86p_emit_init(&e, buf, sizeof buf);
    x86p_emit_pop_r64(&e, (X86pHostReg)r);
    d = emit_and_decode(&e);
    if (d.ok) {
      CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_POP);
      CHECK(reg_id(d.ops[0].reg.value) == r);
    }
  }
}

static void test_ret(void) {
  uint8_t buf[16];
  X86pEmit e;
  Decoded d;
  x86p_emit_init(&e, buf, sizeof buf);
  x86p_emit_ret(&e);
  d = emit_and_decode(&e);
  if (d.ok) {
    CHECK(d.insn.mnemonic == ZYDIS_MNEMONIC_RET);
  }
}

/* ---- the negative: overflow is reported, not truncated ------------------ */

static void test_overflow_is_sticky_and_never_writes_past_the_end(void) {
  uint8_t buf[4];
  uint8_t guard[8];
  X86pEmit e;
  int i;

  memset(guard, 0xCD, sizeof guard);

  x86p_emit_init(&e, buf, sizeof buf);
  CHECK(x86p_emit_ok(&e));

  /* Five 5-byte MOVs into a 4-byte buffer: the first must not fit. */
  for (i = 0; i < 5; i++) {
    x86p_emit_mov_r32_imm32(&e, kX64Rax, 0x11111111u);
  }
  CHECK(!x86p_emit_ok(&e));
  CHECK(e.len <= sizeof buf);

  /* STICKY. A later small emit that would fit must not clear the failure --
     otherwise a caller checking once at the end sees a clean result over a
     block with a hole in the middle. */
  x86p_emit_ret(&e);
  CHECK(!x86p_emit_ok(&e));

  /* A null buffer is overflowed from the start rather than a crash. */
  x86p_emit_init(&e, NULL, 64);
  CHECK(!x86p_emit_ok(&e));
  x86p_emit_ret(&e);
  CHECK(!x86p_emit_ok(&e));

  for (i = 0; i < (int)sizeof guard; i++) {
    CHECK(guard[i] == 0xCD);
  }
}

int main(void) {
  RUN(test_mov_r32_r32_every_pair);
  RUN(test_mov_r64_r64_is_64_bit);
  RUN(test_mov_r32_imm32);
  RUN(test_load32_every_base_and_displacement);
  RUN(test_store32_every_base_and_displacement);
  RUN(test_store8_imm_is_a_byte_store);
  RUN(test_alu_r32_r32_every_op);
  RUN(test_alu_r32_imm32_every_op);
  RUN(test_test_r32_r32);
  RUN(test_cmovcc_every_condition);
  RUN(test_push_pop_every_register);
  RUN(test_ret);
  RUN(test_overflow_is_sticky_and_never_writes_past_the_end);

  printf("\n%d check(s), %d failure(s) in %d test(s)\n", g_checks, g_failed, g_test_failed);
  printf("%lu instruction(s) decoded by the Zydis oracle\n", g_decoded);
  if (g_decoded == 0u) {
    printf("REFUSED: the oracle decoded nothing; these results mean nothing\n");
    return 1;
  }
  return g_failed ? 1 : 0;
}
