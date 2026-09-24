/* The real WASM product dispatcher executes each case, compared against the
 * separately linked diagnostic interpreter. All numeric helpers are shared;
 * this verifies emitted operand access, stack lifetime, imports and exits. */
#include "cpu.h"
#include "exec.h"
#include "jit_engine.h"
#include "jit_wasm_lower.h"
#include "memory_sparse.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BASE 0x400000u
#define SIZE 4096u
static uint8_t guest[SIZE], reference[SIZE];
static unsigned checks, failures, cases, entries, faults, refusals;
/* Memory-operand x87 loads lowered across every case, and those that got the
   emitted widening rather than the import call. Both, because each is a
   negative the other cannot show: a zero inline count would mean the fast path
   was never emitted, and an inline count equal to the total would mean the
   forms it must DECLINE -- the integer loads -- were never lowered here. */
static unsigned long x87_loads, x87_loads_inline;
/* The same pair for stores, and the same two negatives: no inline count means
   the emitted narrowing never ran, and an inline count equal to the total
   means the values and forms it must DECLINE were never stored here. */
static unsigned long x87_stores, x87_stores_inline;
static const char *current;
static void check(int pass, const char *reason) {
  checks++;
  if (!pass) {
    failures++;
    printf("FAIL %s: %s\n", current, reason);
  }
}
static X86pCpu initial(unsigned depth) {
  X86pCpu cpu;
  x86p_cpu_reset(&cpu);
  cpu.eip = BASE;
  cpu.reg[kX86pEax] = 0xabcdef98;
  cpu.reg[kX86pEdi] = BASE + 512;
  cpu.reg[kX86pEsp] = BASE + 2048;
  x86p_flags_set_explicit(&cpu.flags, X86P_CF | X86P_ZF | X86P_OF);
  for (unsigned i = 0; i < depth; i++) {
    x86p_x87_push(&cpu.x87, (long double)(i + 1) * 0.25L);
  }
  memset(guest, 0, sizeof guest);
  const double value = 1.75;
  memcpy(guest + 512, &value, sizeof value);
  return cpu;
}
/*
 * How a case's guest mapping is built, because the emitted code takes a
 * different route for each and the routes are not the same amount of code.
 *
 *   kMapPlain       contiguous, no permission table. The desktop's shape: a
 *                   bounds compare and a direct wasm access.
 *   kMapPerms | A   contiguous WITH a permission table granting A on the pages
 *                   the operand lands on. The emitted guard answers, including
 *                   its two page loads.
 *   kMapSparse | A  sparse, granting A on the operand's range. The checked
 *                   imports answer.
 *
 * kMapPerms exists because a store on the contiguous mapping proves its
 * address with x86p_wasm_state_check -- an if/else that reads the permission
 * table only in the in-bounds arm -- and nothing else in this suite reaches
 * that code with a table present. Without these cases the arm is never taken
 * and a wrong page index would never show.
 */
enum { kMapPlain = 0u, kMapPerms = 0x10u, kMapSparse = 0x20u, kMapAccess = 0x0Fu };
/* 256-byte pages over the 4 KiB fixture, so the code at offset 0, the operand
   at 512 and the stack at 2048 land on pages nothing else governs. */
enum { kTestPageShift = 8, kOperandPage = 512u >> kTestPageShift, kLastPage = (SIZE - 1u) >> kTestPageShift };
static uint8_t guest_perms[SIZE >> kTestPageShift], reference_perms[SIZE >> kTestPageShift];

static void fill_perms(uint8_t *table, unsigned access) {
  for (unsigned page = 0; page < SIZE >> kTestPageShift; page++) {
    table[page] = (uint8_t)(kX86pMemRead | kX86pMemWrite);
  }
  /* Only the pages an operand is aimed at carry the case's access, so the
     code fetch and the stack are never what the case is actually testing. */
  table[kOperandPage] = (uint8_t)access;
  table[kLastPage] = (uint8_t)access;
}

