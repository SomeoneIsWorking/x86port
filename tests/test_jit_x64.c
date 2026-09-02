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
 * carry_in stale produces exactly the right destination value and a corrupted
 * machine. Comparing eight registers, EIP, the raw lazy-flag tuple AND all six
 * derived flags is what turns those into failures instead of into a bug found
 * three months later in a game.
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
#include <string.h>
#include <sys/mman.h>

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

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 4096u

static uint8_t g_guest[GUEST_SIZE];
static uint8_t g_before[GUEST_SIZE];
static uint8_t g_after_interp[GUEST_SIZE];

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
  unsigned pick = (unsigned)(r % 35u);
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
  if (a->eip != b->eip) {
    printf("    FAIL %s: EIP interp=%08X jit=%08X\n", what, a->eip, b->eip);
    ok = 0;
  }
  if (a->flags.kind != b->flags.kind || a->flags.a != b->flags.a || a->flags.b != b->flags.b ||
      a->flags.r != b->flags.r || a->flags.w != b->flags.w || a->flags.carry_in != b->flags.carry_in) {
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

    memset(g_guest, 0x90, sizeof g_guest); /* NOP fill: a run that walks off the
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
    memcpy(g_before, g_guest, sizeof g_guest);
    interp_st = run_interp(&ci, &mem, blk.insns);
    memcpy(g_after_interp, g_guest, sizeof g_guest);
    memcpy(g_guest, g_before, sizeof g_guest);
    jit_exit = x86p_jit_enter(&blk, &cj);
    g_checks++;
    if (!exit_agrees(jit_exit, interp_st, blk.stopper)) {
      g_failed++;
      printf("    FAIL round %d: exit %d disagrees with the interpreter (step %d, stopper %s)\n",
             round,
             (int)jit_exit,
             (int)interp_st,
             blk.stopper ? blk.stopper : "none");
    }
    if (memcmp(g_after_interp, g_guest, sizeof g_guest) != 0) {
      unsigned d = 0;
      size_t bi;
      for (bi = 0; bi < sizeof g_guest; bi++) {
        if (g_after_interp[bi] != g_guest[bi] && d++ < 4u) {
          printf(
              "    FAIL generated program: guest[%04zX] interp=%02X jit=%02X\n", bi, g_after_interp[bi], g_guest[bi]);
        }
      }
      g_failed++;
    }

    g_programs++;
    g_guest_insns += blk.insns;
    if (blk.ends_in_branch) {
      g_branch_blocks++;
    }

    if (!same_state(&ci, &cj, "generated program")) {
      /* Decode and print the program: a divergence report that does not say
         WHICH instructions were involved is a number, not a lead. */
      uint32_t off = 0;
      uint32_t k;
      for (k = 0; k < blk.insns; k++) {
        uint8_t ib[X86P_MAX_INSN_LEN];
        X86pInsn di;
        uint32_t j;
        for (j = 0; j < (uint32_t)X86P_MAX_INSN_LEN; j++) {
          uint32_t bv;
          ib[j] = x86p_mem_read(&mem, GUEST_BASE + off + j, 1, &bv) ? (uint8_t)bv : 0u;
        }
        if (!x86p_decode(ib, X86P_MAX_INSN_LEN, &di)) {
          break;
        }
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
            uint32_t bv2;
            printf(" %02X", x86p_mem_read(&mem, GUEST_BASE + off + bi2, 1, &bv2) ? (unsigned)bv2 : 0u);
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
 * The terminator is PUSHFD (0x9C): decodable, and deliberately WITHOUT an
 * emitter, so the block stops where this test means it to. If PUSHFD ever gains
 * one, this test would silently start comparing longer runs than it asked for,
 * so it checks the block length rather than trusting the arrangement.
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

    memset(g_guest, 0x90, sizeof g_guest);
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
    g_guest[len] = 0x9Cu; /* PUSHFD -- decodable, no emitter, stops the block */

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

    memcpy(g_before, g_guest, sizeof g_guest);
    interp_st = run_interp(&ci, &mem, want);
    memcpy(g_after_interp, g_guest, sizeof g_guest);
    memcpy(g_guest, g_before, sizeof g_guest);
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

    if (memcmp(g_after_interp, g_guest, sizeof g_guest) != 0 || !same_state(&ci, &cj, "single instruction")) {
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
  memset(g_guest, 0, sizeof g_guest);
  /* PUSHFD -- decodes, has no emitter in this build. */
  g_guest[0] = 0x9Cu;

  memset(&blk, 0xEE, sizeof blk);
  reason[0] = '\0';
  st = x86p_jit_translate(&mem, GUEST_BASE, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitUnsupportedAtEntry);
  /* The refusal NAMES the instruction. "Unsupported" without a mnemonic makes
     the unmodelled set a number instead of a work list. */
  CHECK(strstr(reason, "PUSHFD") != NULL);
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
  memset(g_guest, 0, sizeof g_guest);
  /* MOV EAX, 0x11223344 ; PUSHFD */
  g_guest[0] = 0xB8u;
  put_u32(g_guest + 1, 0x11223344u);
  g_guest[5] = 0x9Cu;

  st = x86p_jit_translate(&mem, GUEST_BASE, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitOk);
  if (st != kX86pJitOk) {
    munmap(code, 4096);
    return;
  }
  CHECK(blk.insns == 1u);
  CHECK(blk.guest_len == 5u);
  CHECK(blk.stopper != NULL && strcmp(blk.stopper, "PUSHFD") == 0);

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
  memset(g_guest, 0x90, sizeof g_guest);
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
  printf(
      "%lu single-instruction comparison(s), %lu round(s) skipped on a named refusal\n", g_single_compares, g_refused);
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
