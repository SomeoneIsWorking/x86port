/*
 * test_jit_x64 -- does translated code do what the interpreter does?
 *
 * This is the test the whole backend exists to pass, and its shape matters more
 * than its size. The interpreter is this framework's declared authority on what
 * an x86-32 instruction means (S043), verified against silicon by test_flags and
 * test_alu. So the JIT is not checked against hand-written expected values --
 * which would only prove it agrees with whoever wrote the test -- but against
 * THE OTHER ENGINE, on the same program, from the same starting state, with the
 * ENTIRE guest machine compared afterwards.
 *
 * WHY THE WHOLE MACHINE AND NOT THE DESTINATION REGISTER. A backend that
 * clobbers a scratch register, forgets that CMP writes no result, or leaves
 * carry_in stale where an INC or DEC will read it produces exactly the right
 * destination value and a corrupted machine. Comparing eight registers, EIP,
 * the raw lazy-flag tuple AND all six derived flags is what turns those into
 * failures instead of into a bug found three months later in a game. (The one
 * relaxation: raw carry_in is a dead cache except under kind Inc/Dec, so it is
 * compared only there -- see same_state. The derived CF check stays strict.)
 *
 * PROGRAMS ARE GENERATED, NOT HAND-PICKED. Hand-written cases test the
 * instructions their author thought of, in the order they thought of, and every
 * interesting bug here lives in an interaction: ADC reading a carry that the
 * PREVIOUS instruction set, a destination that is also a source, a register
 * used as both. A deterministic pseudo-random program generator over the
 * supported set reaches those; a fixed seed keeps a failure reproducible.
 */
#include "alu.h"
#include "cpu.h"
#include "decode.h"
#include "exec.h"
#include "jit_x64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* Denominators. Printed at the end so "0 failures" can never be read as
   success over a run that compared nothing. */
static unsigned long g_programs;
static unsigned long g_guest_insns;
static unsigned long g_state_compares;
static unsigned long g_branch_blocks;
static unsigned long g_refused;
static unsigned long g_self_modified;
static unsigned long g_helper_calls;

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 4096u

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
static uint8_t *g_guest;
static uint8_t g_before[GUEST_SIZE];
static uint8_t g_after_interp[GUEST_SIZE];

static void guest_mem_init(void) {
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
  if (GUEST_SIZE > (size_t)page) {
    printf("FATAL: the guest mapping is larger than a page and cannot end on one\n");
    exit(1);
  }
  g_guest = base + page - GUEST_SIZE;
}

static X86pMem guest_mem(void) {
  X86pMem m;
  m.host = g_guest;
  m.lo = GUEST_BASE;
  m.size = GUEST_SIZE;
  return m;
}

/* ---- host code memory ---------------------------------------------------- */

/*
 * RWX in one mapping, which production must never do -- jit-common's
 * code_memory exists precisely because W^X, Apple's MAP_JIT and Android's
 * SELinux policy make that unavailable. It is acceptable HERE because the
 * translator takes a caller-owned buffer and never publishes it itself, so this
 * test exercises the same translate() a real caller would; wiring it to a
 * JcCodeRegion changes the allocation and nothing else.
 */
static void *code_alloc(size_t n) {
  void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}

/* ---- guest program generation -------------------------------------------- */

typedef struct Prog {
  uint32_t len;
  uint32_t insns;
} Prog;

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/*
 * Emit one random supported guest instruction.
 *
 * The immediates are drawn from boundary values as well as from the generator,
 * because carry and overflow live at 0, 1, 0x7FFFFFFF, 0x80000000 and
 * 0xFFFFFFFF, and a uniformly random 32-bit value essentially never lands on
 * one. A sweep that never crosses a boundary cannot see a carry defect.
 */
static uint32_t interesting(uint64_t r) {
  static const uint32_t v[] = {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x0000FFFFu};
  if ((r & 3u) != 0u) {
    return v[(r >> 8) % (sizeof v / sizeof v[0])];
  }
  return (uint32_t)(r >> 16);
}