static void run_case(const char *name, const uint8_t *code, size_t size, X86pCpu cpu, unsigned mode) {
  const unsigned access = mode & kMapAccess;
  X86pCpu oracle = cpu;
  X86pMem mem = {.host = guest, .lo = BASE, .size = SIZE};
  X86pMem expected_mem = {.host = reference, .lo = BASE, .size = SIZE};
  X86pSparseMem *sparse = NULL, *expected_sparse = NULL;
  X86pStepReport report;
  X86pJitEngineStats stats;
  char reason[256] = {0};
  current = name;
  cases++;
  memcpy(guest, code, size);
  guest[size] = 0xeb;
  guest[size + 1] = 0xfe;
  memcpy(reference, guest, sizeof guest);
  if (mode & kMapPerms) {
    fill_perms(guest_perms, access);
    fill_perms(reference_perms, access);
    mem.perms = guest_perms;
    mem.page_shift = kTestPageShift;
    expected_mem.perms = reference_perms;
    expected_mem.page_shift = kTestPageShift;
  }
  if (mode & kMapSparse) {
    sparse = x86p_sparse_create();
    expected_sparse = x86p_sparse_create();
    check(sparse && expected_sparse, "sparse allocation");
    check(x86p_sparse_map(sparse, BASE, guest, 512) &&
              x86p_sparse_map_access(sparse, BASE + 512, guest + 512, SIZE - 512, access) &&
              x86p_sparse_map(expected_sparse, BASE, reference, 512) &&
              x86p_sparse_map_access(expected_sparse, BASE + 512, reference + 512, SIZE - 512, access),
          "sparse mappings");
    mem.sparse = sparse;
    expected_mem.sparse = expected_sparse;
  }
  X86pStepStatus expected = x86p_step(&oracle, &expected_mem, &report);
  if (expected == kX86pStepOk) {
    expected = x86p_step(&oracle, &expected_mem, &report);
  }
  X86pJitEngine *engine = x86p_jit_engine_create(&mem, 65536, 128, reason, sizeof reason);
  check(engine != NULL, reason);
  if (engine) {
    const X86pJitRunStatus actual = x86p_jit_engine_run(engine, &cpu, NULL, 1, reason, sizeof reason);
    check((expected == kX86pStepOk && actual == kX86pRunBudget) ||
              (expected == kX86pStepMemoryFault && actual == kX86pRunMemoryFault),
          reason[0] ? reason : "exit differs");
    faults += expected == kX86pStepMemoryFault;
    check(cpu.eip == oracle.eip, "EIP differs");
    check(memcmp(cpu.reg, oracle.reg, sizeof cpu.reg) == 0, "integer registers differ");
    check(memcmp(&cpu.flags, &oracle.flags, sizeof cpu.flags) == 0, "flags differ");
    check(memcmp(&cpu.x87, &oracle.x87, sizeof cpu.x87) == 0, "x87 physical values/tags/TOP/CW/SW differ");
    check(memcmp(guest, reference, sizeof guest) == 0, "guest memory differs");
    x86p_jit_engine_stats(engine, &stats);
    check(stats.blocks_translated == 1 && stats.blocks_entered == 1 && stats.translate_refusals == 0,
          "case did not enter exactly one translated block");
    entries += (unsigned)stats.blocks_entered;
    x87_loads += (unsigned long)stats.x87_loads_translated;
    x87_loads_inline += (unsigned long)stats.x87_loads_inline;
    x87_stores += (unsigned long)stats.x87_stores_translated;
    x87_stores_inline += (unsigned long)stats.x87_stores_inline;
    x86p_jit_engine_destroy(engine);
  }
  x86p_sparse_destroy(sparse);
  x86p_sparse_destroy(expected_sparse);
}

