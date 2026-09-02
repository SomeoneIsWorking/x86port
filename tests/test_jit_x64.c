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

#define GUEST_BASE 0x00010000u
#define GUEST_SIZE 4096u

static uint8_t g_guest[GUEST_SIZE];

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
  unsigned pick = (unsigned)(r % 20u);
  unsigned dst = (unsigned)((r >> 3) & 7u);
  unsigned src = (unsigned)((r >> 6) & 7u);
  unsigned aluop = (unsigned)((r >> 9) & 7u);

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

static void seed_cpu(X86pCpu *cpu, uint64_t r) {
  int i;
  x86p_cpu_reset(cpu);
  for (i = 0; i < kX86pRegCount; i++) {
    r = r * 6364136223846793005ull + 1442695040888963407ull;
    cpu->reg[i] = interesting(r);
  }
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
static void run_interp(X86pCpu *cpu, const X86pMem *mem, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) {
    if (x86p_step(cpu, mem, NULL) != kX86pStepOk) {
      return;
    }
  }
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

    run_interp(&ci, &mem, blk.insns);
    (void)x86p_jit_enter(&blk, &cj);

    g_programs++;
    g_guest_insns += blk.insns;
    if (blk.ends_in_branch) {
      g_branch_blocks++;
    }

    if (!same_state(&ci, &cj, "generated program")) {
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
  /* PUSH EAX -- decodes, has no emitter in this build. */
  g_guest[0] = 0x50u;

  memset(&blk, 0xEE, sizeof blk);
  reason[0] = '\0';
  st = x86p_jit_translate(&mem, GUEST_BASE, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitUnsupportedAtEntry);
  /* The refusal NAMES the instruction. "Unsupported" without a mnemonic makes
     the unmodelled set a number instead of a work list. */
  CHECK(strstr(reason, "PUSH") != NULL);
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
  /* MOV EAX, 0x11223344 ; PUSH EAX */
  g_guest[0] = 0xB8u;
  put_u32(g_guest + 1, 0x11223344u);
  g_guest[5] = 0x50u;

  st = x86p_jit_translate(&mem, GUEST_BASE, code, 4096, &blk, reason, sizeof reason);
  CHECK(st == kX86pJitOk);
  if (st != kX86pJitOk) {
    munmap(code, 4096);
    return;
  }
  CHECK(blk.insns == 1u);
  CHECK(blk.guest_len == 5u);
  CHECK(blk.stopper != NULL && strcmp(blk.stopper, "PUSH") == 0);

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