static uint32_t emit_guest_insn(uint8_t *p, uint64_t r) {
  unsigned pick = (unsigned)(r % 99u);
  unsigned dst = (unsigned)((r >> 3) & 7u);
  unsigned src = (unsigned)((r >> 6) & 7u);
  unsigned aluop = (unsigned)((r >> 9) & 7u);
  /*
   * EBX, EBP or ESI: the three seeded with in-range addresses. ESI points two
   * bytes below the END of the mapping, and its displacement is kept to 0..3 so
   * the access STARTS inside and runs off -- the only shape that separates a
   * check against `size - w` from one against `size`.
   */
  unsigned mb = (unsigned)((r >> 30) & 3u);
  unsigned membase = (mb == 3u) ? 6u : (mb == 1u) ? 5u : 3u;
  uint8_t memdisp = (membase == 6u) ? (uint8_t)((r >> 22) & 3u) : (uint8_t)((r >> 22) & 0x3Fu);

  switch (pick) {
  case 0: /* MOV r32, imm32 -- B8+r */
    p[0] = (uint8_t)(0xB8u + dst);
    put_u32(p + 1, interesting(r));
    return 5;
  case 1: /* MOV r/m32, r32 -- 89 /r, mod=11 */
    p[0] = 0x89u;
    p[1] = (uint8_t)(0xC0u | (src << 3) | dst);
    return 2;
  case 2: /* <alu> r/m32, r32 -- (op*8+1) /r, mod=11. Includes ADC and SBB,
             which read the carry the PREVIOUS instruction left. */
    p[0] = (uint8_t)((aluop << 3) | 1u);
    p[1] = (uint8_t)(0xC0u | (src << 3) | dst);
    return 2;
  case 3: /* <alu> r/m32, imm32 -- 81 /op, mod=11 */
    p[0] = 0x81u;
    p[1] = (uint8_t)(0xC0u | (aluop << 3) | dst);
    put_u32(p + 2, interesting(r));
    return 6;
  case 4: /* TEST r/m32, r32 -- 85 /r. Writes flags and NO result. */
    p[0] = 0x85u;
    p[1] = (uint8_t)(0xC0u | (src << 3) | dst);
    return 2;
  case 5: /* Jcc rel8 -- 70+cc. Reads the flags the PREVIOUS instruction set,
             which is the interaction a per-instruction test cannot reach. */
    p[0] = (uint8_t)(0x70u | ((r >> 12) & 0xFu));
    p[1] = (uint8_t)((r >> 20) & 0x1Fu);
    return 2;
  case 9: /* MOV r32, [base+disp8] -- 8B /r, mod=01 */
    p[0] = 0x8Bu;
    p[1] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 10: /* MOV [base+disp8], r32 -- 89 /r, mod=01 */
    p[0] = 0x89u;
    p[1] = (uint8_t)(0x40u | (src << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 11: /* <alu> r32, [base+disp8] -- (op*8+3) /r, mod=01 */
    p[0] = (uint8_t)((aluop << 3) | 3u);
    p[1] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 12: /* <alu> [base+disp8], r32 -- (op*8+1) /r, mod=01 */
    p[0] = (uint8_t)((aluop << 3) | 1u);
    p[1] = (uint8_t)(0x40u | (src << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 13: { /* MOV r32, [base+index*scale+disp8] -- SIB, mod=01 */
    /* Half the time the index is EDI, which is small and in range, so the
       SCALE decides the address instead of the access faulting regardless. */
    unsigned use_edi = (unsigned)((r >> 27) & 1u);
    unsigned sibindex = use_edi ? 7u : (unsigned)((r >> 28) & 7u);
    unsigned sibbase = use_edi ? 3u : membase;
    p[0] = 0x8Bu;
    p[1] = (uint8_t)(0x40u | (dst << 3) | 4u); /* rm=100: SIB follows */
    p[2] = (uint8_t)((((r >> 26) & 3u) << 6) | (sibindex << 3) | sibbase);
    p[3] = memdisp;
    return 4;
  }
  case 14: /* PUSH r32 -- 50+r */
    p[0] = (uint8_t)(0x50u + dst);
    return 1;
  case 15: /* POP r32 -- 58+r */
    p[0] = (uint8_t)(0x58u + dst);
    return 1;
  case 16: /* PUSH imm32 -- 68 id */
    p[0] = 0x68u;
    put_u32(p + 1, interesting(r));
    return 5;
  case 17: /* PUSH [base+disp8] -- FF /6, mod=01 */
    p[0] = 0xFFu;
    p[1] = (uint8_t)(0x40u | (6u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 18: /* POP [base+disp8] -- 8F /0, mod=01 */
    p[0] = 0x8Fu;
    p[1] = (uint8_t)(0x40u | membase);
    p[2] = memdisp;
    return 3;
  case 19: /* LEA r32, [base+disp8] -- 8D /r, mod=01 */
    p[0] = 0x8Du;
    p[1] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 20: { /* LEA r32, [base+index*scale+disp8] -- SIB, mod=01. An LEA on a
                WILD base is legal and common: it is arithmetic, not an access,
                so the wide-open value must produce a result rather than a
                fault. */
    unsigned leabase = (unsigned)((r >> 27) & 7u);
    p[0] = 0x8Du;
    p[1] = (uint8_t)(0x40u | (dst << 3) | 4u);
    p[2] = (uint8_t)((((r >> 26) & 3u) << 6) | (((r >> 28) & 7u) << 3) | leabase);
    p[3] = memdisp;
    return 4;
  }
  case 21: /* CALL rel32 -- E8 cd. Ends the block by transferring control, and
              the displacement is deliberately wild: nothing here executes the
              target, and folding the wrong one in is what this compares. */
    p[0] = 0xE8u;
    put_u32(p + 1, interesting(r));
    return 5;
  case 22: /* RET -- C3 */
    p[0] = 0xC3u;
    return 1;
  case 23: /* RET imm16 -- C2 iw, which also releases the caller's arguments */
    p[0] = 0xC2u;
    p[1] = (uint8_t)((r >> 22) & 0xFFu);
    p[2] = 0u;
    return 3;
  case 24: /* JMP r32 -- FF /4, mod=11 */
    p[0] = 0xFFu;
    p[1] = (uint8_t)(0xE0u | dst);
    return 2;
  case 25: /* CALL r32 -- FF /2, mod=11 */
    p[0] = 0xFFu;
    p[1] = (uint8_t)(0xD0u | dst);
    return 2;
  case 26: /* JMP [base+disp8] -- FF /4, mod=01 */
    p[0] = 0xFFu;
    p[1] = (uint8_t)(0x40u | (4u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 27: /* CALL [base+disp8] -- FF /2, mod=01 */
    p[0] = 0xFFu;
    p[1] = (uint8_t)(0x40u | (2u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 28: /* PUSH imm8 -- 6A ib, SIGN-EXTENDED to a dword */
    p[0] = 0x6Au;
    p[1] = (uint8_t)((r >> 22) & 0xFFu);
    return 2;
  case 29: /* MOV r8, r/m8 -- 8A /r, mod=11. Byte-register indices 4..7 are the
              HIGH bytes AH/CH/DH/BH, so the generator uses all eight and the
              partial-write rule is exercised on both halves. */
    p[0] = 0x8Au;
    p[1] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 2;
  case 30: /* MOV r/m8, imm8 -- C6 /0, mod=11 */
    p[0] = 0xC6u;
    p[1] = (uint8_t)(0xC0u | dst);
    p[2] = (uint8_t)((r >> 22) & 0xFFu);
    return 3;
  case 31: /* MOV r16, r/m16 -- 66 8B /r, mod=11 */
    p[0] = 0x66u;
    p[1] = 0x8Bu;
    p[2] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 3;
  case 32: /* MOV r8, [base+disp8] -- 8A /r, mod=01 */
    p[0] = 0x8Au;
    p[1] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 33: /* MOV [base+disp8], r8 -- 88 /r, mod=01 */
    p[0] = 0x88u;
    p[1] = (uint8_t)(0x40u | (src << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 34: /* MOV r16, [base+disp8] -- 66 8B /r, mod=01. The two-byte access at
              a base sitting on the last legal dword straddles the top of the
              mapping, which is the case a width-blind bounds check gets wrong
              in the OTHER direction: it refuses a legal narrow access. */
    p[0] = 0x66u;
    p[1] = 0x8Bu;
    p[2] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 35: /* <alu> r/m8, r8 -- (op*8+0) /r, mod=11. Byte-register indices run
              0..7, so the HIGH byte registers take part in the arithmetic and
              in the partial write-back. */
    p[0] = (uint8_t)(aluop << 3);
    p[1] = (uint8_t)(0xC0u | (src << 3) | dst);
    return 2;
  case 36: /* <alu> r8, r/m8 -- (op*8+2) /r, mod=11 */
    p[0] = (uint8_t)((aluop << 3) | 2u);
    p[1] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 2;
  case 37: /* <alu> r/m8, imm8 -- 80 /op, mod=11 */
    p[0] = 0x80u;
    p[1] = (uint8_t)(0xC0u | (aluop << 3) | dst);
    p[2] = (uint8_t)((r >> 22) & 0xFFu);
    return 3;
  case 38: /* <alu> r/m16, r16 -- 66 (op*8+1) /r, mod=11 */
    p[0] = 0x66u;
    p[1] = (uint8_t)((aluop << 3) | 1u);
    p[2] = (uint8_t)(0xC0u | (src << 3) | dst);
    return 3;
  case 39: /* <alu> r/m16, imm16 -- 66 81 /op, mod=11 */
    p[0] = 0x66u;
    p[1] = 0x81u;
    p[2] = (uint8_t)(0xC0u | (aluop << 3) | dst);
    p[3] = (uint8_t)((r >> 22) & 0xFFu);
    p[4] = (uint8_t)((r >> 30) & 0xFFu);
    return 5;
  case 40: /* <alu> r8, [base+disp8] -- (op*8+2) /r, mod=01 */
    p[0] = (uint8_t)((aluop << 3) | 2u);
    p[1] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 41: /* INC r32 -- 40+r. PRESERVES CF, which is what carry_in exists for,
              so the generator places these among the ALU ops rather than in a
              corner of their own. */
    p[0] = (uint8_t)(0x40u + dst);
    return 1;
  case 42: /* DEC r32 -- 48+r */
    p[0] = (uint8_t)(0x48u + dst);
    return 1;
  case 43: /* NEG r/m32 -- F7 /3, mod=11 */
    p[0] = 0xF7u;
    p[1] = (uint8_t)(0xD8u | dst);
    return 2;
  case 44: /* NOT r/m32 -- F7 /2, mod=11. Writes NO flags, so the instruction
              AFTER it must still see the one before it as its predecessor. */
    p[0] = 0xF7u;
    p[1] = (uint8_t)(0xD0u | dst);
    return 2;
  case 45: /* INC r/m8 -- FE /0, mod=11 */
    p[0] = 0xFEu;
    p[1] = (uint8_t)(0xC0u | dst);
    return 2;
  case 46: /* DEC [base+disp8] -- FF /1, mod=01 */
    p[0] = 0xFFu;
    p[1] = (uint8_t)(0x40u | (1u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 47: /* SHL r/m32, imm8 -- C1 /4. The count deliberately RANGES over 0
              (writes no flags at all), 1..31, and >= 32 (masked to five bits),
              which are three different rules rather than three values. */
    p[0] = 0xC1u;
    p[1] = (uint8_t)(0xE0u | dst);
    p[2] = (uint8_t)((r >> 20) & 0x3Fu);
    return 3;
  case 48: /* SHR r/m32, imm8 -- C1 /5 */
    p[0] = 0xC1u;
    p[1] = (uint8_t)(0xE8u | dst);
    p[2] = (uint8_t)((r >> 20) & 0x3Fu);
    return 3;
  case 49: /* SAR r/m32, imm8 -- C1 /7. Replicates the sign rather than
              shifting in zeroes, so a host SHR here would agree on every
              non-negative input and differ on every negative one. */
    p[0] = 0xC1u;
    p[1] = (uint8_t)(0xF8u | dst);
    p[2] = (uint8_t)((r >> 20) & 0x3Fu);
    return 3;
  case 50: /* SHL r/m32, CL -- D3 /4. The count is a REGISTER, one byte wide
              while the destination is four: loading it at the destination's
              width would pass the whole of ECX as the count. */
    p[0] = 0xD3u;
    p[1] = (uint8_t)(0xE0u | dst);
    return 2;
  case 51: /* SAR r/m32, CL -- D3 /7 */
    p[0] = 0xD3u;
    p[1] = (uint8_t)(0xF8u | dst);
    return 2;
  case 52: /* SHL r/m16, imm8 -- 66 C1 /4. A count of 16..31 is not masked away
              at this width: it is in range for the mask and past the operand
              width, which is the case that returns zero. */
    p[0] = 0x66u;
    p[1] = 0xC1u;
    p[2] = (uint8_t)(0xE0u | dst);
    p[3] = (uint8_t)((r >> 20) & 0x1Fu);
    return 4;
  case 53: /* SHR r/m8, imm8 -- C0 /5 */
    p[0] = 0xC0u;
    p[1] = (uint8_t)(0xE8u | dst);
    p[2] = (uint8_t)((r >> 20) & 0x1Fu);
    return 3;
  /* Additional coverage candidates. Shapes without an emitter end the block
     as a named refusal; emitted shapes remain part of whole-state differential
     coverage. */
  case 54: /* PUSHFD -- 9C. Materialises the WHOLE EFLAGS word from the lazy
              (kind, a, b, r) form, which no emitted instruction does. */
    p[0] = 0x9Cu;
    return 1;
  case 55: /* POPFD -- 9D. And back again, which is how a wrong flag becomes a
              wrong VALUE that later comparisons can see. */
    p[0] = 0x9Du;
    return 1;
  case 56: /* CDQ -- 99 */
    p[0] = 0x99u;
    return 1;
  case 57: /* IMUL r32, r/m32 -- 0F AF /r, mod=11 */
    p[0] = 0x0Fu;
    p[1] = 0xAFu;
    p[2] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 3;
  case 58: /* MOVZX r32, r/m8 -- 0F B6 /r, mod=11 */
    p[0] = 0x0Fu;
    p[1] = 0xB6u;
    p[2] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 3;
  case 59: /* MOVSX r32, r/m8 -- 0F BE /r, mod=11 */
    p[0] = 0x0Fu;
    p[1] = 0xBEu;
    p[2] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 3;
  case 60: /* SETcc r/m8 -- 0F 90+cc, mod=11 */
    p[0] = 0x0Fu;
    p[1] = (uint8_t)(0x90u | ((r >> 12) & 0xFu));
    p[2] = (uint8_t)(0xC0u | dst);
    return 3;
  case 61: /* FLD dword [base+disp8] -- D9 /0. Pushes, so it moves TOP. */
    p[0] = 0xD9u;
    p[1] = (uint8_t)(0x40u | (0u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 62: /* FSTP dword [base+disp8] -- D9 /3. Pops. Pairing it with FLD keeps
              the stack from simply filling up and reporting overflow forever,
              which would test one state and call it eight. */
    p[0] = 0xD9u;
    p[1] = (uint8_t)(0x40u | (3u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 63: /* FADD dword [base+disp8] -- D8 /0 */
    p[0] = 0xD8u;
    p[1] = (uint8_t)(0x40u | (0u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 64: /* FMUL dword [base+disp8] -- D8 /1 */
    p[0] = 0xD8u;
    p[1] = (uint8_t)(0x40u | (1u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 65: /* FILD dword [base+disp8] -- DB /0. Reads the bytes as an INTEGER,
              so a model that treated the FI forms as floats of the same width
              differs on every value. */
    p[0] = 0xDBu;
    p[1] = (uint8_t)(0x40u | (0u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 66: /* FLD ST(i) -- D9 C0+i */
    p[0] = 0xD9u;
    p[1] = (uint8_t)(0xC0u | ((r >> 12) & 7u));
    return 2;
  case 67: /* FADDP ST(i), ST(0) -- DE C0+i. The pop suffix is part of the
              operation, not a detail of it. */
    p[0] = 0xDEu;
    p[1] = (uint8_t)(0xC0u | ((r >> 12) & 7u));
    return 2;
  case 68: /* FXCH ST(i) -- D9 C8+i */
    p[0] = 0xD9u;
    p[1] = (uint8_t)(0xC8u | ((r >> 12) & 7u));
    return 2;
  case 69: /* FCHS -- D9 E0 */
    p[0] = 0xD9u;
    p[1] = 0xE0u;
    return 2;
  case 70: /* FSTP dword, but from the OTHER encoding of the stack: FSTP ST(i),
              DD D8+i. */
    p[0] = 0xDDu;
    p[1] = (uint8_t)(0xD8u | ((r >> 12) & 7u));
    return 2;
  case 71: /* MOV r32, FS:[base+disp8] -- 64 8B /r, mod=01.
              The one addressing mode with a segment BASE to add. A backend
              that ignored the prefix would read the right shape at the wrong
              address, and with fs_base seeded to zero the two would agree. */
    p[0] = 0x64u;
    p[1] = 0x8Bu;
    p[2] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 72: /* MOV FS:[base+disp8], r32 -- 64 89 /r, mod=01 */
    p[0] = 0x64u;
    p[1] = 0x89u;
    p[2] = (uint8_t)(0x40u | (src << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 73: /* LEA r32, FS:[base+disp8] -- 8D with the prefix present.
              LEA computes the OFFSET and must NOT add the segment base; this
              is the case that separates the two address computations. */
    p[0] = 0x64u;
    p[1] = 0x8Du;
    p[2] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 74: /* MOV r/m16, Sreg -- 8C /r, mod=11. ES, FS or GS; never CS, which
              is not encodable as a destination. */
    p[0] = 0x8Cu;
    p[1] = (uint8_t)(0xC0u | ((((r >> 12) % 3u) == 0u ? 0u : (((r >> 12) % 3u) == 1u ? 4u : 5u)) << 3) | dst);
    return 2;
  case 75: /* MOV Sreg, r/m16 -- 8E /r, mod=11 */
    p[0] = 0x8Eu;
    p[1] = (uint8_t)(0xC0u | ((((r >> 12) % 3u) == 0u ? 0u : (((r >> 12) % 3u) == 1u ? 4u : 5u)) << 3) | src);
    return 2;
  case 76: /* MOVSD -- A5, no repeat. Advances ESI and EDI by four. */
    p[0] = 0xA5u;
    return 1;
  case 77: /* REP STOSD -- F3 AB. ECX is seeded from `interesting`, so this is
              sometimes zero (does nothing) and sometimes very large (faults
              partway, which both engines must agree about). */
    p[0] = 0xF3u;
    p[1] = 0xABu;
    return 2;
  case 78: /* STD or CLD -- FD / FC. The stride's SIGN, which nothing else in
              the generator sets. */
    p[0] = ((r >> 12) & 1u) ? 0xFDu : 0xFCu;
    return 1;
  case 79: /* PADDW mm0+i, mm0+j -- 0F FD /r, mod=11. MMX writes through the
              x87 register file, so a block that got the aliasing wrong shows
              up in the x87 comparison rather than in a register nobody looks
              at. */
    p[0] = 0x0Fu;
    p[1] = 0xFDu;
    p[2] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
    return 3;
  case 80: /* PXOR mm, mm */
    p[0] = 0x0Fu;
    p[1] = 0xEFu;
    p[2] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
    return 3;
  case 81: /* MOVQ mm, [base+disp8] -- 0F 6F /r, mod=01. Eight bytes from
              guest memory, which is a width no integer instruction reads. */
    p[0] = 0x0Fu;
    p[1] = 0x6Fu;
    p[2] = (uint8_t)(0x40u | ((dst & 7u) << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 82: /* MOVAPS xmm, [base+disp8] -- 0F 28 /r. Sixteen bytes, and the
              only operand width in the generator that can run off the end of
              the mapping from an address that is itself inside it. */
    p[0] = 0x0Fu;
    p[1] = 0x28u;
    p[2] = (uint8_t)(0x40u | ((dst & 7u) << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 83: /* MULPS xmm, xmm -- 0F 59 /r, mod=11 */
    p[0] = 0x0Fu;
    p[1] = 0x59u;
    p[2] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
    return 3;
  case 84: /* EMMS -- 0F 77. Empties the register file, so an x87 instruction
              after it sees a fresh stack; the aliasing is only visible if
              this and the FLD cases appear in the same block. */
    p[0] = 0x0Fu;
    p[1] = 0x77u;
    return 2;
  case 85: /* MOVZX r32, r/m16 -- 0F B7 /r, mod=11 */
    p[0] = 0x0Fu;
    p[1] = 0xB7u;
    p[2] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 3;
  case 86: /* MOVSX r32, r/m16 -- 0F BF /r, mod=11 */
    p[0] = 0x0Fu;
    p[1] = 0xBFu;
    p[2] = (uint8_t)(0xC0u | (dst << 3) | src);
    return 3;
  case 87: /* MOVZX r32, byte [base+disp8] -- 0F B6 /r, mod=01. The narrow
              memory load the register form cannot exercise. */
    p[0] = 0x0Fu;
    p[1] = 0xB6u;
    p[2] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 88: /* MOVSX r32, word [base+disp8] -- 0F BF /r, mod=01. Sign extend AND
              a two-byte access that can straddle the mapping edge. */
    p[0] = 0x0Fu;
    p[1] = 0xBFu;
    p[2] = (uint8_t)(0x40u | (dst << 3) | membase);
    p[3] = memdisp;
    return 4;
  case 89: /* FLD qword [base+disp8] -- DD /0. The 64-bit source the dword FLD
              (case 61) cannot reach: a different escape opcode, an 8-byte
              access, and the double->extended widen. */
    p[0] = 0xDDu;
    p[1] = (uint8_t)(0x40u | (0u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 90: /* FSUB dword [base+disp8] -- D8 /4. Non-commutative, reverse=0. */
    p[0] = 0xD8u;
    p[1] = (uint8_t)(0x40u | (4u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 91: /* FDIVR dword [base+disp8] -- D8 /7. reverse=1: ST(0) = m32 / ST(0),
              so a backend that dropped the swap differs on every ordered pair. */
    p[0] = 0xD8u;
    p[1] = (uint8_t)(0x40u | (7u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 92: /* FADD ST(0), ST(i) -- D8 C0+i. The one-operand short register form,
              destination ST(0), no pop. */
    p[0] = 0xD8u;
    p[1] = (uint8_t)(0xC0u | ((r >> 12) & 7u));
    return 2;
  case 93: /* FDIVRP ST(i), ST(0) -- DE F0+i. Two-operand, reverse AND pop:
              ST(i) = ST(0) / ST(i), then pop. */
    p[0] = 0xDEu;
    p[1] = (uint8_t)(0xF0u | ((r >> 12) & 7u));
    return 2;
  case 94: /* FST ST(i) -- DD D0+i. Register store WITHOUT the pop, so a backend
              that always popped leaves TOP one short. */
    p[0] = 0xDDu;
    p[1] = (uint8_t)(0xD0u | ((r >> 12) & 7u));
    return 2;
  case 95: /* FSUBR ST(i), ST(0) -- DC E8+i. Two-operand, destination ST(i), no
              pop, reverse=1: the DC/DE reg encodings put FSUBR where FSUB's
              digit would predict, so a backend that read the digit rather than
              the decoder's x87_reverse gets the operand order backwards. */
    p[0] = 0xDCu;
    p[1] = (uint8_t)(0xE8u | ((r >> 12) & 7u));
    return 2;
  case 96: /* FST dword [base+disp8] -- D9 /2. Store WITHOUT the pop: TOP is
              unchanged, and the narrowed bytes land in guest memory (the
              full-image memcmp catches a wrong RC or a wrong width). */
    p[0] = 0xD9u;
    p[1] = (uint8_t)(0x40u | (2u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 97: /* FST qword [base+disp8] -- DD /2. Eight-byte narrowing store. */
    p[0] = 0xDDu;
    p[1] = (uint8_t)(0x40u | (2u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 98: /* FSTP qword [base+disp8] -- DD /3. Narrowing store and a pop. */
    p[0] = 0xDDu;
    p[1] = (uint8_t)(0x40u | (3u << 3) | membase);
    p[2] = memdisp;
    return 3;
  case 7: /* Jcc rel32 -- 0F 80+cc. A different encoding of the same branch;
             a backend that read the displacement at the wrong width would pass
             the rel8 cases and fail only here. */
    p[0] = 0x0Fu;
    p[1] = (uint8_t)(0x80u | ((r >> 12) & 0xFu));
    put_u32(p + 2, (uint32_t)(int32_t)(int8_t)((r >> 20) & 0xFFu));
    return 6;
  case 8: /* JMP rel8 -- EB */
    p[0] = 0xEBu;
    p[1] = (uint8_t)((r >> 20) & 0x1Fu);
    return 2;
  default: /* ALU again, so branches stay a minority and blocks have length */
    p[0] = (uint8_t)((aluop << 3) | 1u);
    p[1] = (uint8_t)(0xC0u | (src << 3) | dst);
    return 2;
  }
}

static Prog generate(uint64_t *rng, uint32_t want_insns) {
  Prog pr;
  uint32_t off = 0;
  uint32_t i;
  pr.insns = 0;
  for (i = 0; i < want_insns; i++) {
    *rng = *rng * 6364136223846793005ull + 1442695040888963407ull;
    if (off + 16u >= GUEST_SIZE) {
      break;
    }
    off += emit_guest_insn(g_guest + off, *rng);
    pr.insns++;
  }
  pr.len = off;
  return pr;
}

/* ---- state comparison ---------------------------------------------------- */

/*
 * Where generated memory operands point.
 *
 * Well clear of the program itself: a store landing in the instruction stream
 * would be self-modifying code, and the interpreter re-reads instructions from
 * this same buffer while the JIT is running code translated BEFORE the write.
 * The two would then diverge legitimately, and the suite would report a
 * translator bug that is really a test-harness bug.
 */
#define DATA_OFF 0x400u

static void seed_cpu(X86pCpu *cpu, uint64_t r) {
  int i;
  x86p_cpu_reset(cpu);
  /*
   * A NON-ZERO FS base, and distinct selectors.
   *
   * With fs_base left at zero an FS-prefixed access and a plain one compute
   * the same address, so a backend that dropped the prefix would agree with
   * the interpreter everywhere. Small enough that a prefixed access still
   * lands inside the mapping most of the time and faults the rest, which are
   * both outcomes worth comparing.
   */
  cpu->fs_base = 0x40u;
  cpu->gs_base = 0x80u;
  for (i = 0; i < kX86pSegRegCount; i++) {
    cpu->seg[i] = (uint16_t)(0x0023u + 8u * (unsigned)i);
  }
  for (i = 0; i < kX86pRegCount; i++) {
    r = r * 6364136223846793005ull + 1442695040888963407ull;
    cpu->reg[i] = interesting(r);
  }
  /*
   * EBX and EBP are given in-range addresses so generated memory operands
   * mostly HIT. The other six keep their wild values, so operands based on them
   * mostly fault -- and both paths matter: a backend whose bounds check never
   * fires and one whose check always fires are equally broken, and a corpus
   * that only exercises one of them cannot tell.
   */
  cpu->reg[kX86pEbx] = GUEST_BASE + DATA_OFF;
  cpu->reg[kX86pEbp] = GUEST_BASE + DATA_OFF + 0x40u;
  /*
   * ESI sits just below the TOP of the mapping, so a four-byte access based on
   * it starts inside and runs off the end. That is the case a bounds check
   * written as `addr - lo > size` lets through while every in-the-middle access
   * still behaves, and a corpus that only walks the interior cannot see it:
   * measured -- comparing against `size` instead of `size - w` survived
   * mutation until this register existed.
   */
  cpu->reg[kX86pEsi] = GUEST_BASE + GUEST_SIZE - 4u;
  /*
   * EDI is a small INDEX rather than an address. With a wild index every scaled
   * operand faults whatever the scale is, so the scale itself is never
   * exercised -- measured: turning scale 4 into scale 2 survived mutation while
   * this register held a random value.
   */
  cpu->reg[kX86pEdi] = 3u;
  /*
   * A stack in the upper half of the mapping, so PUSH walks DOWN into unused
   * space rather than into the program it is executing. With a wild ESP every
   * push and pop would fault and the whole stack path would be exercised only
   * through its refusal.
   */
  cpu->reg[kX86pEsp] = GUEST_BASE + 0x800u;
  cpu->eip = GUEST_BASE;
}

/*
 * Compare every architectural thing both engines are supposed to agree on.
 *
 * The DERIVED flags are compared as well as the raw tuple. They are redundant
 * only if the derivation is right; comparing both means a backend that stores a
 * plausible tuple with the wrong kind still fails, and it fails naming the flag
 * rather than naming a struct field.
 */
static int same_state(const X86pCpu *a, const X86pCpu *b, const char *what) {
  int ok = 1;
  int i;
  g_state_compares++;
  for (i = 0; i < kX86pRegCount; i++) {
    if (a->reg[i] != b->reg[i]) {
      printf("    FAIL %s: %s interp=%08X jit=%08X\n", what, x86p_reg_name(i, 4), a->reg[i], b->reg[i]);
      ok = 0;
    }
  }
  {
    int k;
    for (k = 0; k < kX86pSegRegCount; k++) {
      if (a->seg[k] != b->seg[k]) {
        printf("    FAIL %s: seg[%d] interp=%04X jit=%04X\n", what, k, a->seg[k], b->seg[k]);
        ok = 0;
      }
    }
  }
  if (a->df != b->df) {
    printf("    FAIL %s: DF interp=%u jit=%u\n", what, a->df, b->df);
    ok = 0;
  }
  {
    int k;
    for (k = 0; k < 8; k++) {
      if (memcmp(a->xmm[k], b->xmm[k], 16) != 0) {
        printf("    FAIL %s: XMM%d differs\n", what, k);
        ok = 0;
      }
    }
  }
  if (a->mxcsr != b->mxcsr) {
    printf("    FAIL %s: MXCSR interp=%08X jit=%08X\n", what, a->mxcsr, b->mxcsr);
    ok = 0;
  }
  if (a->fs_base != b->fs_base || a->gs_base != b->gs_base) {
    printf("    FAIL %s: segment bases interp=(%08X %08X) jit=(%08X %08X)\n",
           what,
           a->fs_base,
           a->gs_base,
           b->fs_base,
           b->gs_base);
    ok = 0;
  }
  if (a->eip != b->eip) {
    printf("    FAIL %s: EIP interp=%08X jit=%08X\n", what, a->eip, b->eip);
    ok = 0;
  }
  /*
   * `carry_in` is a cache read only when kind is Inc/Dec (flags.c). Dead-flag-
   * store elimination in jit_x64.c can leave it stale behind the block's last
   * flag write when that write's predecessor tuple was elided -- unobservable,
   * since the derived FLAG(x86p_flag_cf, ...) check below stays strict. Match
   * cpu_compare.c: compare the raw carry_in only where it is live.
   */
  {
    const int carry_live = (a->flags.kind == kX86pFlagsInc || a->flags.kind == kX86pFlagsDec);
    if (a->flags.kind != b->flags.kind || a->flags.a != b->flags.a || a->flags.b != b->flags.b ||
        a->flags.r != b->flags.r || a->flags.w != b->flags.w ||
        (carry_live && a->flags.carry_in != b->flags.carry_in)) {
      printf("    FAIL %s: lazy flags interp=(k%u %08X %08X %08X w%u c%u) jit=(k%u %08X %08X %08X w%u c%u)\n",
             what,
             a->flags.kind,
             a->flags.a,
             a->flags.b,
             a->flags.r,
             a->flags.w,
             a->flags.carry_in,
             b->flags.kind,
             b->flags.a,
             b->flags.b,
             b->flags.r,
             b->flags.w,
             b->flags.carry_in);
      ok = 0;
    }
  }
#define FLAG(fn, name)                                                                                                 \
  if (fn(&a->flags) != fn(&b->flags)) {                                                                                \
    printf("    FAIL %s: %s interp=%d jit=%d\n", what, name, fn(&a->flags), fn(&b->flags));                            \
    ok = 0;                                                                                                            \
  }
  FLAG(x86p_flag_cf, "CF")
  FLAG(x86p_flag_pf, "PF")
  FLAG(x86p_flag_af, "AF")
  FLAG(x86p_flag_zf, "ZF")
  FLAG(x86p_flag_sf, "SF")
  FLAG(x86p_flag_of, "OF")
#undef FLAG
  /*
   * The x87 machine, compared REGISTER BY POSITION rather than as a memcmp of
   * the struct.
   *
   * It has to be compared at all: the generator emits floating-point
   * instructions, and every one of them is executed by a call into the
   * interpreter. If this function looked only at the integer registers, a
   * translated block could drop an FLD entirely -- or push it onto the wrong
   * physical register -- and pass, because nothing else in the machine would
   * move.
   *
   * By position because TOP is a rotation: two states with the same values in
   * ST(0)..ST(7) and different `top` are different machines, and two with the
   * same physical array and different `top` are also different machines. A
   * memcmp would catch both, but it would also compare the padding bytes of a
   * long double array, which are not architectural state.
   */
  {
    int k;
    if (a->x87.top != b->x87.top || a->x87.control != b->x87.control || a->x87.status != b->x87.status) {
      printf("    FAIL %s: x87 top/cw/sw interp=(%u %04X %04X) jit=(%u %04X %04X)\n",
             what,
             a->x87.top,
             a->x87.control,
             a->x87.status,
             b->x87.top,
             b->x87.control,
             b->x87.status);
      ok = 0;
    }
    for (k = 0; k < X86P_X87_REGS; k++) {
      long double va = 0.0L;
      long double vb = 0.0L;
      int ha = x86p_x87_get(&a->x87, k, &va);
      int hb = x86p_x87_get(&b->x87, k, &vb);
      /*
       * Compare the ARCHITECTURAL bytes, not sizeof(long double).
       *
       * On this host an 80-bit extended value occupies ten bytes inside a
       * sixteen-byte object, and the remaining six are padding that no
       * instruction defines. A memcmp over the whole object reported a
       * divergence between two values that printed identically, which is the
       * opposite of what a differential is for. Where `long double` is not
       * x87's format the whole object is significant, and x87.c is the
       * authority on which case this is.
       */
      const size_t significant = x86p_x87_precision_is_exact() ? 10u : sizeof va;
      if (ha != hb || (ha && memcmp(&va, &vb, significant) != 0)) {
        printf("    FAIL %s: ST(%d) interp=%s%.20Lg jit=%s%.20Lg\n",
               what,
               k,
               ha ? "" : "(empty)",
               va,
               hb ? "" : "(empty)",
               vb);
        ok = 0;
      }
    }
  }
  g_checks++;
  if (!ok) {
    g_failed++;
  }
  return ok;
}

/* Run the interpreter until it has executed `n` instructions or stopped. */
/* Returns the interpreter's own reason for stopping, which is what the JIT's
   exit code is checked against. Comparing only the register file lets a block
   that stops for the WRONG REASON pass: a RET that reports "unsupported" leaves
   identical state and still forces the caller back to the interpreter for an
   instruction the backend just executed. */
static X86pStepStatus run_interp(X86pCpu *cpu, const X86pMem *mem, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) {
    X86pStepStatus st = x86p_step(cpu, mem, NULL);
    if (st != kX86pStepOk) {
      return st;
    }
  }
  return kX86pStepOk;
}

/* The exit a block that ran `n` instructions must report, given how the
   interpreter stopped and whether the translation had a stopper. */
static int exit_agrees(X86pJitExit got, X86pStepStatus interp, const char *stopper) {
  if (interp == kX86pStepMemoryFault) {
    return got == kX86pJitExitMemoryFault;
  }
  if (stopper != NULL) {
    return got == kX86pJitExitUnsupported;
  }
  return got == kX86pJitExitBlockEnd;
}

/* ---- the differential ---------------------------------------------------- */

static void test_jit_matches_interpreter_on_generated_programs(void) {
  void *code = code_alloc(65536);
  uint64_t rng = 0x5EED1234ABCDull;
  int round;

  CHECK(code != NULL);
  if (!code) {
    return;
  }

  for (round = 0; round < 1500; round++) {
    X86pMem mem = guest_mem();
    X86pCpu ci;
    X86pCpu cj;
    X86pJitBlock blk;
    char reason[192];
    X86pJitStatus st;
    X86pStepStatus interp_st;
    X86pJitExit jit_exit;
    Prog pr;
    uint64_t seed;

    memset(g_guest, 0x90, GUEST_SIZE); /* NOP fill: a run that walks off the
                                              program hits defined instructions
                                              rather than random bytes */
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    seed = rng;
    pr = generate(&rng, 1u + (uint32_t)(rng % 24u));
    if (pr.insns == 0) {
      continue;
    }

    seed_cpu(&ci, seed);
    seed_cpu(&cj, seed);

    st = x86p_jit_translate(&mem, GUEST_BASE, code, 65536, &blk, reason, sizeof reason);
    if (st == kX86pJitUnsupportedAtEntry) {
      /* The generator produced a shape this backend refuses BY NAME -- a
         memory-form ADC, say. Refusing is the designed behaviour, not a
         defect, so the round is skipped and COUNTED: a run where the refusals
         quietly grew to swallow the corpus must be visible in the totals
         rather than showing as a clean pass over nothing. */
      g_refused++;
      continue;
    }
    g_checks++;
    if (st != kX86pJitOk) {
      g_failed++;
      printf("    FAIL round %d: translate -> %s (%s)\n", round, x86p_jit_status_name(st), reason);
      continue;
    }

    /* The JIT must have translated something. A zero-instruction block that
       reported Ok would make no progress and every counter would look fine. */
    CHECK(blk.insns > 0u);

    /* Counted by the independent walk below, and used by the carry-helper
       bound: a shift's flag kind is not knowable until it runs. */
    unsigned unknown_kind_insns = 0u;

    /*
     * guest_len must be exactly the bytes the translated instructions occupy,
     * computed here by an INDEPENDENT walk rather than taken from the block.
     *
     * This is the field range invalidation uses to decide whether a write to
     * guest memory stales a block. Nothing else in this suite reads it, so a
     * backend that set it from the branch TARGET instead of the fall-through
     * would pass every state comparison and leave the cache unable to
     * invalidate correctly -- found by mutation, which is why it is checked.
     */
    {
      uint32_t span = 0u;
      uint32_t k;
      int walked = 1;
      unknown_kind_insns = 0u;
      for (k = 0; k < blk.insns; k++) {
        uint8_t ib[X86P_MAX_INSN_LEN];
        X86pInsn di;
        uint32_t j;
        for (j = 0; j < (uint32_t)X86P_MAX_INSN_LEN; j++) {
          uint32_t byte;
          if (!x86p_mem_read(&mem, GUEST_BASE + span + j, 1, &byte)) {
            break;
          }
          ib[j] = (uint8_t)byte;
        }
        if (!x86p_decode(ib, X86P_MAX_INSN_LEN, &di)) {
          walked = 0;
          break;
        }
        span += di.length;
        if ((di.op == (uint8_t)kX86pInsnAlu && di.alu >= (uint8_t)kX86pAluShl && di.alu <= (uint8_t)kX86pAluSar) ||
            di.op == (uint8_t)kX86pInsnImul ||
            (di.op == (uint8_t)kX86pInsnString &&
             (di.str == (uint8_t)kX86pStringScas || di.str == (uint8_t)kX86pStringCmps))) {
          unknown_kind_insns++;
        }
      }
      g_checks++;
      if (!walked) {
        g_failed++;
        printf("    FAIL round %d: could not re-walk the block\n", round);
      } else if (span != blk.guest_len) {
        g_failed++;
        printf("    FAIL round %d: guest_len=%u but %u instruction(s) span %u byte(s)\n",
               round,
               blk.guest_len,
               blk.insns,
               span);
      }
    }

    /*
     * BOTH ENGINES MUST SEE THE SAME MEMORY. They share one guest buffer, so
     * running the interpreter first leaves ITS stores in place and the JIT then
     * reads values the guest program never wrote -- a divergence with no
     * translator bug behind it. Snapshot, run, keep the interpreter's memory,
     * rewind, run the JIT, and compare the two images: memory is architectural
     * state, so a store to the wrong address is a failure this must name rather
     * than a difference it happens not to look at.
     */
    memcpy(g_before, g_guest, GUEST_SIZE);
    interp_st = run_interp(&ci, &mem, blk.insns);
    memcpy(g_after_interp, g_guest, GUEST_SIZE);
    memcpy(g_guest, g_before, GUEST_SIZE);
    jit_exit = x86p_jit_enter(&blk, &cj);

    /*
     * SELF-MODIFYING CODE IS NOT A DIVERGENCE, AND THIS INSTRUMENT CANNOT JUDGE
     * IT.
     *
     * A generated store can land in the program itself -- the base registers
     * are seeded in range but any MOV can overwrite one, after which the
     * address is wherever it lands, and the mapping includes the code. The
     * interpreter re-reads each instruction from memory as it goes; the JIT
     * runs a block translated BEFORE the write. Both behaviours are correct,
     * and the guest is required to invalidate between them, which is what
     * test_jit_engine's invalidation test covers.
     *
     * So the round is dropped and COUNTED. A silent skip here could grow to
     * swallow the corpus while the suite still reported a clean run.
     */
    /*
     * The range checked is what the BLOCK covers, not what the generator
     * emitted. Blocks used to stop at the first instruction with no emitter,
     * so they never ran past the generated bytes; they now translate on
     * through the NOP fill that follows, and a store landing in that fill is
     * every bit as self-modifying. Checking only `pr.len` reported one such
     * round as a divergence -- correctly detecting that the two engines
     * disagreed, and wrongly blaming the translator.
     */
    {
      size_t covered = pr.len > blk.guest_len ? pr.len : (size_t)blk.guest_len;
      if (memcmp(g_after_interp, g_before, covered) != 0 || memcmp(g_guest, g_before, covered) != 0) {
        g_self_modified++;
        continue;
      }
    }
    g_checks++;
    if (!exit_agrees(jit_exit, interp_st, blk.stopper)) {
      g_failed++;
      printf("    FAIL round %d: exit %d disagrees with the interpreter (step %d, stopper %s)\n",
             round,
             (int)jit_exit,
             (int)interp_st,
             blk.stopper ? blk.stopper : "none");
    }
    if (memcmp(g_after_interp, g_guest, GUEST_SIZE) != 0) {
      unsigned d = 0;
      size_t bi;
      for (bi = 0; bi < GUEST_SIZE; bi++) {
        if (g_after_interp[bi] != g_guest[bi] && d++ < 4u) {
          printf(
              "    FAIL generated program: guest[%04zX] interp=%02X jit=%02X\n", bi, g_after_interp[bi], g_guest[bi]);
        }
      }
      g_failed++;
    }

    /*
     * AT MOST ONE call to x86p_flag_cf per block.
     *
     * Only the FIRST flag write in a block faces a predecessor from outside it;
     * every later one has a kind this file chose and can derive inline. Calling
     * the helper more often is entirely correct and merely slow, so no state
     * comparison can see it -- measured: treating an instruction that writes no
     * flags (NOT) as if it destroyed the predecessor's kind passed everything.
     *
     * Plus one per instruction whose recorded flag kind is not decidable at
     * translation time: a SHIFT, because a count of zero writes no flags at
     * all and the count can be a register, or IMUL, whose narrow semantics
     * owner materialises the selectively-written flags. The allowance is per
     * instruction rather than blanket so that a backend which stopped deriving
     * carry-in entirely still fails here.
     */
    g_checks++;
    if (blk.flag_helper_calls > 1u + unknown_kind_insns) {
      g_failed++;
      printf("    FAIL round %d: %u carry-in helper call(s) in one block; at most %u is derivable\n",
             round,
             blk.flag_helper_calls,
             1u + unknown_kind_insns);
    }
    g_helper_calls += blk.flag_helper_calls;

    g_programs++;
    g_guest_insns += blk.insns;
    if (blk.ends_in_branch) {
      g_branch_blocks++;
    }

    if (!same_state(&ci, &cj, "generated program")) {
      /* Decode and print the program: a divergence report that does not say
         WHICH instructions were involved is a number, not a lead. */
      /* Decoded from the program AS IT WAS SEEDED, not from memory after the
         run: the program can store into itself, and disassembling the result
         shows instructions that were never executed. */
      uint32_t off = 0;
      uint32_t k;
      for (k = 0; k < blk.insns; k++) {
        X86pInsn di;
        if (off + (uint32_t)X86P_MAX_INSN_LEN > GUEST_SIZE) {
          break;
        }
        if (!x86p_decode(g_before + off, X86P_MAX_INSN_LEN, &di)) {
          break;
        }
        printf("        %04X", off);
        printf("        [%u] %-8s op=%u alu=%u ops=%d k0=%u k1=%u\n",
               k,
               di.mnemonic,
               di.op,
               di.alu,
               di.operands,
               di.operand[0].kind,
               di.operand[1].kind);
        {
          unsigned bi2;
          printf("             bytes:");
          for (bi2 = 0; bi2 < di.length; bi2++) {
            printf(" %02X", g_before[off + bi2]);
          }
          printf("\n");
        }
        off += di.length;
      }
      printf("      round %d, seed %llu, %u guest insn(s), %zu host byte(s)\n",
             round,
             (unsigned long long)seed,
             blk.insns,
             blk.host_bytes);
    }
  }
  munmap(code, 65536);
}

/* ---- the negatives: refusals are named and are not silent ---------------- */

/*
 * ONE guest instruction per comparison.
 *
 * The block differential above proves a whole block agrees, and when it does
 * not it names a divergence that may have been introduced any of fourteen
 * instructions earlier -- a fact about the block, not about an emitter. This
 * runs a single instruction between two identical states, so a failure names
 * the shape that is wrong and nothing else.
 *
 * The terminator is RCPPS (0F 53 C0): decodable, NAMED, and deliberately
 * without semantics -- its result comes from a hardware table to about twelve
 * mantissa bits, and simd.c refuses it rather than writing 1.0f/x. That makes
 * it the stable choice for a stopper: an instruction merely not implemented
 * YET stops being one the day it is implemented, and this test has already
 * had to move twice for that reason. It still checks the block length rather
 * than trusting the arrangement.
 */
static unsigned long g_single_compares;

static void test_jit_matches_interpreter_one_instruction_at_a_time(void) {
  void *code = code_alloc(65536);
  uint64_t rng = 0xA5A5C0FFEEull;
  int round;

  CHECK(code != NULL);
  if (!code) {
    return;
  }

  for (round = 0; round < 4000; round++) {
    X86pMem mem = guest_mem();
    X86pCpu ci;
    X86pCpu cj;
    X86pJitBlock blk;
    char reason[192];
    X86pJitStatus st;
    X86pStepStatus interp_st;
    X86pJitExit jit_exit;
    uint64_t seed;
    uint32_t len;
    uint32_t want;

    memset(g_guest, 0x90, GUEST_SIZE);
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    seed = rng;
    /*
     * One instruction, then TWO. A single instruction always translates with
     * no known predecessor flag state, so the inlined carry-in forms -- the
     * whole point of tracking the previous flag kind -- are only reachable
     * from the second instruction of a pair. Testing only singletons leaves
     * that path to the block differential, which cannot say which of fourteen
     * instructions was wrong.
     */
    want = 1u + (uint32_t)(round & 3);
    len = emit_guest_insn(g_guest, seed);
    if (len == 0u) {
      continue;
    }
    {
      uint32_t k2;
      uint32_t bad = 0;
      for (k2 = 1u; k2 < want; k2++) {
        uint32_t len2;
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        len2 = emit_guest_insn(g_guest + len, rng);
        if (len2 == 0u) {
          bad = 1;
          break;
        }
        len += len2;
      }
      if (bad) {
        continue;
      }
    }
    /* RCPPS XMM0, XMM0 -- decodable, refused on purpose, stops the block. */
    g_guest[len] = 0x0Fu;
    g_guest[len + 1u] = 0x53u;
    g_guest[len + 2u] = 0xC0u;

    seed_cpu(&ci, seed);
    seed_cpu(&cj, seed);

    st = x86p_jit_translate(&mem, GUEST_BASE, code, 65536, &blk, reason, sizeof reason);
    if (st == kX86pJitUnsupportedAtEntry) {
      g_refused++;
      continue; /* a shape this backend refuses by name */
    }
    g_checks++;
    if (st != kX86pJitOk) {
      g_failed++;
      printf("    FAIL single round %d: translate -> %s (%s)\n", round, x86p_jit_status_name(st), reason);
      continue;
    }
    g_checks++;
    /* A branch ends the block, so a pair whose first instruction is a branch
       legitimately translates as one. Anything LONGER than asked for means the
       PUSH terminator gained an emitter and this test quietly stopped being
       what it claims to be. */
    if (blk.insns > want || blk.insns == 0u) {
      g_failed++;
      printf("    FAIL single round %d: asked for %u instruction(s), block has %u\n", round, want, blk.insns);
      continue;
    }
    want = blk.insns;

    memcpy(g_before, g_guest, GUEST_SIZE);
    interp_st = run_interp(&ci, &mem, want);
    memcpy(g_after_interp, g_guest, GUEST_SIZE);
    memcpy(g_guest, g_before, GUEST_SIZE);
    jit_exit = x86p_jit_enter(&blk, &cj);
    g_single_compares++;
    g_checks++;
    if (!exit_agrees(jit_exit, interp_st, blk.stopper)) {
      g_failed++;
      printf("    FAIL single round %d: exit %d disagrees with the interpreter (step %d, stopper %s)\n",
             round,
             (int)jit_exit,
             (int)interp_st,
             blk.stopper ? blk.stopper : "none");
    }

    if (memcmp(g_after_interp, g_guest, GUEST_SIZE) != 0 || !same_state(&ci, &cj, "single instruction")) {
      uint32_t o = 0;
      uint32_t k;
      for (k = 0; k < want; k++) {
        X86pInsn di;
        unsigned bi;
        if (!x86p_decode(g_before + o, X86P_MAX_INSN_LEN, &di)) {
          break;
        }
        printf("        %-8s alu=%u k0=%u k1=%u bytes:", di.mnemonic, di.alu, di.operand[0].kind, di.operand[1].kind);
        for (bi = 0; bi < di.length; bi++) {
          printf(" %02X", g_before[o + bi]);
        }
        printf("\n");
        o += di.length;
      }
      printf("        single round %d, seed %llu\n", round, (unsigned long long)seed);
      g_failed++;
    }
  }
  munmap(code, 65536);
}

static void test_unsupported_at_entry_produces_no_block(void) {
  void *code = code_alloc(4096);
  X86pMem mem = guest_mem();
  X86pJitBlock blk;
  char reason[192];
  X86pJitStatus st;

  CHECK(code != NULL);
  if (!code) {
    return;
  }
  memset(g_guest, 0, GUEST_SIZE);
  /* RCPPS XMM0, XMM0 -- decodes, and is refused on purpose. */
  g_guest[0] = 0x0Fu;
  g_guest[1] = 0x53u;
  g_guest[2] = 0xC0u;

  memset(&blk, 0xEE, sizeof blk);
  reason[0] = '\0';
  st = x86p_jit_translate(&mem, GUEST_BASE, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitUnsupportedAtEntry);
  /* The refusal NAMES the instruction. "Unsupported" without a mnemonic makes
     the unmodelled set a number instead of a work list. */
  CHECK(strstr(reason, "RCPPS") != NULL);
  munmap(code, 4096);
}

static void test_block_stops_at_unsupported_with_eip_on_it(void) {
  void *code = code_alloc(4096);
  X86pMem mem = guest_mem();
  X86pCpu cpu;
  X86pJitBlock blk;
  char reason[192];
  X86pJitStatus st;
  X86pJitExit exit;

  CHECK(code != NULL);
  if (!code) {
    return;
  }
  memset(g_guest, 0, GUEST_SIZE);
  /* MOV EAX, 0x11223344 ; RCPPS XMM0, XMM0 */
  g_guest[0] = 0xB8u;
  put_u32(g_guest + 1, 0x11223344u);
  g_guest[5] = 0x0Fu;
  g_guest[6] = 0x53u;
  g_guest[7] = 0xC0u;

  st = x86p_jit_translate(&mem, GUEST_BASE, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitOk);
  if (st != kX86pJitOk) {
    munmap(code, 4096);
    return;
  }
  CHECK(blk.insns == 1u);
  CHECK(blk.guest_len == 5u);
  CHECK(blk.stopper != NULL && strcmp(blk.stopper, "RCPPS") == 0);

  seed_cpu(&cpu, 99u);
  exit = x86p_jit_enter(&blk, &cpu);
  CHECK(exit == kX86pJitExitUnsupported);
  CHECK(cpu.reg[kX86pEax] == 0x11223344u);
  /* EIP points AT the instruction that could not be translated, so the caller
     can hand exactly that one to the interpreter. Pointing past it would skip
     an instruction, silently. */
  CHECK(cpu.eip == GUEST_BASE + 5u);
  munmap(code, 4096);
}

static void test_out_of_space_is_refused_not_truncated(void) {
  void *code = code_alloc(4096);
  X86pMem mem = guest_mem();
  X86pJitBlock blk;
  char reason[192];
  X86pJitStatus st;
  uint64_t rng = 7u;
  Prog pr;

  CHECK(code != NULL);
  if (!code) {
    return;
  }
  memset(g_guest, 0x90, GUEST_SIZE);
  pr = generate(&rng, 64u);
  CHECK(pr.insns > 0u);

  /* A buffer too small for even the prologue plus one instruction. The result
     must be a refusal -- a truncated block would end without a RET. */
  reason[0] = '\0';
  st = x86p_jit_translate(&mem, GUEST_BASE, code, 8u, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitOutOfSpace || st == kX86pJitUnsupportedAtEntry);
  CHECK(reason[0] != '\0');
  munmap(code, 4096);
}

static void test_fetch_fault_at_an_unmapped_eip(void) {
  void *code = code_alloc(4096);
  X86pMem mem = guest_mem();
  X86pJitBlock blk;
  char reason[192];
  X86pJitStatus st;

  CHECK(code != NULL);
  if (!code) {
    return;
  }
  reason[0] = '\0';
  st = x86p_jit_translate(&mem, GUEST_BASE + GUEST_SIZE + 0x1000u, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitFetchFault);
  CHECK(reason[0] != '\0');
  munmap(code, 4096);
}

int main(void) {
  guest_mem_init();
  if (!x86p_jit_available()) {
    /* Not a pass. A host with no backend must say so rather than report a
       clean run over a suite that executed nothing. */
    printf("NO x86-64 BACKEND on this host: this suite cannot run and claims nothing\n");
    return 77; /* ctest: skipped */
  }

  RUN(test_jit_matches_interpreter_on_generated_programs);
  RUN(test_jit_matches_interpreter_one_instruction_at_a_time);
  RUN(test_unsupported_at_entry_produces_no_block);
  RUN(test_block_stops_at_unsupported_with_eip_on_it);
  RUN(test_out_of_space_is_refused_not_truncated);
  RUN(test_fetch_fault_at_an_unmapped_eip);

  printf("\n%d check(s), %d failure(s) in %d test(s)\n", g_checks, g_failed, g_test_failed);
  printf("%lu program(s), %lu guest instruction(s) translated, %lu full-machine comparison(s)\n",
         g_programs,
         g_guest_insns,
         g_state_compares);
  printf("%lu of %lu block(s) ended in a translated branch\n", g_branch_blocks, g_programs);
  printf("%lu single-instruction comparison(s), %lu round(s) skipped on a named refusal, %lu on a store into\n"
         "the program itself;\n%lu carry-in helper call(s) over %lu block(s)\n",
         g_single_compares,
         g_refused,
         g_self_modified,
         g_helper_calls,
         g_programs);
  if (g_single_compares == 0u) {
    printf("NO single-instruction comparison ran: this suite claims nothing\n");
    return 1;
  }
  if (g_programs == 0u || g_state_compares == 0u) {
    printf("REFUSED: the differential compared nothing; these results mean nothing\n");
    return 1;
  }
  if (g_branch_blocks == 0u) {
    /* The generator emits branches; zero here means it stopped, or that every
       branch was refused before emission. Either way the branch path was never
       executed and a clean run would be claiming coverage it does not have. */
    printf("REFUSED: no block ended in a branch; the branch path was never exercised\n");
    return 1;
  }
  return g_failed ? 1 : 0;
}