static void register_forms(void) {
  static const struct {
    const char *name;
    uint8_t code[2];
  } forms[] = {
      {"FLD ST1", {0xd9, 0xc1}},   {"FST ST1", {0xdd, 0xd1}},  {"FSTP ST1", {0xdd, 0xd9}},  {"FADD ST1", {0xd8, 0xc1}},
      {"FMUL ST1", {0xd8, 0xc9}},  {"FSUB ST1", {0xd8, 0xe1}}, {"FSUBR ST1", {0xd8, 0xe9}}, {"FDIV ST1", {0xd8, 0xf1}},
      {"FDIVR ST1", {0xd8, 0xf9}}, {"FADDP", {0xde, 0xc1}},    {"FMULP", {0xde, 0xc9}},     {"FSUBP", {0xde, 0xe9}},
      {"FDIVP", {0xde, 0xf9}},     {"FCOM", {0xd8, 0xd1}},     {"FCOMP", {0xd8, 0xd9}},     {"FCOMPP", {0xde, 0xd9}},
      {"FCOMI", {0xdb, 0xf1}},     {"FCOMIP", {0xdf, 0xf1}},   {"FUCOMI", {0xdb, 0xe9}},    {"FXCH", {0xd9, 0xc9}},
      {"FFREE", {0xdd, 0xc1}},     {"FABS", {0xd9, 0xe1}},     {"FCHS", {0xd9, 0xe0}},      {"FTST", {0xd9, 0xe4}},
      {"FLDZ", {0xd9, 0xee}},      {"FLD1", {0xd9, 0xe8}},     {"FLDPI", {0xd9, 0xeb}},     {"FLDL2T", {0xd9, 0xe9}},
      {"FLDL2E", {0xd9, 0xea}},    {"FLDLG2", {0xd9, 0xec}},   {"FLDLN2", {0xd9, 0xed}},    {"FNSTSW AX", {0xdf, 0xe0}},
      {"FNCLEX", {0xdb, 0xe2}},    {"FNINIT", {0xdb, 0xe3}},   {"FSQRT", {0xd9, 0xfa}},     {"FSIN", {0xd9, 0xfe}},
      {"FCOS", {0xd9, 0xff}},      {"FSINCOS", {0xd9, 0xfb}},  {"FPTAN", {0xd9, 0xf2}},     {"FPATAN", {0xd9, 0xf3}},
      {"FYL2X", {0xd9, 0xf1}},     {"FYL2XP1", {0xd9, 0xf9}},  {"F2XM1", {0xd9, 0xf0}},     {"FSCALE", {0xd9, 0xfd}},
      {"FRNDINT", {0xd9, 0xfc}},   {"FPREM", {0xd9, 0xf8}},    {"FPREM1", {0xd9, 0xf5}}};
  static const unsigned depths[] = {0, 1, 2, 8};
  for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
    for (unsigned d = 0; d < sizeof depths / sizeof *depths; d++) {
      run_case(forms[f].name, forms[f].code, 2, initial(depths[d]), kMapPlain);
    }
  }
  for (unsigned negate = 0; negate < 2; negate++) {
    for (unsigned cond = 0; cond < 4; cond++) {
      for (unsigned flags = 0; flags < 8; flags++) {
        uint8_t code[] = {(uint8_t)(0xda + negate), (uint8_t)(0xc1 + cond * 8)};
        X86pCpu cpu = initial(2);
        x86p_flags_set_explicit(&cpu.flags,
                                (flags & 1 ? X86P_CF : 0) | (flags & 2 ? X86P_ZF : 0) | (flags & 4 ? X86P_PF : 0));
        run_case("FCMOV condition", code, 2, cpu, kMapPlain);
      }
    }
  }
  {
    const uint8_t code[] = {0x9b};
    run_case("FWAIT populated stack", code, 1, initial(8), kMapPlain);
  }
}

static void comparison_sources(void) {
  static const struct {
    long double a, b;
    uint32_t expected;
  } values[] = {{1, 2, X86P_CF}, {2, 1, 0}, {2, 2, X86P_ZF}, {NAN, 1, X86P_CF | X86P_PF | X86P_ZF}};
  static const uint8_t code[] = {0xdb, 0xf1};
  for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
    X86pCpu cpu = initial(2);
    x86p_x87_set(&cpu.x87, 0, values[i].a);
    x86p_x87_set(&cpu.x87, 1, values[i].b);
    run_case("FCOMI explicit source", code, sizeof code, cpu, kMapPlain);
    /* The interpreter is independently checked against architectural outcomes,
       so a source-selection bug cannot pass through matching JIT/oracle code. */
    X86pMem mem = {.host = guest, .lo = BASE, .size = SIZE};
    X86pStepReport report;
    check(x86p_step(&cpu, &mem, &report) == kX86pStepOk, "FCOMI oracle status");
    check((x86p_eflags(&cpu.flags) & (X86P_CF | X86P_PF | X86P_ZF)) == values[i].expected,
          "FCOMI selected destination instead of source");
  }
}

