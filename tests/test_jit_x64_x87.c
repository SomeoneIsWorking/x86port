/*
 * test_jit_x64_x87 -- the x87 inline fast paths, at their fallback edges.
 *
 * jit_x64_x87_inline.c emits the ordinary case of FLD, the arithmetic forms
 * and FST/FSTP inline, and jumps to the helper sequence for everything else.
 * test_jit_x64's generated programs reach the ordinary case constantly and the
 * edges almost never: they never load a control word, never divide by a zero
 * on purpose, never fill the stack, and never arm the op census. Each program
 * here is written to stand on one of those edges, and is run through BOTH
 * engines from the same state.
 *
 * The comparison is stricter than test_jit_x64's: the whole x87 register
 * file, including the TAG of every register (a wrong Zero/Special/Valid is
 * invisible to a value comparison), TOP, the control and status words, the
 * census counters, the data page, and the exit reason.
 *
 * Consecutive inline forms keep their values on the HOST x87 stack, so every
 * run also checks that the block left that stack empty, whichever way it
 * exited: a value left behind breaks the next host call that uses x87.
 */
#include "cpu.h"
#include "exec.h"
#include "jit_x64.h"
#include "jit_x64_harness.h"
#include "jit_x87_predicates.h"
#include "x87.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define DATA_OFF 0x800u
#define DATA_BYTES 0x100u

static uint8_t *g_guest;

typedef struct Case {
  const char *name;
  uint8_t code[64];
  uint32_t code_len;
  uint32_t insns;
  /* Little-endian dwords at [EBX]. */
  uint32_t data[8];
  int census;
} Case;

static void fail(const char *name, const char *what) {
  g_failed++;
  printf("    FAIL %s: %s\n", name, what);
}

static void seed(X86pCpu *cpu) {
  x86p_cpu_reset(cpu);
  cpu->reg[kX86pEbx] = GUEST_BASE + DATA_OFF;
  /* ESI is past the mapping: an access through it faults. */
  cpu->reg[kX86pEsi] = GUEST_BASE + GUEST_SIZE + 0x100u;
  cpu->reg[kX86pEsp] = GUEST_BASE + 0x700u;
  cpu->eip = GUEST_BASE;
}

static void load_case(const Case *k) {
  memset(g_guest, 0x90, GUEST_SIZE);
  memcpy(g_guest, k->code, k->code_len);
  /* RCPPS XMM0, XMM0: decodable and refused by name, so the block stops here. */
  g_guest[k->code_len] = 0x0Fu;
  g_guest[k->code_len + 1u] = 0x53u;
  g_guest[k->code_len + 2u] = 0xC0u;
  memcpy(g_guest + DATA_OFF, k->data, sizeof k->data);
}

static int same_x87(const Case *k, const X86pCpu *a, const X86pCpu *b) {
  int ok = 1;
  int p;
  char what[160];
  if (a->x87.top != b->x87.top || a->x87.control != b->x87.control || a->x87.status != b->x87.status) {
    snprintf(what,
             sizeof what,
             "top/cw/sw interp=(%u %04X %04X) jit=(%u %04X %04X)",
             a->x87.top,
             a->x87.control,
             a->x87.status,
             b->x87.top,
             b->x87.control,
             b->x87.status);
    fail(k->name, what);
    ok = 0;
  }
  for (p = 0; p < X86P_X87_REGS; p++) {
    if (a->x87.tag[p] != b->x87.tag[p]) {
      snprintf(what, sizeof what, "physical %d tag interp=%u jit=%u", p, a->x87.tag[p], b->x87.tag[p]);
      fail(k->name, what);
      ok = 0;
    } else if (memcmp(&a->x87.reg[p], &b->x87.reg[p], 10) != 0) {
      /* An empty register is compared too: a pop leaves its value behind,
         and FSAVE writes all eight. */
      snprintf(what, sizeof what, "physical %d value differs", p);
      fail(k->name, what);
      ok = 0;
    }
  }
  return ok;
}

/* Every host x87 register is empty: the tag word FNSTENV reports, at byte 8
   of its 32-bit layout, is all ones. FNSTENV masks exceptions, so the
   environment is loaded back; EMMS then empties the stack for the next case
   whatever the answer was. */
