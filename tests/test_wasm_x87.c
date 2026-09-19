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
  precision_and_refusals();
  current = "denominators";
  check(cases == entries && entries > 400, "missing translated cases");
  check(faults > 50 && refusals == 6, "negative classes were not exercised");
  printf("WASM x87: cases=%u entries=%u faults=%u refusals=%u checks=%u failures=%u\n",
         cases,
         entries,
         faults,
         refusals,
         checks,
         failures);
  return failures ? 1 : 0;
}