static void memory_forms(void) {
  static const struct {
    const char *name;
    uint8_t code[2];
  } forms[] = {
      {"FLD32", {0xd9, 0x07}},   {"FLD64", {0xdd, 0x07}},    {"FILD16", {0xdf, 0x07}},        {"FILD32", {0xdb, 0x07}},
      {"FILD64", {0xdf, 0x2f}},  {"FST32", {0xd9, 0x17}},    {"FSTP32", {0xd9, 0x1f}},        {"FST64", {0xdd, 0x17}},
      {"FSTP64", {0xdd, 0x1f}},  {"FIST16", {0xdf, 0x17}},   {"FISTP16", {0xdf, 0x1f}},       {"FIST32", {0xdb, 0x17}},
      {"FISTP32", {0xdb, 0x1f}}, {"FISTP64", {0xdf, 0x3f}},  {"FADD32", {0xd8, 0x07}},        {"FMUL64", {0xdc, 0x0f}},
      {"FSUB32", {0xd8, 0x27}},  {"FSUBR64", {0xdc, 0x2f}},  {"FDIV32", {0xd8, 0x37}},        {"FDIVR64", {0xdc, 0x3f}},
      {"FIADD32", {0xda, 0x07}}, {"FIMUL16", {0xde, 0x0f}},  {"FCOM32", {0xd8, 0x17}},        {"FCOMP64", {0xdc, 0x1f}},
      {"FICOM16", {0xde, 0x17}}, {"FICOMP32", {0xda, 0x1f}}, {"FNSTSW memory", {0xdd, 0x3f}}, {"FLDCW", {0xd9, 0x2f}},
      {"FNSTCW", {0xd9, 0x3f}}};
  /* Every access combination on both tables, plus the table-free shape. The
     four access values are the whole lattice: neither, read, write, both. */
  static const unsigned modes[] = {kMapPlain,
                                   kMapPerms | 0u,
                                   kMapPerms | 1u,
                                   kMapPerms | 2u,
                                   kMapPerms | 3u,
                                   kMapSparse | 0u,
                                   kMapSparse | 1u,
                                   kMapSparse | 2u,
                                   kMapSparse | 3u};
  for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
    for (unsigned m = 0; m < sizeof modes / sizeof *modes; m++) {
      run_case(forms[f].name, forms[f].code, 2, initial(2), modes[m]);
    }
    /* Off both ends of the mapping, on the table-free shape and on the one
       whose guard reads the table -- an out-of-bounds offset must be refused
       before it becomes a page index. */
    X86pCpu cpu = initial(2);
    cpu.reg[kX86pEdi] = BASE + SIZE - 1;
    run_case(forms[f].name, forms[f].code, 2, cpu, kMapPlain);
    run_case(forms[f].name, forms[f].code, 2, cpu, kMapPerms | 3u);
    cpu = initial(0);
    cpu.reg[kX86pEdi] = BASE - 1;
    run_case(forms[f].name, forms[f].code, 2, cpu, kMapPlain);
    run_case(forms[f].name, forms[f].code, 2, cpu, kMapPerms | 3u);
  }
  {
    /*
     * The ordering case, and the reason the store's address verdict travels as
     * a VALUE rather than as an early return. FIST of an infinity records an
     * invalid operation in the status word, and it records it even though the
     * write never happens -- so the conversion has to run before the refusal.
     *
     * Three ways to be refused, because they refuse at three different points:
     * below the mapping entirely, off its far end, and inside it on a page the
     * table does not make writable. The last one is the only one that reaches
     * the permission arm of x86p_wasm_state_check.
     */
    const uint8_t code[] = {0xdb, 0x1f};
    X86pCpu cpu = initial(1);
    x86p_x87_set(&cpu.x87, 0, INFINITY);
    cpu.reg[kX86pEdi] = BASE - 1;
    run_case("FIST invalid sets IE before fault, preserves stack", code, 2, cpu, kMapPlain);
    run_case("FIST invalid sets IE before fault, preserves stack", code, 2, cpu, kMapPerms | 3u);
    cpu.reg[kX86pEdi] = BASE + SIZE - 1;
    run_case("FIST invalid sets IE past the far end", code, 2, cpu, kMapPerms | 3u);
    cpu.reg[kX86pEdi] = BASE + 512;
    run_case("FIST invalid sets IE on a read-only page", code, 2, cpu, kMapPerms | kX86pMemRead);
    run_case("FIST invalid sets IE on an unmapped page", code, 2, cpu, kMapPerms | 0u);
  }
}

/*
 * FLD32 and FLD64 over the operand values that decide which arm of the emitted
 * widening runs, and over the stack depths that decide whether it may run at
 * all.
 *
 * The backend emits the ordinary case itself and calls x86p_wasm_x87_load_bits
 * for the rest (jit_wasm_x87_load.h says why). So there are now two
 * implementations of one conversion, and the values below are exactly the ones
 * that tell them apart: the zeroes and subnormals whose significand has no
 * leading one to make explicit, the infinities and NaNs whose exponent means
 * something else entirely, and the two ends of the normal range where an
 * off-by-one in the rebias shows. The existing memory_forms cases reach FLD
 * with one value, 1.75, which is normal as a double and +0.0 as the float made
 * of its low four bytes -- so it exercises one arm each and neither boundary.
 *
 * DEPTH 8 IS NOT A DUPLICATE OF THE OTHERS. A full stack is a push overflow:
 * nothing is stored, three status bits are set, and TOP does not move. That is
 * the one refusal the emitted test for "ordinary" makes about the DESTINATION
 * rather than the operand, and without it a fast path that stored over a live
 * register would pass every value above.
 */