static int host_x87_empty(void) {
#if defined(__x86_64__)
  uint8_t env[28];
  uint16_t tags;
  __asm__ volatile("fnstenv %0\n\tfldenv %0\n\temms" : "+m"(env));
  memcpy(&tags, env + 8, sizeof tags);
  return tags == 0xFFFFu;
#else
  return 1;
#endif
}

/* `ok_exit` is how a block that completes exits: at the refused stopper, or
   at a branch of its own. */
static void run_case(const Case *k, void *code, X86pJitExit ok_exit) {
  X86pMem mem = jit_x64_harness_mem(g_guest);
  X86pCpu ci;
  X86pCpu cj;
  X86pX87OpCensus census_i;
  X86pX87OpCensus census_j;
  X86pJitBlock blk;
  char reason[192];
  uint8_t after_interp[DATA_BYTES];
  X86pStepStatus interp = kX86pStepOk;
  X86pJitExit exit_code;
  X86pJitStatus st;
  uint32_t i;

  load_case(k);
  seed(&ci);
  seed(&cj);
  memset(&census_i, 0, sizeof census_i);
  memset(&census_j, 0, sizeof census_j);
  if (k->census) {
    x86p_x87_set_op_census(&ci.x87, &census_i);
    x86p_x87_set_op_census(&cj.x87, &census_j);
  }

  g_checks++;
  st = jit_x64_harness_translate(&mem, GUEST_BASE, code, 65536, &blk, reason, sizeof reason);
  if (st != kX86pJitOk) {
    printf("      %s\n", reason);
    snprintf(reason, sizeof reason, "translate -> %s", x86p_jit_status_name(st));
    fail(k->name, reason);
    return;
  }
  g_checks++;
  /* Every instruction must be TRANSLATED; a block that stops early has not
     run the emitter this test is about. */
  if (blk.insns != k->insns) {
    snprintf(reason, sizeof reason, "block has %u instruction(s), the case has %u", blk.insns, k->insns);
    fail(k->name, reason);
    return;
  }

  for (i = 0; i < k->insns && interp == kX86pStepOk; i++) {
    interp = x86p_step(&ci, &mem, NULL);
  }
  memcpy(after_interp, g_guest + DATA_OFF, DATA_BYTES);
  load_case(k);
  exit_code = x86p_jit_enter(&blk, &cj);
  g_checks++;
  if (!host_x87_empty()) {
    fail(k->name, "the block left values on the host x87 stack");
  }

  g_checks++;
  if ((interp == kX86pStepMemoryFault) != (exit_code == kX86pJitExitMemoryFault) ||
      (interp == kX86pStepOk && exit_code != ok_exit)) {
    snprintf(reason, sizeof reason, "exit %d disagrees with the interpreter's step %d", (int)exit_code, (int)interp);
    fail(k->name, reason);
  }
  g_checks++;
  if (ci.eip != cj.eip) {
    snprintf(reason, sizeof reason, "eip interp=%08X jit=%08X", ci.eip, cj.eip);
    fail(k->name, reason);
  }
  g_checks++;
  if (memcmp(ci.reg, cj.reg, sizeof ci.reg) != 0) {
    fail(k->name, "general registers differ");
  }
  g_checks++;
  same_x87(k, &ci, &cj);
  g_checks++;
  if (memcmp(after_interp, g_guest + DATA_OFF, DATA_BYTES) != 0) {
    fail(k->name, "data page differs");
  }
  g_checks++;
  if (memcmp(&census_i, &census_j, sizeof census_i) != 0) {
    fail(k->name, "op census differs");
  }
}

