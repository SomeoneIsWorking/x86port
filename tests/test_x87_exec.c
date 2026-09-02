/*
 * test_x87_exec -- x87 instructions executed as instructions.
 *
 * test_x87.c checks the FPU model through its C interface. This one runs real
 * machine code through the interpreter, which is the only way to catch the
 * layer between them: whether FSTP pops, whether FCOMPP pops twice, whether
 * FXCH names the position the encoding names, whether a memory operand's width
 * selects the right format. Those are decoding and routing questions, and a
 * model that is perfectly correct in isolation still gets them wrong.
 *
 * Every program below is assembled machine code, not bytes written by hand,
 * and each asserts its own disassembly before it runs -- a committed byte array
 * is a measured constant, and a measured constant is diffed by code rather than
 * trusted.
 */
#include "exec.h"

#include <math.h>
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

/* ------------------------------------------------------------------------- */

#define CODE_BASE 0x1000u
#define ARENA_SIZE 0x4000u
/* Guest addresses the programs use for their operands. 0x2000 and 0x2008 are
   inputs, 0x2010 onwards is where results are stored. */
#define IN_A 0x2000u
#define IN_B 0x2008u
#define OUT_0 0x2010u
#define OUT_1 0x2014u

static uint8_t g_arena[ARENA_SIZE];

static X86pMem arena(void) {
  X86pMem m;
  m.host = g_arena;
  m.lo = CODE_BASE;
  m.size = ARENA_SIZE;
  return m;
}

static uint8_t *at(uint32_t guest) {
  return g_arena + (guest - CODE_BASE);
}

static void load(const uint8_t *code, size_t n) {
  memset(g_arena, 0, sizeof g_arena);
  memcpy(g_arena, code, n);
}

static void put_f64(uint32_t guest, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  memcpy(at(guest), &bits, sizeof bits);
}

static double get_f64(uint32_t guest) {
  uint64_t bits;
  double v;
  memcpy(&bits, at(guest), sizeof bits);
  memcpy(&v, &bits, sizeof v);
  return v;
}

static void put_f32(uint32_t guest, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  memcpy(at(guest), &bits, sizeof bits);
}

static float get_f32(uint32_t guest) {
  uint32_t bits;
  float v;
  memcpy(&bits, at(guest), sizeof bits);
  memcpy(&v, &bits, sizeof v);
  return v;
}

static void put_i32(uint32_t guest, int32_t v) {
  memcpy(at(guest), &v, sizeof v);
}

static int32_t get_i32(uint32_t guest) {
  int32_t v;
  memcpy(&v, at(guest), sizeof v);
  return v;
}

static void expect_disassembly(const uint8_t *code, size_t n, const char *const *mnemonics, int count) {
  size_t off = 0;
  int i = 0;
  for (i = 0; i < count && off < n; i++) {
    X86pInsn insn;
    memset(&insn, 0, sizeof insn);
    if (!x86p_decode(code + off, n - off, &insn)) {
      g_checks++;
      g_failed++;
      printf("    FAIL byte %zu did not decode; expected %s\n", off, mnemonics[i]);
      return;
    }
    g_checks++;
    if (strcmp(insn.mnemonic, mnemonics[i]) != 0) {
      g_failed++;
      printf("    FAIL byte %zu decodes as %s, expected %s\n", off, insn.mnemonic, mnemonics[i]);
    }
    off += insn.length;
  }
  /* The count is the denominator: a program whose bytes ran out early would
     otherwise "pass" every comparison it managed to make. */
  CHECK_EQ_U(i, count);
}

/*
 * Run until HLT, which decodes and has no semantics, so it comes back as a
 * NAMED Unsupported and serves as the stop marker. `steps` is evidence in its
 * own right: a program that stopped at the first instruction and one that ran
 * to the end leave the same memory when the memory was already right.
 */