static void load_widening(void) {
  static const struct {
    const char *name;
    uint64_t f64;
    uint32_t f32;
  } operands[] = {
      {"one and a half", 0x3ff8000000000000ull, 0x3fc00000u},
      {"negative normal", 0xc008000000000000ull, 0xc0400000u},
      {"smallest normal", 0x0010000000000000ull, 0x00800000u},
      {"largest normal", 0x7fefffffffffffffull, 0x7f7fffffu},
      {"exponent one, full fraction", 0x001fffffffffffffull, 0x00ffffffu},
      {"positive zero", 0x0000000000000000ull, 0x00000000u},
      {"negative zero", 0x8000000000000000ull, 0x80000000u},
      {"smallest subnormal", 0x0000000000000001ull, 0x00000001u},
      {"largest subnormal", 0x000fffffffffffffull, 0x007fffffu},
      {"negative subnormal", 0x8000000000000001ull, 0x80000001u},
      {"infinity", 0x7ff0000000000000ull, 0x7f800000u},
      {"negative infinity", 0xfff0000000000000ull, 0xff800000u},
      {"quiet NaN", 0x7ff8000000000000ull, 0x7fc00000u},
      {"signalling NaN", 0x7ff0000000000001ull, 0x7f800001u},
  };
  static const uint8_t fld32[] = {0xd9, 0x07};
  static const uint8_t fld64[] = {0xdd, 0x07};
  /* Both mappings, because the operand reaches the widening by a different
     route on each -- a direct wasm load, or the checked import pair. */
  static const unsigned modes[] = {kMapPlain, kMapSparse | 3u};
  static const unsigned depths[] = {0, 2, 7, 8};
  for (unsigned v = 0; v < sizeof operands / sizeof *operands; v++) {
    for (unsigned d = 0; d < sizeof depths / sizeof *depths; d++) {
      for (unsigned m = 0; m < sizeof modes / sizeof *modes; m++) {
        X86pCpu cpu = initial(depths[d]);
        memcpy(guest + 512, &operands[v].f64, sizeof operands[v].f64);
        run_case(operands[v].name, fld64, sizeof fld64, cpu, modes[m]);
        cpu = initial(depths[d]);
        memcpy(guest + 512, &operands[v].f32, sizeof operands[v].f32);
        run_case(operands[v].name, fld32, sizeof fld32, cpu, modes[m]);
      }
    }
  }
}

/*
 * FST/FSTP m32 and m64 over the ST(0) values and machine states that decide
 * which arm of the emitted narrowing runs.
 *
 * The store ROUNDS, so unlike the load its arms are separated by the value's
 * low bits as well as by its class: a discarded part that is exactly half an
 * ulp goes to even, one bit either side of that does not, and a significand of
 * all ones carries out of the top into the next exponent -- which the emitted
 * arm refuses and the helper completes. jit_wasm_x87_store.h says why the two
 * sides refuse the same set.
 *
 * ROUNDING CONTROL IS PART OF THE FIXTURE and not a separate suite. Only
 * round-to-nearest is the emitted arm's; the other three must reach the helper
 * with the value unchanged, and a fast path that ignored RC would agree with
 * the interpreter on every case here except those.
 *
 * The values arrive as the guest's own ten bytes rather than as a long double,
 * so a case can name an encoding the host's float type cannot hold -- an
 * unnormal, whose stored exponent has no explicit integer bit, is a value the
 * emitted arm must refuse and no arithmetic can produce.
 */