/* Encodings, spelled once. [EBX+d8] operands use ModRM mod=01, rm=011. */
#define FLD_M32(d) 0xD9, 0x43, (d)            /* D9 /0 */
#define FLD_M64(d) 0xDD, 0x43, (d)            /* DD /0 */
#define FILD_M16(d) 0xDF, 0x43, (d)           /* DF /0 */
#define FILD_M32(d) 0xDB, 0x43, (d)           /* DB /0 */
#define FILD_M64(d) 0xDF, 0x6B, (d)           /* DF /5 */
#define FLDCW(d) 0xD9, 0x6B, (d)              /* D9 /5 */
#define FADD_M32(d) 0xD8, 0x43, (d)           /* D8 /0 */
#define FMUL_M64(d) 0xDC, 0x4B, (d)           /* DC /1 */
#define FDIV_M32(d) 0xD8, 0x73, (d)           /* D8 /6 */
#define FDIVR_M32(d) 0xD8, 0x7B, (d)          /* D8 /7 */
#define FDIV_M64(d) 0xDC, 0x73, (d)           /* DC /6 */
#define FIDIV_M16(d) 0xDE, 0x73, (d)          /* DE /6 */
#define FIDIV_M32(d) 0xDA, 0x73, (d)          /* DA /6 */
#define FIADD_M16(d) 0xDE, 0x43, (d)          /* DE /0 */
#define FST_M32(d) 0xD9, 0x53, (d)            /* D9 /2 */
#define FSTP_M32(d) 0xD9, 0x5B, (d)           /* D9 /3 */
#define FSTP_M64(d) 0xDD, 0x5B, (d)           /* DD /3 */
#define FSTP_M32_FAULT 0xD9, 0x9E, 0, 0, 0, 0 /* D9 /3 [esi+disp32] */
#define FLD_M32_FAULT 0xD9, 0x86, 0, 0, 0, 0  /* D9 /0 [esi+disp32] */
#define MOV_EAX_EBX 0x89, 0xD8
#define MOV_EAX_M_FAULT 0x8B, 0x86, 0, 0, 0, 0 /* 8B /r [esi+disp32] */
#define SHL_EAX(n) 0xC1, 0xE0, (n)
#define ADD_EAX_EBX 0x01, 0xD8
#define INC_EAX 0x40
#define JMP_NEXT 0xEB, 0x00
#define FSQRT 0xD9, 0xFA
#define FCHS 0xD9, 0xE0
#define FABS 0xD9, 0xE1
#define FLDZ 0xD9, 0xEE
#define FLD1 0xD9, 0xE8
#define FCOM_ST(i) 0xD8, (0xD0 + (i))
#define FCOMP_ST(i) 0xD8, (0xD8 + (i))
#define FCOMPP 0xDE, 0xD9
#define FUCOMPP 0xDA, 0xE9
#define FCOM_M32(d) 0xD8, 0x53, (d)  /* D8 /2 */
#define FCOMP_M64(d) 0xDC, 0x5B, (d) /* DC /3 */
#define FICOM_M32(d) 0xDA, 0x53, (d) /* DA /2 */
#define FNSTSW_AX 0xDF, 0xE0
#define MOV_M_EAX(d) 0x89, 0x43, (d) /* mov [ebx+d8], eax */
#define FLD_ST(i) 0xD9, (0xC0 + (i))
#define FST_ST(i) 0xDD, (0xD0 + (i))
#define FSTP_ST(i) 0xDD, (0xD8 + (i))
#define FADD_ST0_ST(i) 0xD8, (0xC0 + (i))
#define FMUL_ST0_ST(i) 0xD8, (0xC8 + (i))
#define FSUB_ST0_ST(i) 0xD8, (0xE0 + (i))
#define FSUBP_ST_ST0(i) 0xDE, (0xE8 + (i))
#define FXCH_ST(i) 0xD9, (0xC8 + (i))
#define FSUBR_ST_ST0(i) 0xDC, (0xE8 + (i))
#define FDIV_ST0_ST(i) 0xD8, (0xF0 + (i))
#define FDIVP_ST_ST0(i) 0xDE, (0xF8 + (i))
#define FDIVRP_ST_ST0(i) 0xDE, (0xF0 + (i))
#define FMULP_ST_ST0(i) 0xDE, (0xC8 + (i))
#define FADDP_ST_ST0(i) 0xDE, (0xC0 + (i))

#define F32_ONE 0x3F800000u
#define F32_THREE 0x40400000u
#define F32_ZERO 0x00000000u
#define F32_NEG_ZERO 0x80000000u
#define F32_INF 0x7F800000u
#define F32_QNAN 0x7FC00000u
#define F32_SNAN 0x7F800001u
#define F32_DENORMAL 0x00000001u
#define F32_HUGE 0x7F7FFFFFu
#define CW_TRUNCATE 0x0F7Fu
#define CW_SINGLE 0x007Fu

#define CODE(...) {__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__})

