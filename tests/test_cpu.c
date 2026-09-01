/*
 * test_cpu -- the register file and guest memory.
 *
 * No hardware oracle here, and that is not a gap: none of this is arithmetic a
 * host instruction can be asked to reproduce. It is bit placement and bounds,
 * where the failures are specific and enumerable, so they are enumerated --
 * exhaustively where the space allows, which for the byte-register aliasing it
 * does.
 */
#include "cpu.h"

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

#define CHECK_EQ_U(got, want)                                                                                          \
  do {                                                                                                                 \
    unsigned long long g_ = (unsigned long long)(got);                                                                 \
    unsigned long long w_ = (unsigned long long)(want);                                                                \
    g_checks++;                                                                                                        \
    if (g_ != w_) {                                                                                                    \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s: got %#llx want %#llx\n", __FILE__, __LINE__, #got, g_, w_);                          \
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

/*
 * The encoding order is load-bearing exactly as the ALU's group-1 order is: a
 * decoded ModRM field indexes this enum. Reordering it "tidily" would make the
 * interpreter read EBX where the guest said EDX, silently.
 */
static void test_register_order_is_the_encoding_order(void) {
  static const char *manual[] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
  int i;
  for (i = 0; i < 8; i++) {
    CHECK(strcmp(x86p_reg_name(i, 4), manual[i]) == 0);
  }
  CHECK_EQ_U(kX86pEax, 0);
  CHECK_EQ_U(kX86pEsp, 4);
  CHECK_EQ_U(kX86pEdi, 7);
  CHECK_EQ_U(kX86pRegCount, 8);
}

/*
 * THE ALIASING. Byte index 4 is AH -- the second byte of EAX -- not ESP.
 * Exhaustive over all eight byte indices, checking both that the write lands in
 * the right place and that it lands in NO other register.
 */
static void test_byte_registers_alias_high_bytes(void) {
  static const char *names[] = {"AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH"};
  int idx, other;
  for (idx = 0; idx < 8; idx++) {
    X86pCpu cpu;
    int target = idx < 4 ? idx : idx - 4;
    uint32_t shift = idx < 4 ? 0u : 8u;
    x86p_cpu_reset(&cpu);
    CHECK(strcmp(x86p_reg_name(idx, 1), names[idx]) == 0);

    x86p_reg_write(&cpu, idx, 1, 0xA5u);
    CHECK_EQ_U(cpu.reg[target], 0xA5u << shift);
    CHECK_EQ_U(x86p_reg_read(&cpu, idx, 1), 0xA5u);
    /* Nothing else moved. Index 4 writing ESP is the failure this catches. */
    for (other = 0; other < 8; other++) {
      if (other != target) {
        CHECK_EQ_U(cpu.reg[other], 0u);
      }
    }
  }
}

/* AH and AL are independent halves of the same register, which is the point of
   the aliasing and the thing a naive implementation collapses. */
static void test_al_and_ah_are_independent(void) {
  X86pCpu cpu;
  x86p_cpu_reset(&cpu);
  cpu.reg[kX86pEax] = 0x11223344u;
  CHECK_EQ_U(x86p_reg_read(&cpu, 0, 1), 0x44u); /* AL */
  CHECK_EQ_U(x86p_reg_read(&cpu, 4, 1), 0x33u); /* AH */
  x86p_reg_write(&cpu, 0, 1, 0xFFu);
  CHECK_EQ_U(cpu.reg[kX86pEax], 0x112233FFu);
  x86p_reg_write(&cpu, 4, 1, 0xEEu);
  CHECK_EQ_U(cpu.reg[kX86pEax], 0x1122EEFFu);
}

/* A partial write preserves everything outside it. The most natural mistake in
   an interpreter, and it looks right at every call site. */
static void test_partial_writes_preserve_the_rest(void) {
  X86pCpu cpu;
  int i;
  for (i = 0; i < 8; i++) {
    x86p_cpu_reset(&cpu);
    cpu.reg[i] = 0xDEADBEEFu;
    x86p_reg_write(&cpu, i, 2, 0x0000u);
    CHECK_EQ_U(cpu.reg[i], 0xDEAD0000u); /* top half untouched */
    x86p_reg_write(&cpu, i, 2, 0xFFFFu);
    CHECK_EQ_U(cpu.reg[i], 0xDEADFFFFu);
    /* A too-wide value is masked, not smuggled into the upper half. */
    x86p_reg_write(&cpu, i, 2, 0x12345678u);
    CHECK_EQ_U(cpu.reg[i], 0xDEAD5678u);
  }
  for (i = 0; i < 4; i++) {
    x86p_cpu_reset(&cpu);
    cpu.reg[i] = 0xDEADBEEFu;
    x86p_reg_write(&cpu, i, 1, 0x00u);
    CHECK_EQ_U(cpu.reg[i], 0xDEADBE00u);
    x86p_reg_write(&cpu, i + 4, 1, 0x00u); /* the high byte */
    CHECK_EQ_U(cpu.reg[i], 0xDEAD0000u);
  }
  /* Only a dword write replaces the whole thing. */
  x86p_cpu_reset(&cpu);
  cpu.reg[kX86pEbx] = 0xDEADBEEFu;
  x86p_reg_write(&cpu, kX86pEbx, 4, 0x11u);
  CHECK_EQ_U(cpu.reg[kX86pEbx], 0x11u);
}

/* ---- memory ------------------------------------------------------------- */

static uint8_t g_arena[256];

static X86pMem arena(void) {
  X86pMem m;
  m.host = g_arena;
  m.lo = 0x1000u;
  m.size = sizeof g_arena;
  return m;
}

static void test_little_endian_round_trip(void) {
  X86pMem m = arena();
  uint32_t v;
  memset(g_arena, 0, sizeof g_arena);

  CHECK(x86p_mem_write(&m, 0x1000u, 4, 0x11223344u));
  /* The byte order is asserted against the ARENA, not just round-tripped: a
     read and a write that are wrong the same way round-trip perfectly. */
  CHECK_EQ_U(g_arena[0], 0x44u);
  CHECK_EQ_U(g_arena[1], 0x33u);
  CHECK_EQ_U(g_arena[2], 0x22u);
  CHECK_EQ_U(g_arena[3], 0x11u);

  CHECK(x86p_mem_read(&m, 0x1000u, 4, &v));
  CHECK_EQ_U(v, 0x11223344u);
  CHECK(x86p_mem_read(&m, 0x1000u, 2, &v));
  CHECK_EQ_U(v, 0x3344u);
  CHECK(x86p_mem_read(&m, 0x1000u, 1, &v));
  CHECK_EQ_U(v, 0x44u);
  CHECK(x86p_mem_read(&m, 0x1002u, 2, &v));
  CHECK_EQ_U(v, 0x1122u);

  /* A narrow write touches only its own bytes. */
  CHECK(x86p_mem_write(&m, 0x1001u, 1, 0xFFu));
  CHECK(x86p_mem_read(&m, 0x1000u, 4, &v));
  CHECK_EQ_U(v, 0x1122FF44u);
}

/* Unaligned accesses are legal for the guest, so they must work rather than
   being quietly rounded to something that does not fault. */
static void test_unaligned_access(void) {
  X86pMem m = arena();
  uint32_t v;
  memset(g_arena, 0, sizeof g_arena);
  CHECK(x86p_mem_write(&m, 0x1001u, 4, 0xAABBCCDDu));
  CHECK_EQ_U(g_arena[1], 0xDDu);
  CHECK_EQ_U(g_arena[4], 0xAAu);
  CHECK(x86p_mem_read(&m, 0x1001u, 4, &v));
  CHECK_EQ_U(v, 0xAABBCCDDu);
  /* Bytes 3 and 4 are BB then AA, so the word is 0xAABB -- spelled out because
     getting this expectation wrong is as easy as getting the code wrong. */
  CHECK(x86p_mem_read(&m, 0x1003u, 2, &v));
  CHECK_EQ_U(v, 0xAABBu);
}

/*
 * THE NEGATIVE, and the reason it matters more than the positive: an access
 * outside the mapping must be REPORTED and must change nothing. A truncated or
 * wrapped access that appears to have worked is the failure an interpreter has
 * to be incapable of.
 */
static void test_out_of_range_is_refused(void) {
  X86pMem m = arena();
  uint32_t v = 0xA5A5A5A5u;
  int refused = 0;
  memset(g_arena, 0x5Au, sizeof g_arena);

  if (!x86p_mem_read(&m, 0x0FFFu, 1, &v)) {
    refused++; /* below the mapping */
  }
  if (!x86p_mem_read(&m, 0x1000u + sizeof g_arena, 1, &v)) {
    refused++; /* one past the end */
  }
  if (!x86p_mem_read(&m, 0x1000u + sizeof g_arena - 1u, 2, &v)) {
    refused++; /* starts inside, ENDS outside -- the off-by-one that reads a
                  neighbouring allocation and looks like data */
  }
  if (!x86p_mem_read(&m, 0x1000u + sizeof g_arena - 3u, 4, &v)) {
    refused++;
  }
  if (!x86p_mem_read(&m, 0xFFFFFFFFu, 4, &v)) {
    refused++; /* would wrap the address space */
  }
  if (!x86p_mem_read(&m, 0x1000u, 3, &v)) {
    refused++; /* not a real operand width */
  }
  {
    X86pMem nul = {NULL, 0, 0};
    if (!x86p_mem_read(&nul, 0, 4, &v)) {
      refused++;
    }
    if (!x86p_mem_write(&nul, 0, 4, 1u)) {
      refused++;
    }
  }
  if (!x86p_mem_read(&m, 0x1000u, 4, NULL)) {
    refused++;
  }
  CHECK_EQ_U(refused, 9);     /* the denominator: every refusal probed */
  CHECK_EQ_U(v, 0xA5A5A5A5u); /* and none of them wrote to *out */

  /* A refused WRITE leaves the arena alone -- checked, because "returned 0"
     and "did nothing" are different claims. */
  CHECK(!x86p_mem_write(&m, 0x1000u + sizeof g_arena - 1u, 4, 0u));
  CHECK_EQ_U(g_arena[sizeof g_arena - 1], 0x5Au);
}

/* The boundary itself: the last byte IS mapped, and a dword ending exactly at
   the end IS mapped. An off-by-one in the guard rejects real accesses, which is
   a fault the guest never took. */
static void test_the_last_byte_is_usable(void) {
  X86pMem m = arena();
  uint32_t v;
  CHECK(x86p_mem_ok(&m, 0x1000u, 1));
  CHECK(x86p_mem_ok(&m, 0x1000u + sizeof g_arena - 1u, 1));
  CHECK(x86p_mem_ok(&m, 0x1000u + sizeof g_arena - 4u, 4));
  CHECK(x86p_mem_write(&m, 0x1000u + sizeof g_arena - 4u, 4, 0xCAFEBABEu));
  CHECK(x86p_mem_read(&m, 0x1000u + sizeof g_arena - 4u, 4, &v));
  CHECK_EQ_U(v, 0xCAFEBABEu);
}

/* ---- stack -------------------------------------------------------------- */

static void test_push_and_pop(void) {
  X86pCpu cpu;
  X86pMem m = arena();
  uint32_t v;
  x86p_cpu_reset(&cpu);
  memset(g_arena, 0, sizeof g_arena);
  cpu.reg[kX86pEsp] = 0x1000u + sizeof g_arena;

  CHECK(x86p_push32(&cpu, &m, 0x12345678u));
  CHECK_EQ_U(cpu.reg[kX86pEsp], 0x1000u + sizeof g_arena - 4u);
  CHECK(x86p_push32(&cpu, &m, 0x9ABCDEF0u));
  CHECK_EQ_U(cpu.reg[kX86pEsp], 0x1000u + sizeof g_arena - 8u);
  CHECK(x86p_pop32(&cpu, &m, &v));
  CHECK_EQ_U(v, 0x9ABCDEF0u);
  CHECK(x86p_pop32(&cpu, &m, &v));
  CHECK_EQ_U(v, 0x12345678u);
  CHECK_EQ_U(cpu.reg[kX86pEsp], 0x1000u + sizeof g_arena);
}

/* A faulting push must not move ESP. Moving it past a value that was never
   stored corrupts every frame after it, while the fault report blames the
   address -- so the store is attempted first and ESP follows. */
static void test_faulting_push_leaves_esp_alone(void) {
  X86pCpu cpu;
  X86pMem m = arena();
  x86p_cpu_reset(&cpu);
  cpu.reg[kX86pEsp] = 0x1000u; /* pushing goes below the mapping */
  CHECK(!x86p_push32(&cpu, &m, 0xDEADBEEFu));
  CHECK_EQ_U(cpu.reg[kX86pEsp], 0x1000u);

  /* Likewise a faulting pop. */
  cpu.reg[kX86pEsp] = 0x1000u + sizeof g_arena;
  CHECK(!x86p_pop32(&cpu, &m, NULL));
  CHECK_EQ_U(cpu.reg[kX86pEsp], 0x1000u + sizeof g_arena);
}

int main(void) {
  RUN(test_register_order_is_the_encoding_order);
  RUN(test_byte_registers_alias_high_bytes);
  RUN(test_al_and_ah_are_independent);
  RUN(test_partial_writes_preserve_the_rest);
  RUN(test_little_endian_round_trip);
  RUN(test_unaligned_access);
  RUN(test_out_of_range_is_refused);
  RUN(test_the_last_byte_is_usable);
  RUN(test_push_and_pop);
  RUN(test_faulting_push_leaves_esp_alone);
  printf("%d check(s), %d failed, %d failing test(s)\n", g_checks, g_failed, g_test_failed);
  return g_test_failed ? 1 : 0;
}