static void store_narrowing(void) {
  static const struct {
    const char *name;
    uint16_t sign_exp;
    uint64_t signif;
  } values[] = {
      {"one", 0x3fffu, 0x8000000000000000ull},
      {"negative one and a half", 0xbfffu, 0xc000000000000000ull},
      {"f32 tie to even, down", 0x3fffu, 0x8000008000000000ull},
      {"f32 one above the tie", 0x3fffu, 0x8000008000000001ull},
      {"f32 one below the tie", 0x3fffu, 0x8000007fffffffffull},
      {"f32 tie to even, up", 0x3fffu, 0x8000018000000000ull},
      {"f64 tie to even, down", 0x3fffu, 0x8000000000000400ull},
      {"f64 one above the tie", 0x3fffu, 0x8000000000000401ull},
      {"f64 tie to even, up", 0x3fffu, 0x8000000000000c00ull},
      {"carries out of the significand", 0x3fffu, 0xffffffffffffffffull},
      {"positive zero", 0x0000u, 0x0000000000000000ull},
      {"negative zero", 0x8000u, 0x0000000000000000ull},
      {"ext80 subnormal", 0x0000u, 0x0000000000000001ull},
      {"infinity", 0x7fffu, 0x8000000000000000ull},
      {"quiet NaN", 0x7fffu, 0xc000000000000000ull},
      {"unnormal", 0x3fffu, 0x4000000000000000ull},
      {"below binary32's normals", 0x3f80u, 0x8000000000000000ull},
      {"binary32's smallest normal", 0x3f81u, 0x8000000000000000ull},
      {"binary32's largest normal", 0x407eu, 0xffffff0000000000ull},
      {"rounds up out of binary32", 0x407eu, 0xffffffffffffffffull},
      /* The exponent exactly one above the target's largest normal, which
         rebiases to its all-ones exponent -- the infinity encoding. It must be
         REFUSED, and it is the only value that separates a bound of exp_max-1
         from one of exp_max: without it, an emitted test that admitted this
         and stored an infinity passed every other case here. */
      {"rebiases onto binary32's infinity", 0x407fu, 0x8000000000000000ull},
      /* And the same exponent with a fraction, which is the case that actually
         separates the two bounds: with an empty fraction both a refusal and a
         wrongly-taken inline arm produce the infinity that the overflowing
         conversion rounds to, and only a fraction makes the wrong arm produce
         a NaN instead. */
      {"rebiases onto binary32's infinity, with a fraction", 0x407fu, 0xffffff0000000000ull},
      {"rebiases onto binary64's infinity, with a fraction", 0x43ffu, 0xfffffffffffff800ull},
      {"above binary32's normals", 0x4080u, 0x8000000000000000ull},
      {"rebiases onto binary64's infinity", 0x43ffu, 0x8000000000000000ull},
      {"binary64's smallest normal", 0x3c01u, 0x8000000000000000ull},
      {"rounds up out of binary64", 0x43feu, 0xffffffffffffffffull},
  };
  static const struct {
    const char *name;
    uint8_t code[2];
  } forms[] = {{"FST32", {0xd9, 0x17}}, {"FSTP32", {0xd9, 0x1f}}, {"FST64", {0xdd, 0x17}}, {"FSTP64", {0xdd, 0x1f}}};
  /* Both mappings and the permission arm, because the emitted narrowing runs
     only on the contiguous one and its address verdict is a value it must
     honour: a page the table does not make writable has to reach the helper
     and fault exactly as it did before. */
  static const unsigned modes[] = {kMapPlain, kMapPerms | 3u, kMapPerms | kX86pMemRead, kMapSparse | 3u};
  for (unsigned v = 0; v < sizeof values / sizeof *values; v++) {
    for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
      for (unsigned m = 0; m < sizeof modes / sizeof *modes; m++) {
        for (unsigned rc = 0; rc < 4; rc++) {
          uint8_t bytes[10];
          X86pCpu cpu = initial(2);
          for (unsigned b = 0; b < 8; b++) {
            bytes[b] = (uint8_t)(values[v].signif >> (8u * b));
          }
          bytes[8] = (uint8_t)values[v].sign_exp;
          bytes[9] = (uint8_t)(values[v].sign_exp >> 8);
          cpu.x87.control = (uint16_t)(0x7f | rc << 10);
          x86p_x87_set_raw(&cpu.x87, 0, x86p_x87_reg_from_f80(bytes));
          run_case(values[v].name, forms[f].code, 2, cpu, modes[m]);
        }
      }
    }
  }
  /* An empty stack: the store must not happen, three status bits are set and
     TOP does not move. That is the emitted arm's one refusal about the
     MACHINE rather than the value. */
  for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
    run_case("store from an empty stack", forms[f].code, 2, initial(0), kMapPlain);
  }
}

/* An ext80 value by its two fields, for values a long double cannot name. */
typedef struct Ext80Value {
  const char *name;
  uint16_t sign_exp;
  uint64_t signif;
} Ext80Value;

static void set_ext80(X86pCpu *cpu, int i, Ext80Value v) {
  uint8_t bytes[10];
  for (unsigned b = 0; b < 8; b++) {
    bytes[b] = (uint8_t)(v.signif >> (8u * b));
  }
  bytes[8] = (uint8_t)v.sign_exp;
  bytes[9] = (uint8_t)(v.sign_exp >> 8);
  check(x86p_x87_set_raw(&cpu->x87, i, x86p_x87_reg_from_f80(bytes)), "fixture register write");
}

static X86pCpu binary64_machine(void) {
  X86pCpu cpu = initial(2);
  check(x86p_x87_set_double_arith(&cpu.x87, 1), "binary64 arithmetic is available on this host");
  return cpu;
}

/*
 * FADD/FSUB/FMUL/FDIV with binary64 arithmetic selected, which the backend
 * computes in the block (jit_wasm_x87_arith.h) against the interpreter, which
 * answers through x86p_ext80_double_arith. The values separate each refusal
 * from the arm: the two ends of the exponents the emitted narrowing takes and
 * one past each, the ties its rounding must send to even, the classes it
 * leaves to the helper, results that leave the normals, and the control words
 * and census that the mode does not answer under.
 */