static X86pStepStatus run(X86pCpu *cpu, const X86pMem *m, int *steps) {
  X86pStepReport rep;
  int n = 0;
  for (;;) {
    X86pStepStatus st = x86p_step(cpu, m, &rep);
    if (st != kX86pStepOk) {
      if (steps) {
        *steps = n;
      }
      if (st == kX86pStepUnsupported && rep.mnemonic && strcmp(rep.mnemonic, "HLT") == 0) {
        return kX86pStepOk; /* the stop marker, not a failure */
      }
      printf("    stopped: %s at %#x (%s)\n", x86p_step_status_name(st), rep.eip, rep.mnemonic ? rep.mnemonic : "?");
      return st;
    }
    n++;
    if (n > 64) {
      printf("    FAIL runaway\n");
      g_failed++;
      return kX86pStepUnsupported;
    }
  }
}

/* ---- the programs ------------------------------------------------------- */

/*
 * fldl 0x2000 / fldl 0x2008 / fmulp %st,%st(1) / fstpl 0x2010 / hlt
 *
 * The one every guest does thousands of times. It also pins the POP COUNT: two
 * loads, an FMULP that consumes one, and an FSTP that consumes the other must
 * leave the stack EMPTY. A model where the P is cosmetic finishes with values
 * still on it and drifts from there.
 */
static void test_load_multiply_store(void) {
  static const uint8_t code[] = {0xdd, 0x05, 0x00, 0x20, 0x00, 0x00, 0xdd, 0x05, 0x08, 0x20, 0x00,
                                 0x00, 0xde, 0xc9, 0xdd, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FLD", "FLD", "FMULP", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;

  expect_disassembly(code, sizeof code, asm_, 5);
  load(code, sizeof code);
  put_f64(IN_A, 6.25);
  put_f64(IN_B, -4.0);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 4);
  CHECK(get_f64(OUT_0) == -25.0);
  CHECK_EQ_U(x86p_x87_depth(&cpu.x87), 0); /* the pops actually happened */
  CHECK_EQ_U(cpu.x87.status & X86P_X87_SF, 0);
}

/*
 * fildl 0x2000 / fchs / fistpl 0x2010 / hlt
 *
 * The integer forms read and write two's complement, not floats of the same
 * width -- reading 4 bytes as a float here gives a denormal near zero and the
 * store then writes 0, which looks plausible and is wrong.
 */
static void test_integer_load_and_store(void) {
  static const uint8_t code[] = {
      0xdb, 0x05, 0x00, 0x20, 0x00, 0x00, 0xd9, 0xe0, 0xdb, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FILD", "FCHS", "FISTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;

  expect_disassembly(code, sizeof code, asm_, 4);
  load(code, sizeof code);
  put_i32(IN_A, -1234567);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 3);
  CHECK_EQ_U((uint32_t)get_i32(OUT_0), (uint32_t)1234567);
  CHECK_EQ_U(x86p_x87_depth(&cpu.x87), 0);
}

/*
 * fldl / fldl / fcompp / fnstsw %ax / hlt
 *
 * FCOMPP pops TWICE and FNSTSW lands the result in AX, where the guest then
 * branches on it. This is the path every floating-point `if` in the game takes,
 * and it crosses three modules: the FPU, the register file, and the decoder's
 * reading of an implicit operand.
 */