static const Case kCases[] = {
    /* The ordinary cases, which take the inline path. */
    {"ordinary arithmetic",
     CODE(FLD_M32(0),
          FLD_M32(4),
          FADD_ST0_ST(1),
          FSUB_ST0_ST(1),
          FDIV_ST0_ST(1),
          FSUBR_ST_ST0(1),
          FMULP_ST_ST0(1),
          FSTP_M32(8)),
     8,
     {F32_ONE, F32_THREE},
     0},
    {"memory arithmetic",
     CODE(FLD_M32(0), FADD_M32(4), FMUL_M64(16), FDIV_M32(4), FDIVR_M32(4), FSTP_M64(24)),
     6,
     {F32_ONE, F32_THREE, 0, 0, 0, 0x3FF80000u},
     0},
    {"integer operands",
     CODE(FILD_M16(0),
          FILD_M32(4),
          FILD_M64(8),
          FIADD_M16(0),
          FIDIV_M32(4),
          FIDIV_M16(0),
          FSTP_M64(16),
          FSTP_M32(24),
          FSTP_M32(28)),
     9,
     {0xFFFF8003u, 7u, 0x89ABCDEFu, 0x80000000u},
     0},
    {"register copies",
     CODE(FLD_M32(0), FLD_M32(4), FLD_ST(1), FST_ST(2), FSTP_ST(3), FSTP_ST(0), FST_M32(8)),
     7,
     {F32_ONE, F32_THREE},
     0},
    {"special values keep their tags",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(8), FLD_M32(12), FLD_M32(16), FLD_M32(20), FADD_ST0_ST(1), FMULP_ST_ST0(2)),
     8,
     {F32_INF, F32_QNAN, F32_SNAN, F32_DENORMAL, F32_NEG_ZERO, F32_ZERO},
     0},
    /* 1e-300 squared four times is 1e-4800, and times 1e-140 is below the
       ten-byte format's smallest normal: a denormal, whose zero exponent and
       nonzero significand the tag word calls valid. */
    {"a result below the normal range is tagged valid",
     CODE(FLD_M64(0),
          FMUL_ST0_ST(0),
          FMUL_ST0_ST(0),
          FMUL_ST0_ST(0),
          FMUL_ST0_ST(0),
          FMUL_M64(8),
          FLD_M32(16),
          FMUL_ST0_ST(1)),
     8,
     {0xC2F8F359u, 0x01A56E1Fu, 0x127BD87Eu, 0x22DE7C5Fu, F32_ONE},
     0},
    {"overflow to infinity",
     CODE(FLD_M32(0), FMUL_M64(8), FMUL_M64(8), FMUL_M64(8), FSTP_M32(4)),
     5,
     {F32_HUGE, 0, 0x00000000u, 0x7FE00000u},
     0},

    /* Division by a zero: the helper owns ZE. */
    {"FDIVP by a zero register", CODE(FLD_M32(0), FLD_M32(4), FDIVP_ST_ST0(1), FSTP_M32(8)), 4, {F32_ONE, F32_ZERO}, 0},
    {"FDIVRP by a zero register",
     CODE(FLD_M32(4), FLD_M32(0), FDIVRP_ST_ST0(1), FSTP_M32(8)),
     4,
     {F32_ONE, F32_NEG_ZERO},
     0},
    {"FDIV by a zero m32", CODE(FLD_M32(0), FDIV_M32(4), FSTP_M32(8)), 3, {F32_ONE, F32_NEG_ZERO}, 0},
    {"FDIV by a zero m64", CODE(FLD_M32(0), FDIV_M64(8), FSTP_M32(4)), 3, {F32_ONE, 0, 0, 0x80000000u}, 0},
    {"FDIVR by a zero ST(0)", CODE(FLD_M32(4), FDIVR_M32(0), FSTP_M32(8)), 3, {F32_ONE, F32_ZERO}, 0},
    {"FIDIV by a zero m16", CODE(FLD_M32(0), FIDIV_M16(4), FSTP_M32(8)), 3, {F32_ONE, 0xFFFF0000u}, 0},
    {"zero over zero", CODE(FLD_M32(0), FDIV_M32(0), FSTP_M32(8)), 3, {F32_ZERO}, 0},

    /* A guest control word the host does not hold: the helper's FLDCW path. */
    {"truncating control word",
     CODE(FLDCW(16), FLD_M32(0), FDIV_M32(4), FLD_M32(0), FDIV_ST0_ST(1), FST_M32(8), FSTP_M64(24)),
     7,
     {F32_ONE, F32_THREE, 0, 0, CW_TRUNCATE},
     0},
    {"single precision control word",
     CODE(FLDCW(16), FLD_M32(0), FDIV_M32(4), FADD_M32(4), FSTP_M64(24)),
     5,
     {F32_ONE, F32_THREE, 0, 0, CW_SINGLE},
     0},

    /* Stack faults. */
    {"arithmetic on an empty stack", CODE(FADD_ST0_ST(1), FADD_M32(0), FDIVP_ST_ST0(1)), 3, {F32_ONE}, 0},
    {"empty source register", CODE(FLD_M32(0), FADD_ST0_ST(3), FLD_ST(5), FST_ST(4)), 4, {F32_ONE}, 0},
    {"push onto a full stack",
     CODE(FLD_M32(0),
          FLD_M32(0),
          FLD_M32(0),
          FLD_M32(0),
          FLD_M32(0),
          FLD_M32(0),
          FLD_M32(0),
          FLD_M32(0),
          FLD_M32(4),
          FLD_ST(3),
          FILD_M32(4)),
     11,
     {F32_ONE, F32_THREE},
     0},
    {"store from an empty stack to a bad address", CODE(FSTP_M32_FAULT, FSTP_M32(0)), 2, {F32_ONE}, 0},
    {"store from a full stack to a bad address", CODE(FLD_M32(0), FSTP_M32_FAULT), 2, {F32_ONE}, 0},

    /* The host-stack mirror: its depth limit, its reloads and its exits. */
    {"a chain deeper than the mirror",
     CODE(FLD_M32(0),
          FLD_M32(4),
          FLD_M32(0),
          FLD_M32(4),
          FLD_M32(0),
          FLD_M32(4),
          FLD_M32(0),
          FADD_ST0_ST(6),
          FLD_M32(4),
          FMUL_ST0_ST(6),
          FSUBR_ST_ST0(5),
          FDIVP_ST_ST0(6),
          FST_ST(5),
          FADD_ST0_ST(7),
          FSUBP_ST_ST0(4),
          FSTP_M32(8),
          FSTP_M64(16)),
     17,
     {F32_ONE, F32_THREE},
     0},
    {"a slow path inside a chain",
     CODE(
         FLD_M32(0), FLD_M32(4), FADD_ST0_ST(3), FMUL_ST0_ST(1), FXCH_ST(1), FSUB_ST0_ST(1), FSTP_M32(8), FSTP_M32(12)),
     8,
     {F32_ONE, F32_THREE},
     0},
    {"an integer instruction inside a chain",
     CODE(FLD_M32(0), FLD_M32(4), MOV_EAX_EBX, FADDP_ST_ST0(1), FLD_ST(0), FMULP_ST_ST0(1), FSTP_M32(8)),
     7,
     {F32_ONE, F32_THREE},
     0},
    {"integer arithmetic inside a chain",
     CODE(FLD_M32(0), FLD_M32(4), SHL_EAX(3), ADD_EAX_EBX, FADDP_ST_ST0(1), FLD_ST(0), FSTP_M32(8), FSTP_M32(12)),
     8,
     {F32_ONE, F32_THREE},
     0},
    /* The block's first flag writer has an unknown predecessor, so INC's
       carry-in calls out while the mirror holds two values. */
    {"a carry-in call inside a chain",
     CODE(FLD_M32(0), FLD_M32(4), INC_EAX, FMULP_ST_ST0(1), FSTP_M32(8)),
     5,
     {F32_ONE, F32_THREE},
     0},
    /* Lazy write-back: dirty values reach the register file at a flush, a
       pop, an eviction and the block's end. More than two dirty values at a
       flush go through the spill routine. */
    {"four dirty values at a carry-in call",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(0), FMUL_ST0_ST(1), FLD_M32(4), INC_EAX, FADDP_ST_ST0(1), FSTP_M32(8)),
     8,
     {F32_ONE, F32_THREE},
     0},
    {"four dirty values at the end of the block",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(0), FLD_M32(4), FADD_ST0_ST(3)),
     5,
     {F32_ONE, F32_THREE},
     0},
    {"a slow path with four dirty values",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(0), FLD_M32(4), FADD_ST0_ST(5), FSTP_M32(8), FSTP_M32(12)),
     7,
     {F32_ONE, F32_THREE},
     0},
    /* The occupancy cache (THE CACHE) rotates on every push and pop. A wrong
       direction reports an empty ST(5) as occupied here, and the multiply
       would read it instead of faulting. */
    {"an empty register after pushes and a pop",
     CODE(FLD_M32(0), FLD_M32(4), FSTP_M32(8), FLD_M32(4), FMUL_ST0_ST(5), FSTP_M32(12)),
     6,
     {F32_ONE, F32_THREE},
     0},
    /* FSQRT runs its helper, so the cache is read back from memory with TOP at
       3 and ST(0..4) occupied: a rotate the wrong way reports ST(6) occupied. */
    {"an empty register after the cache is read back",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(0), FLD_M32(4), FLD_M32(0), FSQRT, FMUL_ST0_ST(6), FSTP_M32(8)),
     8,
     {F32_ONE, F32_THREE},
     0},
    /* INC's carry-in empties the mirror, so FDIV's early guards leave with an
       empty host stack and its divisor guard, after the reload, with two
       values: the slow path cannot know which statically. */
    {"a zero divisor after the mirror was emptied",
     CODE(FLD_M32(0), FLD_M32(4), INC_EAX, FDIV_ST0_ST(1), FSTP_M32(8), FSTP_M32(12)),
     6,
     {F32_ZERO, F32_ONE},
     0},
    {"a dirty ST(0) popped into a register",
     CODE(FLD_M32(0), FLD_M32(4), FMUL_ST0_ST(1), FSTP_ST(1), FLD_ST(0), FADDP_ST_ST0(1), FSTP_M32(8)),
     7,
     {F32_ONE, F32_THREE},
     0},
    {"a store to a register the mirror does not hold",
     CODE(FLD_M32(0),
          FLD_M32(4),
          FLD_M32(0),
          FLD_M32(4),
          FLD_M32(0),
          FLD_M32(4),
          FLD_M32(0),
          FLD_M32(4),
          FADD_ST0_ST(1),
          FST_ST(7),
          FSTP_M32(8),
          FSTP_M32(12)),
     12,
     {F32_ONE, F32_THREE},
     0},
    {"an integer fault with the mirror live",
     CODE(FLD_M32(0), FLD_M32(4), FMUL_ST0_ST(1), MOV_EAX_M_FAULT, FSTP_M32(8)),
     5,
     {F32_ONE, F32_THREE},
     0},
    {"a zero divisor the mirror holds",
     CODE(FLD_M32(4), FLD_M32(0), FDIV_ST0_ST(1), FDIVRP_ST_ST0(1), FSTP_M32(8)),
     5,
     {F32_ONE, F32_ZERO},
     0},
    {"a store fault with the mirror full",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(0), FADD_ST0_ST(2), FSTP_M32_FAULT),
     5,
     {F32_ONE, F32_THREE},
     0},
    {"a load fault with the mirror full",
     CODE(FLD_M32(0), FLD_M32(4), FMUL_ST0_ST(1), FLD_M32_FAULT),
     4,
     {F32_ONE, F32_THREE},
     0},
    {"exchange and sign inside a chain",
     CODE(FLD_M32(0), FLD_M32(4), FXCH_ST(1), FCHS, FSUB_ST0_ST(1), FABS, FXCH_ST(1), FSTP_M32(8), FSTP_M32(12)),
     9,
     {F32_ONE, F32_THREE},
     0},
    {"sign of a NaN and a zero",
     CODE(FLD_M32(0), FABS, FLD_M32(4), FCHS, FLD_M32(8), FABS, FSTP_M32(12), FSTP_M32(16), FSTP_M32(20)),
     9,
     {0xFFC00000u, F32_ZERO, F32_NEG_ZERO},
     0},
    {"register compares",
     CODE(FLD_M32(0),
          FLD_M32(4),
          FCOM_ST(1),
          FNSTSW_AX,
          MOV_M_EAX(32),
          FXCH_ST(1),
          FCOM_ST(1),
          FNSTSW_AX,
          MOV_M_EAX(36),
          FLD_ST(0),
          FCOMP_ST(1),
          FNSTSW_AX,
          MOV_M_EAX(40),
          FCOMPP,
          FNSTSW_AX),
     15,
     {F32_ONE, F32_THREE},
     0},
    {"an unordered register compare",
     CODE(FLD_M32(0), FLD_M32(4), FUCOMPP, FNSTSW_AX, MOV_M_EAX(32), FLD_M32(4), FCOM_ST(3)),
     7,
     {F32_ONE, F32_QNAN},
     0},
    {"memory compares",
     CODE(FLD_M32(0),
          FLD_M32(0),
          FCOM_M32(4),
          FNSTSW_AX,
          MOV_M_EAX(32),
          FCOM_M32(0),
          FNSTSW_AX,
          MOV_M_EAX(36),
          FICOM_M32(8),
          FNSTSW_AX,
          MOV_M_EAX(40),
          FCOMP_M64(16),
          FNSTSW_AX),
     13,
     {F32_THREE, F32_ONE, 3u, 0, 0, 0x7FF80000u},
     0},
    {"compare below a memory operand",
     CODE(FLD_M32(4), FLD_M32(4), FCOM_M32(0), FNSTSW_AX),
     4,
     {F32_THREE, F32_ONE},
     0},
    {"an exchange that stays on the stack",
     CODE(FLD_M32(0), FLD_M32(4), FLD_M32(0), FXCH_ST(2), FXCH_ST(1)),
     5,
     {F32_ONE, F32_THREE},
     0},
    {"compare against an empty stack", CODE(FCOM_M32(0), FNSTSW_AX, FCOMPP, FNSTSW_AX), 4, {F32_ONE}, 0},
    {"constants", CODE(FLDZ, FLD1, FADD_ST0_ST(1), FLD1, FLDZ, FLDZ, FLDZ, FLDZ, FLDZ, FLD1, FSTP_M64(16)), 11, {0}, 0},
    {"a chain that runs to the end of the block",
     CODE(FLD_M32(0), FLD_M32(4), FADD_ST0_ST(1)),
     3,
     {F32_ONE, F32_THREE},
     0},

    /* The census counts inside the helper, so an armed census takes it. */
    {"armed census",
     CODE(FLD_M32(0), FLD_M32(4), FMULP_ST_ST0(1), FADD_M32(4), FSTP_M32(8)),
     5,
     {F32_ONE, F32_THREE},
     1},
};

