/*
 * test_exec -- real guest programs, run.
 *
 * The other suites check pieces against hardware. This one checks that the
 * pieces COMPOSE: decode feeding operand resolution feeding the ALU feeding the
 * flag model feeding a conditional branch. Every one of those can be
 * individually correct while the interpreter still does not run a loop.
 *
 * THE PROGRAMS ARE REAL MACHINE CODE, produced by assembling the listed source
 * with `clang -m32` -- the assembler is the authority on encoding, not me with
 * an opcode table. The bytes are committed rather than assembled at build time,
 * because requiring a 32-bit toolchain to run the tests would be a
 * prerequisite this framework does not otherwise have; the assembly source is
 * kept beside each one so a reader can regenerate it.
 *
 * And a committed constant is not trusted on sight: every program asserts what
 * its bytes DECODE to before executing them, so a mistyped byte fails as a
 * decode mismatch naming the instruction, rather than as a wrong number forty
 * lines later.
 *
 * HLT is the stop marker. It has no semantics in this build, so it comes back
 * as a named Unsupported -- which is exactly the property being relied on: the
 * runner stops at the first instruction the engine cannot execute, and says
 * which.
 */
#include "exec.h"

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
static uint8_t g_arena[ARENA_SIZE];

static X86pMem arena(void) {
  X86pMem m;
  m.host = g_arena;
  m.lo = CODE_BASE;
  m.size = ARENA_SIZE;
  return m;
}

static void load(const uint8_t *code, size_t n) {
  memset(g_arena, 0, sizeof g_arena);
  memcpy(g_arena, code, n);
}

/*
 * Assert what the bytes decode to, before running them. A committed byte array
 * is a measured constant, and a measured constant that ships is diffed by code
 * rather than trusted -- here against the mnemonic sequence the assembler
 * produced, which is what the comment above each program shows.
 */
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
 * Run until something stops. Returns the status that stopped it and fills the
 * report; `steps` counts instructions executed, which is itself evidence -- a
 * loop that exits immediately and a loop that runs produce the same registers
 * surprisingly often.
 */
static X86pStepStatus run(X86pCpu *cpu, const X86pMem *m, int max_steps, int *steps, X86pStepReport *last) {
  X86pStepReport rep;
  int n = 0;
  for (;;) {
    X86pStepStatus st = x86p_step(cpu, m, &rep);
    if (st != kX86pStepOk) {
      if (last) {
        *last = rep;
      }
      *steps = n;
      return st;
    }
    if (++n >= max_steps) {
      if (last) {
        *last = rep;
      }
      *steps = n;
      /* Ok is returned, but the step count says it never finished -- the
         caller checks that, so a runaway loop cannot look like success. */
      return kX86pStepOk;
    }
  }
}

/* ------------------------------------------------------------------------- */

/*
 *     mov  $10, %ecx
 *     xor  %eax, %eax
 * 1:  add  %ecx, %eax
 *     dec  %ecx
 *     jnz  1b
 *     hlt
 */
static void test_program_sum_loop(void) {
  static const uint8_t code[] = {0xb9, 0x0a, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x01, 0xc8, 0x49, 0x75, 0xfb, 0xf4};
  static const char *const mn[] = {"MOV", "XOR", "ADD", "DEC", "JNZ", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  int steps = 0;
  X86pStepStatus st;

  expect_disassembly(code, sizeof code, mn, 6);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  cpu.reg[kX86pEsp] = CODE_BASE + ARENA_SIZE - 0x100u;

  st = run(&cpu, &m, 1000, &steps, &rep);
  /* HLT is a PROTECTION FAULT, not a gap: a ring-3 process may not execute it,
     and that is its defined outcome rather than something this build has yet
     to implement. Using it as the terminator is stable for the same reason --
     unlike "unsupported", it cannot stop being true when someone implements
     another instruction. */
  CHECK_EQ_U(st, kX86pStepProtectionFault);
  CHECK(strcmp(rep.mnemonic, "HLT") == 0);
  CHECK_EQ_U(cpu.eip, CODE_BASE + 12u); /* and EIP still points at it */

  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEax, 4), 55u); /* 10+9+...+1 */
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEcx, 4), 0u);
  /* 2 setup + 10 iterations x 3 = 32. Asserted because "EAX is 55" is also
     true of a loop that never ran and an EAX that was set directly. */
  CHECK_EQ_U(steps, 32);
}