static void test_compare_reaches_ax(void) {
  static const uint8_t code[] = {
      0xdd, 0x05, 0x00, 0x20, 0x00, 0x00, 0xdd, 0x05, 0x08, 0x20, 0x00, 0x00, 0xde, 0xd9, 0xdf, 0xe0, 0xf4};
  static const char *const asm_[] = {"FLD", "FLD", "FCOMPP", "FNSTSW", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;
  const uint16_t cc = X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
  struct {
    double a, b;
    uint16_t want;
  } cases[] = {
      /* ST(0) is the SECOND load, so the comparison is b against a. */
      {1.0, 2.0, 0},           /* 2 > 1  -> all clear */
      {2.0, 1.0, X86P_X87_C0}, /* 1 < 2  -> C0 */
      {3.0, 3.0, X86P_X87_C3}, /* equal  -> C3 */
      {1.0, (double)NAN, cc},  /* unordered -> all three */
  };
  const int n = (int)(sizeof cases / sizeof cases[0]);
  int i;

  expect_disassembly(code, sizeof code, asm_, 5);
  for (i = 0; i < n; i++) {
    load(code, sizeof code);
    put_f64(IN_A, cases[i].a);
    put_f64(IN_B, cases[i].b);
    x86p_cpu_reset(&cpu);
    cpu.eip = CODE_BASE;
    CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
    CHECK_EQ_U(steps, 4);
    CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEax, 2) & cc, cases[i].want);
    CHECK_EQ_U(x86p_x87_depth(&cpu.x87), 0); /* popped TWICE */
  }
}

/*
 * fldz / fld1 / fxch %st(1) / fstps 0x2010 / fstps 0x2014 / hlt
 *
 * FXCH is the instruction that catches an ST(i)-is-register-i model: after the
 * two pushes TOP is 6, so ST(1) is physical register 7, and a model indexing
 * reg[1] swaps two registers that hold nothing. Stores go out in stack order,
 * so the exchange is visible in which value lands where.
 */
static void test_exchange_uses_positions(void) {
  static const uint8_t code[] = {
      0xd9, 0xee, 0xd9, 0xe8, 0xd9, 0xc9, 0xd9, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xd9, 0x1d, 0x14, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FLDZ", "FLD1", "FXCH", "FSTP", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;

  expect_disassembly(code, sizeof code, asm_, 6);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 5);
  /* Pushed 0 then 1, so ST(0)=1 and ST(1)=0; after FXCH, ST(0)=0. */
  CHECK(get_f32(OUT_0) == 0.0f);
  CHECK(get_f32(OUT_1) == 1.0f);
  CHECK_EQ_U(x86p_x87_depth(&cpu.x87), 0);
  /* And TOP is back where it started, which is a separate fact from the
     stack being empty -- one wrong pop leaves both looking fine. */
  CHECK_EQ_U(cpu.x87.top, 0);
}

/*
 * fldz / fld1 / fld %st(1) / three stores / hlt
 *
 * FLD ST(i) READS BEFORE IT PUSHES. The push moves TOP, which renumbers every
 * position -- so a model that pushes first and then reads ST(i) copies its
 * neighbour instead. Invisible to every memory-operand FLD, which is why a
 * mutation swapping the order passed until this program existed.
 */
static void test_load_from_stack_reads_before_pushing(void) {
  static const uint8_t code[] = {0xd9, 0xee, 0xd9, 0xe8, 0xd9, 0xc1, 0xd9, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xd9,
                                 0x1d, 0x14, 0x20, 0x00, 0x00, 0xd9, 0x1d, 0x18, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FLDZ", "FLD1", "FLD", "FSTP", "FSTP", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;

  expect_disassembly(code, sizeof code, asm_, 7);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 6);
  /* Stack before the FLD is ST(0)=1, ST(1)=0, so it pushes 0. Reading after
     the push would see the old ST(0) and push 1 instead. */
  CHECK(get_f32(OUT_0) == 0.0f);
  CHECK(get_f32(OUT_1) == 1.0f);
  CHECK(get_f32(0x2018u) == 0.0f);
  CHECK_EQ_U(cpu.x87.top, 0);
}

/*
 * fldz / fld1 / flds 0x2000 / fxch %st(2) / three stores / hlt
 *
 * FXCH AT A POSITION OTHER THAN 1. The %st(1) case above cannot tell a model
 * that reads the encoded index from one that always exchanges with 1 -- a
 * mutation proved it, by hardcoding the index and passing. This one names
 * ST(2), so the two differ.
 */