/* Blocks that end at their own branch, whose exit leaves through the chained
   exit path rather than the block end. */
static const Case kBranchCases[] = {
    {"a chain that ends in a branch",
     CODE(FLD_M32(0), FLD_M32(4), FADD_ST0_ST(1), JMP_NEXT),
     4,
     {F32_ONE, F32_THREE},
     0},
};

static void run_table(const Case *cases, size_t count, void *code, X86pJitExit ok_exit) {
  size_t i;
  for (i = 0; i < count; i++) {
    int before = g_failed;
    run_case(&cases[i], code, ok_exit);
    printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", cases[i].name);
  }
}

int main(void) {
  void *code;
  if (!x86p_jit_available()) {
    printf("SKIP: no x86-64 JIT backend on this host\n");
    return 77;
  }
  if (!x87_values_are_emittable()) {
    /* Every case here starts with an x87 value, which a host without an
       admitted value representation (an MSVC long double is binary64)
       refuses at entry: the emitters are not in this build to test. */
    printf("SKIP: this build admits no x87 values to the JIT\n");
    return 77;
  }
  g_guest = jit_x64_harness_guest_init();
  code = jit_x64_harness_code_alloc(65536);
  if (!code) {
    return 1;
  }
  run_table(kCases, sizeof kCases / sizeof kCases[0], code, kX86pJitExitUnsupported);
  run_table(kBranchCases, sizeof kBranchCases / sizeof kBranchCases[0], code, kX86pJitExitBlockEnd);
  jit_x64_harness_code_free(code, 65536);
  printf("\n%d check(s), %d failure(s) over %zu x87 edge case(s)\n",
         g_checks,
         g_failed,
         sizeof kCases / sizeof kCases[0] + sizeof kBranchCases / sizeof kBranchCases[0]);
  return g_failed ? 1 : 0;
}