static void binary64_arithmetic(void) {
  static const Ext80Value values[] = {
      {"one", 0x3fffu, 0x8000000000000000ull},
      {"negative one and a half", 0xbfffu, 0xc000000000000000ull},
      {"tie to even, down", 0x3fffu, 0x8000000000000400ull},
      {"tie to even, up", 0x3fffu, 0x8000000000000c00ull},
      {"one above the tie", 0x3fffu, 0x8000000000000401ull},
      {"carries into the exponent", 0x3fffu, 0xffffffffffffffffull},
      {"positive zero", 0x0000u, 0x0000000000000000ull},
      {"negative zero", 0x8000u, 0x0000000000000000ull},
      {"lowest narrowed exponent", 0x3fffu - 959u, 0x8000000000000001ull},
      {"one below it", 0x3fffu - 960u, 0x8000000000000001ull},
      {"highest narrowed exponent", 0x3fffu + 1022u, 0xfffffffffffff800ull},
      {"one above it", 0x3fffu + 1023u, 0x8000000000000000ull},
      {"rounds up out of binary64", 0x3fffu + 1023u, 0xffffffffffffffffull},
      {"infinity", 0x7fffu, 0x8000000000000000ull},
      {"quiet NaN", 0x7fffu, 0xc000000000000000ull},
      {"unnormal", 0x3fffu, 0x4000000000000000ull},
      {"ext80 subnormal", 0x0000u, 0x0000000000000001ull},
  };
  static const Ext80Value partners[] = {
      {"three", 0x4000u, 0xc000000000000000ull},
      {"huge", 0x3fffu + 1000u, 0x8000000000000000ull},
      {"tiny", 0x3fffu - 950u, 0x8000000000000000ull},
      {"zero", 0x0000u, 0x0000000000000000ull},
  };
  static const struct {
    const char *name;
    uint8_t code[2];
  } registers[] = {{"FADD ST1", {0xd8, 0xc1}},
                   {"FSUBR ST1", {0xd8, 0xe9}},
                   {"FMUL ST1,ST0", {0xdc, 0xc9}},
                   {"FDIV ST1", {0xd8, 0xf1}},
                   {"FDIVP", {0xde, 0xf9}},
                   {"FSUBP", {0xde, 0xe9}},
                   {"FADD ST0,ST0", {0xd8, 0xc0}}};
  static const struct {
    const char *name;
    uint8_t code[2];
  } memories[] = {{"FADD32", {0xd8, 0x07}},
                  {"FMUL64", {0xdc, 0x0f}},
                  {"FSUBR64", {0xdc, 0x2f}},
                  {"FDIV32", {0xd8, 0x37}},
                  {"FIADD32", {0xda, 0x07}}};
  static const struct {
    uint64_t f64;
    uint32_t f32;
  } operands[] = {{0x3ffc000000000000ull, 0x3fe00000u},
                  {0x0000000000000000ull, 0x00000000u},
                  {0x8000000000000000ull, 0x80000000u},
                  {0x0000000000000001ull, 0x00000001u},
                  {0x7ff0000000000000ull, 0x7f800000u},
                  {0x7ff8000000000000ull, 0x7fc00000u},
                  {0x7fefffffffffffffull, 0x7f7fffffu},
                  {0x0010000000000000ull, 0x00800000u}};
  for (unsigned v = 0; v < sizeof values / sizeof *values; v++) {
    for (unsigned p = 0; p < sizeof partners / sizeof *partners; p++) {
      for (unsigned f = 0; f < sizeof registers / sizeof *registers; f++) {
        for (unsigned swap = 0; swap < 2; swap++) {
          X86pCpu cpu = binary64_machine();
          set_ext80(&cpu, 0, swap ? partners[p] : values[v]);
          set_ext80(&cpu, 1, swap ? values[v] : partners[p]);
          run_case(values[v].name, registers[f].code, 2, cpu, kMapPlain);
        }
      }
      for (unsigned f = 0; f < sizeof memories / sizeof *memories; f++) {
        X86pCpu cpu = binary64_machine();
        set_ext80(&cpu, 0, values[v]);
        run_case(values[v].name, memories[f].code, 2, cpu, kMapPlain);
      }
    }
  }
  for (unsigned o = 0; o < sizeof operands / sizeof *operands; o++) {
    for (unsigned f = 0; f < sizeof memories / sizeof *memories; f++) {
      static const unsigned modes[] = {kMapPlain, kMapSparse | 3u, kMapPerms | 0u};
      for (unsigned m = 0; m < sizeof modes / sizeof *modes; m++) {
        X86pCpu cpu = binary64_machine();
        memcpy(guest + 512, memories[f].code[0] == 0xdc ? (const void *)&operands[o].f64 : &operands[o].f32, 8);
        set_ext80(&cpu, 0, values[1]);
        run_case("binary64 memory operand", memories[f].code, 2, cpu, modes[m]);
      }
    }
  }
  /* The control words the mode does not answer under, and an empty stack. */
  static const uint16_t controls[] = {0x37f, 0x27f, 0x07f, 0x77f, 0xb7f, 0xf7f};
  for (unsigned c = 0; c < sizeof controls / sizeof *controls; c++) {
    for (unsigned f = 0; f < sizeof registers / sizeof *registers; f++) {
      X86pCpu cpu = binary64_machine();
      cpu.x87.control = controls[c];
      set_ext80(&cpu, 0, values[2]);
      set_ext80(&cpu, 1, values[4]);
      run_case("binary64 control word", registers[f].code, 2, cpu, kMapPlain);
    }
  }
  for (unsigned f = 0; f < sizeof registers / sizeof *registers; f++) {
    X86pCpu cpu = binary64_machine();
    x86p_x87_reset(&cpu.x87);
    check(x86p_x87_set_double_arith(&cpu.x87, 1), "binary64 arithmetic after reset");
    run_case("binary64 empty stack", registers[f].code, 2, cpu, kMapPlain);
  }
  /* An armed census counts every operation, so the helper must answer: the
     oracle and the block each count one. */
  {
    X86pX87OpCensus census;
    memset(&census, 0, sizeof census);
    X86pCpu cpu = binary64_machine();
    x86p_x87_set_op_census(&cpu.x87, &census);
    run_case("binary64 with the census armed", registers[0].code, 2, cpu, kMapPlain);
    uint64_t counted = 0;
    for (unsigned i = 0; i < 4; i++) {
      counted += census.by_precision[i];
    }
    check(counted == 2u, "an operation under an armed census went uncounted");
  }
}