/*
 *     mov  $7, %eax        ; push two arguments, call, clean up
 *     push %eax
 *     mov  $35, %eax
 *     push %eax
 *     call add_two
 *     add  $8, %esp
 *     hlt
 * add_two:
 *     push %ebp
 *     mov  %esp, %ebp
 *     mov  8(%ebp), %eax
 *     add  12(%ebp), %eax
 *     leave
 *     ret
 */
static void test_program_call_and_frame(void) {
  static const uint8_t code[] = {0xb8, 0x07, 0x00, 0x00, 0x00, 0x50, 0xb8, 0x23, 0x00, 0x00, 0x00,
                                 0x50, 0xe8, 0x04, 0x00, 0x00, 0x00, 0x83, 0xc4, 0x08, 0xf4, 0x55,
                                 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x03, 0x45, 0x0c, 0xc9, 0xc3};
  static const char *const mn[] = {"MOV", "PUSH", "MOV", "PUSH", "CALL", "ADD", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  int steps = 0;
  uint32_t esp0 = CODE_BASE + ARENA_SIZE - 0x100u;
  X86pStepStatus st;

  expect_disassembly(code, sizeof code, mn, 7);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  cpu.reg[kX86pEsp] = esp0;

  st = run(&cpu, &m, 1000, &steps, &rep);
  CHECK_EQ_U(st, kX86pStepProtectionFault);
  CHECK(strcmp(rep.mnemonic, "HLT") == 0);
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEax, 4), 42u); /* 35 + 7 */
  /* THE STACK IS BALANCED. A call that returns the right value having leaked
     four bytes of stack is the failure that surfaces a thousand frames later
     as unrelated corruption, so it is checked here rather than inferred. */
  CHECK_EQ_U(cpu.reg[kX86pEsp], esp0);
  CHECK_EQ_U(steps, 12);
}

/*
 *     mov  $0x2000, %ebx
 *     movl $0xdeadbeef, (%ebx)
 *     movzbl (%ebx), %eax
 *     movsbl (%ebx), %edx
 *     lea  4(%ebx,%ecx,2), %esi
 *     movw $0x1234, 8(%ebx)
 *     movzwl 8(%ebx), %edi
 *     hlt
 */
static void test_program_memory_and_extension(void) {
  static const uint8_t code[] = {0xbb, 0x00, 0x20, 0x00, 0x00, 0xc7, 0x03, 0xef, 0xbe, 0xad, 0xde,
                                 0x0f, 0xb6, 0x03, 0x0f, 0xbe, 0x13, 0x8d, 0x74, 0x4b, 0x04, 0x66,
                                 0xc7, 0x43, 0x08, 0x34, 0x12, 0x0f, 0xb7, 0x7b, 0x08, 0xf4};
  static const char *const mn[] = {"MOV", "MOV", "MOVZX", "MOVSX", "LEA", "MOV", "MOVZX", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  int steps = 0;
  X86pStepStatus st;

  expect_disassembly(code, sizeof code, mn, 8);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  cpu.reg[kX86pEsp] = CODE_BASE + ARENA_SIZE - 0x100u;
  cpu.reg[kX86pEcx] = 3u;

  st = run(&cpu, &m, 100, &steps, &rep);
  CHECK_EQ_U(st, kX86pStepProtectionFault);
  CHECK(strcmp(rep.mnemonic, "HLT") == 0);

  /* The dword really landed in guest memory, little-endian. */
  CHECK_EQ_U(g_arena[0x2000u - CODE_BASE], 0xefu);
  CHECK_EQ_U(g_arena[0x2003u - CODE_BASE], 0xdeu);
  /* MOVZX takes 0xEF to 0x000000EF; MOVSX takes it to 0xFFFFFFEF. Two
     instructions apart in the encoding and opposite in effect. */
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEax, 4), 0x000000EFu);
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEdx, 4), 0xFFFFFFEFu);
  /* LEA computes the ADDRESS: 0x2000 + 3*2 + 4. It must not read there. */
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEsi, 4), 0x2000u + 6u + 4u);
  /* A 16-bit store wrote two bytes, and only two. */
  CHECK_EQ_U(g_arena[0x2008u - CODE_BASE], 0x34u);
  CHECK_EQ_U(g_arena[0x2009u - CODE_BASE], 0x12u);
  CHECK_EQ_U(g_arena[0x200Au - CODE_BASE], 0x00u);
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEdi, 4), 0x1234u);
  CHECK_EQ_U(steps, 7);
}