static void test_exchange_names_the_encoded_position(void) {
  static const uint8_t code[] = {0xd9, 0xee, 0xd9, 0xe8, 0xd9, 0x05, 0x00, 0x20, 0x00, 0x00, 0xd9,
                                 0xca, 0xd9, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xd9, 0x1d, 0x14, 0x20,
                                 0x00, 0x00, 0xd9, 0x1d, 0x18, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FLDZ", "FLD1", "FLD", "FXCH", "FSTP", "FSTP", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;

  expect_disassembly(code, sizeof code, asm_, 8);
  load(code, sizeof code);
  put_f32(IN_A, 9.0f);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 7);
  /* Pushed 0, 1, 9 -- so ST(0)=9, ST(1)=1, ST(2)=0. FXCH ST(2) swaps 9 and 0,
     and the stores then read the stack downwards. */
  CHECK(get_f32(OUT_0) == 0.0f);
  CHECK(get_f32(OUT_1) == 1.0f);
  CHECK(get_f32(0x2018u) == 9.0f);
  CHECK_EQ_U(cpu.x87.top, 0);
}

/*
 * flds 0x2004 / fiadds 0x2000 / fimull 0x2008 / fstpl 0x2010 / hlt
 *
 * The FI ARITHMETIC forms, whose memory operand is a two's-complement integer
 * rather than a float of the same width. Untested until a mutation that read
 * them as floats passed everything -- FILD and FISTP go down a different path
 * and do not cover this one. The word operand also pins the width: read as a
 * float, 2 bytes is not a format at all.
 */
static void test_integer_memory_arithmetic(void) {
  static const uint8_t code[] = {0xd9, 0x05, 0x04, 0x20, 0x00, 0x00, 0xde, 0x05, 0x00, 0x20, 0x00, 0x00, 0xda,
                                 0x0d, 0x08, 0x20, 0x00, 0x00, 0xdd, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FLD", "FIADD", "FIMUL", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;

  expect_disassembly(code, sizeof code, asm_, 5);
  load(code, sizeof code);
  put_f32(IN_A + 4, 2.5f);
  at(IN_A)[0] = 0xF9; /* -7 as a 16-bit integer, which FIADD reads */
  at(IN_A)[1] = 0xFF;
  put_i32(IN_B, 3);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 4);
  CHECK(get_f64(OUT_0) == (2.5 + -7.0) * 3.0);
  CHECK_EQ_U(x86p_x87_depth(&cpu.x87), 0);
}

/*
 * fldcw 0x2000 / flds 0x2004 / fadds 0x2008 / fstpl 0x2010 / hlt
 *
 * The guest's control word must actually reach the arithmetic. This is the
 * failure with no symptom: ignoring FLDCW leaves every result plausible and
 * slightly wrong, and nothing crashes.
 */