static void precision_and_refusals(void) {
  static const uint16_t precision[] = {0, 0x200, 0x300};
  static const uint8_t forms[][2] = {
      {0xd8, 0xc1}, {0xde, 0xf9}, {0xd9, 0xfa}, {0xd9, 0xfc}, {0xd9, 0x1f}, {0xdd, 0x1f}};
  for (unsigned pc = 0; pc < 3; pc++) {
    for (unsigned rc = 0; rc < 4; rc++) {
      for (unsigned f = 0; f < sizeof forms / sizeof *forms; f++) {
        X86pCpu cpu = initial(2);
        cpu.x87.control = (uint16_t)(0x7f | precision[pc] | rc << 10);
        x86p_x87_set(&cpu.x87, 0, 0x1.0000000000000002p0L);
        x86p_x87_set(&cpu.x87, 1, 0x1p-1000L);
        run_case("precision/rounding through emitted helpers", forms[f], 2, cpu, kMapPlain);
      }
    }
  }
  static const uint8_t rejected[][3] = {
      {0xdb, 0x2f, 0}, {0xdb, 0x3f, 0}, {0xdd, 0x27, 0}, {0xdd, 0x37, 0}, {0xd9, 0xf4, 0}, {0x67, 0xd9, 0x07}};
  current = "raw f80/state/FXTRACT/addr16 refusal";
  for (unsigned i = 0; i < sizeof rejected / sizeof *rejected; i++) {
    X86pInsn insn;
    check(x86p_decode(rejected[i], 3, &insn) != 0, "invalid refusal fixture");
    const int refused = !x86p_wasm_can_lower(&insn);
    check(refused, "unsupported state/shape admitted");
    refusals += (unsigned)refused;
  }
}
int main(void) {
  register_forms();
  comparison_sources();
  memory_forms();
  load_widening();
  store_narrowing();
  precision_and_refusals();
  binary64_arithmetic();
  current = "denominators";
  check(cases == entries && entries > 400, "missing translated cases");
  check(faults > 50 && refusals == 6, "negative classes were not exercised");
  /* The emitted widening was reached, and so was the decline: the integer
     loads in memory_forms are lowered here and must NOT get it. Either
     equality would mean this suite proved nothing about which path ran. */
  check(x87_loads_inline > 0, "no x87 load was lowered to the emitted widening");
  check(x87_loads_inline < x87_loads, "every x87 load was inlined, so the declined forms went untested");
  check(x87_stores_inline > 0, "no x87 store was lowered to the emitted narrowing");
  check(x87_stores_inline < x87_stores, "every x87 store was inlined, so the declined forms went untested");
  printf("WASM x87: cases=%u entries=%u faults=%u refusals=%u loads=%lu inline=%lu stores=%lu inline=%lu "
         "checks=%u failures=%u\n",
         cases,
         entries,
         faults,
         refusals,
         x87_loads,
         x87_loads_inline,
         x87_stores,
         x87_stores_inline,
         checks,
         failures);
  return failures ? 1 : 0;
}