/*
 *     mov  $100, %eax
 *     xor  %edx, %edx
 *     mov  $7, %ecx
 *     div  %ecx
 *     cmp  $14, %eax
 *     sete %bl
 *     mov  $0, %esi
 *     cmovz %eax, %esi
 *     hlt
 */
static void test_program_divide_setcc_cmovcc(void) {
  static const uint8_t code[] = {0xb8, 0x64, 0x00, 0x00, 0x00, 0x31, 0xd2, 0xb9, 0x07, 0x00,
                                 0x00, 0x00, 0xf7, 0xf1, 0x83, 0xf8, 0x0e, 0x0f, 0x94, 0xc3,
                                 0xbe, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x44, 0xf0, 0xf4};
  static const char *const mn[] = {"MOV", "XOR", "MOV", "DIV", "CMP", "SETZ", "MOV", "CMOVZ", "HLT"};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  int steps = 0;
  X86pStepStatus st;

  expect_disassembly(code, sizeof code, mn, 9);
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  cpu.reg[kX86pEsp] = CODE_BASE + ARENA_SIZE - 0x100u;

  st = run(&cpu, &m, 100, &steps, &rep);
  CHECK_EQ_U(st, kX86pStepProtectionFault);
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEax, 4), 14u); /* 100 / 7 */
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEdx, 4), 2u);  /* remainder */
  CHECK_EQ_U(x86p_reg_read(&cpu, 3, 1), 1u);         /* BL, from SETZ */
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEsi, 4), 14u); /* the CMOV took */
  CHECK_EQ_U(steps, 8);
}

/* ---- the failure paths, which are the point of naming them ---- */

/* A divide by zero is delivered as a status with nothing written, because the
   guest has to receive it. */
static void test_divide_error_is_delivered(void) {
  /* xor %edx,%edx ; xor %ecx,%ecx ; div %ecx */
  static const uint8_t code[] = {0x31, 0xd2, 0x31, 0xc9, 0xf7, 0xf1};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  int steps = 0;
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  cpu.reg[kX86pEax] = 100u;

  CHECK_EQ_U(run(&cpu, &m, 100, &steps, &rep), kX86pStepDivideError);
  CHECK(strcmp(rep.mnemonic, "DIV") == 0);
  CHECK_EQ_U(cpu.eip, CODE_BASE + 4u);                /* points AT the DIV */
  CHECK_EQ_U(x86p_reg_read(&cpu, kX86pEax, 4), 100u); /* nothing written */
}

/* Touching unmapped memory is a named fault carrying the address, and EIP
   stays on the instruction so a caller can map and retry. */
static void test_memory_fault_is_named(void) {
  /* mov $0xf0000000, %ebx ; mov (%ebx), %eax */
  static const uint8_t code[] = {0xbb, 0x00, 0x00, 0x00, 0xf0, 0x8b, 0x03};
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  int steps = 0;
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(run(&cpu, &m, 100, &steps, &rep), kX86pStepMemoryFault);
  CHECK_EQ_U(rep.fault_addr, 0xf0000000u);
  CHECK_EQ_U(cpu.eip, CODE_BASE + 5u);
  CHECK(strcmp(rep.mnemonic, "MOV") == 0);
}

/* An instruction with no semantics in this build is REPORTED BY NAME and does
   not advance EIP. Silence here would make "the interpreter ran it" and "the
   interpreter skipped it" the same run. */