static void test_control_word_reaches_the_arithmetic(void) {
  static const uint8_t code[] = {0xd9, 0x2d, 0x00, 0x20, 0x00, 0x00, 0xd9, 0x05, 0x04, 0x20, 0x00, 0x00, 0xd8,
                                 0x05, 0x08, 0x20, 0x00, 0x00, 0xdd, 0x1d, 0x10, 0x20, 0x00, 0x00, 0xf4};
  static const char *const asm_[] = {"FLDCW", "FLD", "FADD", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  int steps = 0;
  /* 1.0 + 2^-30: needs more than 24 significand bits to survive. */
  const float a = 1.0f;
  const float b = ldexpf(1.0f, -30);

  expect_disassembly(code, sizeof code, asm_, 5);

  /* Extended precision: the sum keeps the small addend. */
  load(code, sizeof code);
  at(IN_A)[0] = (uint8_t)(X86P_X87_CW_INIT & 0xFF);
  at(IN_A)[1] = (uint8_t)(X86P_X87_CW_INIT >> 8);
  put_f32(IN_A + 4, a);
  put_f32(IN_B, b);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
  CHECK_EQ_U(steps, 4);
  CHECK_EQ_U(cpu.x87.control, X86P_X87_CW_INIT);
  CHECK(get_f64(OUT_0) != 1.0);

  /* Single precision: the same instructions, and the addend is gone. If this
     matched the line above, FLDCW would be doing nothing. */
  {
    const uint16_t cw = (uint16_t)((X86P_X87_CW_INIT & ~X86P_X87_PC_MASK) | X86P_X87_PC_SINGLE);
    load(code, sizeof code);
    at(IN_A)[0] = (uint8_t)(cw & 0xFF);
    at(IN_A)[1] = (uint8_t)(cw >> 8);
    put_f32(IN_A + 4, a);
    put_f32(IN_B, b);
    x86p_cpu_reset(&cpu);
    cpu.eip = CODE_BASE;
    CHECK_EQ_U(run(&cpu, &m, &steps), kX86pStepOk);
    CHECK_EQ_U(cpu.x87.control, cw);
    CHECK(get_f64(OUT_0) == 1.0);
  }
}

/*
 * A store to unmapped memory FAULTS AND DOES NOT POP. The instruction did not
 * complete, so the stack must be exactly as it was -- popping anyway loses the
 * value the guest was trying to save and desynchronises everything after it.
 */
static void test_faulting_store_does_not_pop(void) {
  /* fldl 0x2000 / fstpl 0xf0000000 / hlt */
  static const uint8_t code[] = {0xdd, 0x05, 0x00, 0x20, 0x00, 0x00, 0xdd, 0x1d, 0x00, 0x00, 0x00, 0xf0, 0xf4};
  static const char *const asm_[] = {"FLD", "FSTP", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;

  expect_disassembly(code, sizeof code, asm_, 3);
  load(code, sizeof code);
  put_f64(IN_A, 7.5);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepOk); /* the FLD */
  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepMemoryFault);
  CHECK_EQ_U(rep.fault_addr, 0xf0000000u);
  CHECK_EQ_U(cpu.eip, CODE_BASE + 6u); /* EIP still AT the faulting store */
  CHECK_EQ_U(x86p_x87_depth(&cpu.x87), 1);
  {
    long double v = 0.0L;
    CHECK(x86p_x87_get(&cpu.x87, 0, &v) && v == 7.5L);
  }
}

/*
 * The refusal is still a refusal. FSIN decodes and has no semantics here --
 * the transcendentals are not approximated -- and it must come back NAMED
 * rather than silently doing nothing, which would leave the stack wrong with
 * no report.
 */
static void test_unmodelled_x87_is_named(void) {
  /* RCPPS, not FSIN: FSIN now runs on the host's own x87 unit. See the same
     case in test_exec.c for why a refused-by-decision instruction is the right
     subject for a test of the refusal path. */
  static const uint8_t code[] = {0x0f, 0x53, 0xc0, 0xf4}; /* RCPPS XMM0,XMM0 / HLT */
  static const char *const asm_[] = {"RCPPS", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;

  expect_disassembly(code, sizeof code, asm_, 2);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepUnsupported);
  CHECK(strcmp(rep.mnemonic, "RCPPS") == 0);
  CHECK_EQ_U(rep.length, 3u); /* it DECODED; only the semantics are refused */
  CHECK_EQ_U(cpu.eip, CODE_BASE);
}

int main(void) {
  RUN(test_load_multiply_store);
  RUN(test_integer_load_and_store);
  RUN(test_compare_reaches_ax);
  RUN(test_exchange_uses_positions);
  RUN(test_load_from_stack_reads_before_pushing);
  RUN(test_exchange_names_the_encoded_position);
  RUN(test_integer_memory_arithmetic);
  RUN(test_control_word_reaches_the_arithmetic);
  RUN(test_faulting_store_does_not_pop);
  RUN(test_unmodelled_x87_is_named);
  printf("%d check(s), %d failed, %d failing test(s)\n", g_checks, g_failed, g_test_failed);
  return g_test_failed ? 1 : 0;
}