static void test_unsupported_is_named_and_does_not_advance(void) {
  /*
   * RCPPS. This was FLD, then FSIN, and each stopped testing anything the day
   * its subject acquired semantics -- FSIN now runs on the host's own x87
   * unit, which is exact rather than an approximation, so the reasoning that
   * made it a good choice stopped applying and the test failed and asked to be
   * updated, exactly as its comment said it would.
   *
   * RCPPS is the stable choice. Its result comes from a hardware table to
   * about twelve mantissa bits; there is no exact way to compute it here and
   * `1.0f / x` would be wrong below the sixth digit, so simd.c refuses it on
   * purpose. It is unmodelled by DECISION rather than by not having got to it
   * yet, which is what a test of the refusal path needs.
   */
  static const uint8_t code[] = {0x0f, 0x53, 0xc0}; /* RCPPS XMM0, XMM0 */
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  load(code, sizeof code);
  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;

  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepUnsupported);
  CHECK(strcmp(rep.mnemonic, "RCPPS") == 0);
  CHECK_EQ_U(rep.length, 3u); /* it DECODED; only the semantics are refused */
  CHECK_EQ_U(rep.op, kX86pInsnSimd);
  CHECK_EQ_U(cpu.eip, CODE_BASE);
}

/* EIP outside mapped memory is a different fact from bytes that do not decode,
   and the two are told apart. */
static void test_fetch_fault_and_decode_failure_differ(void) {
  X86pCpu cpu;
  X86pMem m = arena();
  X86pStepReport rep;
  /* FF /7 is not an instruction: opcode FF's group has no entry for reg 7.
     Chosen after `f0 f0 f0 f0` turned out to decode -- with the rest of the
     arena readable it becomes LOCK-prefixed `ADD [EAX], AL`, which then failed
     as a MEMORY fault rather than a decode failure. A negative fixture that
     depends on what follows it is not a negative fixture. */
  static const uint8_t junk[] = {0xff, 0xff};

  load(junk, sizeof junk);
  x86p_cpu_reset(&cpu);
  cpu.eip = 0xF0000000u;
  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepFetchFault);
  CHECK_EQ_U(rep.fault_addr, 0xF0000000u);
  CHECK(strcmp(rep.mnemonic, "?") == 0); /* nothing was read, so nothing named */

  cpu.eip = CODE_BASE;
  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepDecodeFailed);
  CHECK_EQ_U(cpu.eip, CODE_BASE);

  /* Every status is nameable, including one outside the enum. */
  {
    int i, named = 0;
    for (i = 0; i < (int)kX86pStepStatusCount; i++) {
      CHECK(strcmp(x86p_step_status_name((X86pStepStatus)i), "unknown") != 0);
      named++;
    }
    CHECK_EQ_U(named, (int)kX86pStepStatusCount);
    CHECK(strcmp(x86p_step_status_name((X86pStepStatus)99), "unknown") == 0);
  }
}

/* An instruction ending exactly at the end of a mapping must execute. Fetching
   a fixed 15-byte window would fault on code the guest runs happily -- a fault
   the guest never took, which is worse than one it did. */
static void test_instruction_at_the_very_end_of_memory(void) {
  X86pCpu cpu;
  X86pMem m;
  X86pStepReport rep;
  memset(g_arena, 0, sizeof g_arena);
  m.host = g_arena;
  m.lo = CODE_BASE;
  m.size = 3; /* exactly one three-byte instruction is mapped */
  g_arena[0] = 0x83;
  g_arena[1] = 0xc4;
  g_arena[2] = 0x08; /* add $8, %esp */

  x86p_cpu_reset(&cpu);
  cpu.eip = CODE_BASE;
  cpu.reg[kX86pEsp] = 0x100u;
  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepOk);
  CHECK_EQ_U(cpu.reg[kX86pEsp], 0x108u);
  CHECK_EQ_U(cpu.eip, CODE_BASE + 3u);
  /* And the next fetch, now genuinely past the end, is a fetch fault. */
  CHECK_EQ_U(x86p_step(&cpu, &m, &rep), kX86pStepFetchFault);
}

int main(void) {
  RUN(test_program_sum_loop);
  RUN(test_program_call_and_frame);
  RUN(test_program_memory_and_extension);
  RUN(test_program_divide_setcc_cmovcc);
  RUN(test_divide_error_is_delivered);
  RUN(test_memory_fault_is_named);
  RUN(test_unsupported_is_named_and_does_not_advance);
  RUN(test_fetch_fault_and_decode_failure_differ);
  RUN(test_instruction_at_the_very_end_of_memory);
  printf("%d check(s), %d failed, %d failing test(s)\n", g_checks, g_failed, g_test_failed);
  return g_test_failed ? 1 : 0;
}
