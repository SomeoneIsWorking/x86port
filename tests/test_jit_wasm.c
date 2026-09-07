/*
 * test_jit_wasm.c -- the WebAssembly lowering, differentially against the
 * interpreter oracle, in a real engine.
 *
 * WHAT THIS ESTABLISHES. For each case: a guest basic block is lowered to a
 * WebAssembly module, the module is instantiated and RUN by node with an
 * X86pCpu and a guest arena laid out in its linear memory, and the whole
 * resulting machine -- every register, the guest EIP, every field of the lazy
 * flag state, and every byte of guest memory -- is compared against what the
 * separately linked interpreter produced from the same starting state. A
 * matching final EAX proves very little; a matching machine proves the block.
 *
 * WITHOUT NODE THIS TEST SKIPS (77) RATHER THAN PASSING. A run that
 * instantiated nothing has established nothing about the lowering, and a green
 * tick for it would be the most expensive kind of wrong.
 *
 * THE IMPORTED HELPERS ARE RECORDED, NOT REIMPLEMENTED. A lowered block calls
 * x86p_alu, x86p_alu_unary, x86p_cond or x86p_flag_cf -- C functions node
 * cannot reach. So this test calls the REAL function on the state the block
 * starts from and hands the oracle what it returned and which bytes it wrote,
 * and the oracle replays exactly that. Two rules keep it honest:
 *
 *   - a case contains AT MOST ONE helper-using instruction and it is the
 *     FIRST, so the state the helper sees is the state the case set up. The
 *     driver decodes every instruction in the block and FAILS the case if that
 *     is not true, rather than trusting the case table;
 *   - the one exception is x86p_flag_cf, whose call count comes from the
 *     backend's own published counter and whose answer is the CF of the
 *     initial flag state -- the first flag write in a block is by definition
 *     preceded by no other, so that state IS what it reads.
 *
 * The arguments the block actually passed are recorded and compared too: a
 * lowering that pushed them in the wrong order would otherwise pass whenever
 * the recorded return value happened to be right.
 */
#include "alu.h"
#include "cond.h"
#include "cpu.h"
#include "decode.h"
#include "exec.h"
#include "flags.h"
#include "jit_wasm_lower.h"
#include "jit_wasm_module.h"
#include "jit_x64.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define x86p_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define x86p_mkdir(path) mkdir(path, 0777)
#endif

#define CASE_DIR "wasm_jit_cases"
#define MODULE_CAP 8192

/* The guest picture, and where it sits in the engine's linear memory. */
#define GUEST_LO 0x00400000u
#define ARENA_SIZE 0x1000u
#define CODE_OFF 0x100u
#define STACK_OFF 0x800u
#define CPU_AT 0x100u
#define ARENA_AT 0x1000u
#define IMAGE_BYTES 0x2000u
#define IMAGE_PAGES 1u

static int g_checks;
static int g_failed;
static int g_ran;   /* cases that reached the engine and were compared */
static int g_cases; /* cases attempted */

static void fail(const char *name, const char *what, const char *detail) {
  g_failed++;
  printf("FAIL %s: %s%s%s\n", name, what, detail ? " -- " : "", detail ? detail : "");
}

static void check_u32(const char *name, const char *what, uint32_t got, uint32_t want) {
  g_checks++;
  if (got != want) {
    char detail[128];
    snprintf(detail, sizeof detail, "got %08X, want %08X", got, want);
    fail(name, what, detail);
  }
}

/* ---- the cases ----------------------------------------------------------- */

typedef struct Case {
  const char *name;
  uint8_t code[16];
  uint8_t code_len;
  /* Append `EB 00` -- a JMP to the next instruction -- so the block ends where
     the case's own instructions do. Without a terminator the block loop would
     read on into the zeroed arena and decode whatever is there. */
  int terminate;
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  int carry_in; /* start with CF set, through a real EFLAGS word */
  /*
   * Start with CF set by a SUB that BORROWED, which is the only shape in which
   * the carry_in byte and the derived CF disagree: the byte still holds the
   * previous operation's carry (0) while x86p_flag_cf answers 1. That is what
   * makes storing carry_in before a bounds check observable -- with an
   * explicit EFLAGS word the two agree, and the ordering bug hides.
   */
  int borrow;
  uint32_t fs_base; /* the one segment with a base that a guest really uses */
  X86pJitExit expect_exit;
} Case;

#define STACK (GUEST_LO + STACK_OFF)
#define DATA (GUEST_LO + 0x200u)

static const Case kCases[] = {
    {"mov_imm", {0xB8, 0x78, 0x56, 0x34, 0x12}, 5, 1, .expect_exit = kX86pJitExitBlockEnd},
    /* MOV ECX,EAX ; MOV [EBX+4],EAX ; MOV EDX,[EBX+4] */
    {"mov_reg_and_memory",
     {0x89, 0xC1, 0x89, 0x43, 0x04, 0x8B, 0x53, 0x04},
     8,
     1,
     .eax = 0xDEADBEEF,
     .ebx = DATA,
     .expect_exit = kX86pJitExitBlockEnd},
    /* MOV AL,BL ; MOV CX,AX -- the narrow widths, whose partial writes must
       preserve the rest of the register. */
    {"mov_narrow_widths",
     {0x88, 0xD8, 0x66, 0x89, 0xC1},
     5,
     1,
     .eax = 0x11223344,
     .ecx = 0x55667788,
     .ebx = 0x99AABBCC,
     .expect_exit = kX86pJitExitBlockEnd},
    /* ADD EAX,EBX ; SUB EAX,ECX ; XOR EAX,0x0F ; CMP EAX,EBX */
    {"alu_inline_chain",
     {0x01, 0xD8, 0x29, 0xC8, 0x83, 0xF0, 0x0F, 0x39, 0xD8},
     9,
     1,
     .eax = 0x00001000,
     .ecx = 0x00000100,
     .ebx = 0x00000234,
     .carry_in = 1,
     .expect_exit = kX86pJitExitBlockEnd},
    /* ADD AL,BL across a byte boundary that carries. */
    {"alu_byte", {0x00, 0xD8}, 2, 1, .eax = 0x1122337F, .ebx = 0x44556601, .expect_exit = kX86pJitExitBlockEnd},
    /* ADD AX,BX, whose result overflows sixteen bits. */
    {"alu_word", {0x66, 0x01, 0xD8}, 3, 1, .eax = 0x11118000, .ebx = 0x22228000, .expect_exit = kX86pJitExitBlockEnd},
    /* ADD [EBX],EAX -- the destination is memory, so the flag tuple must not
       be stored until the bounds check has passed. */
    {"alu_memory_dest", {0x01, 0x03}, 2, 1, .eax = 5, .ebx = DATA, .expect_exit = kX86pJitExitBlockEnd},
    /* ADD EAX,[EBX] */
    {"alu_memory_src", {0x03, 0x03}, 2, 1, .eax = 5, .ebx = DATA, .expect_exit = kX86pJitExitBlockEnd},
    /* NOT EAX writes no flags at all, which is why it is inlined. */
    {"not_inline", {0xF7, 0xD0}, 2, 1, .eax = 0x0F0F0F0F, .carry_in = 1, .expect_exit = kX86pJitExitBlockEnd},
    {"neg_helper", {0xF7, 0xD8}, 2, 1, .eax = 0x00000007, .expect_exit = kX86pJitExitBlockEnd},
    {"inc_helper", {0x40}, 1, 1, .eax = 0x7FFFFFFF, .carry_in = 1, .expect_exit = kX86pJitExitBlockEnd},
    {"dec_helper", {0x48}, 1, 1, .eax = 0x00000000, .carry_in = 1, .expect_exit = kX86pJitExitBlockEnd},
    /* ADC EAX,EBX with CF set: the case the lazy triple cannot express, and
       the reason ADC calls the semantic owner. */
    {"adc_helper",
     {0x11, 0xD8},
     2,
     1,
     .eax = 0x00000010,
     .ebx = 0x00000020,
     .carry_in = 1,
     .expect_exit = kX86pJitExitBlockEnd},
    /* SHL EAX,3 */
    {"shl_helper", {0xC1, 0xE0, 0x03}, 3, 1, .eax = 0x12345678, .expect_exit = kX86pJitExitBlockEnd},
    /* SETE AL, reading the flag state this case set up. */
    {"setcc_helper", {0x0F, 0x94, 0xC0}, 3, 1, .eax = 0x11223344, .expect_exit = kX86pJitExitBlockEnd},
    /* JE +5: the conditional branch, which ends the block at one address or
       the other. */
    {"jcc_helper", {0x74, 0x05}, 2, 0, .expect_exit = kX86pJitExitBlockEnd},
    /* JECXZ +5 with ECX zero, and again with ECX set. */
    {"jecxz_taken", {0xE3, 0x05}, 2, 0, .ecx = 0, .expect_exit = kX86pJitExitBlockEnd},
    {"jecxz_not_taken", {0xE3, 0x05}, 2, 0, .ecx = 1, .expect_exit = kX86pJitExitBlockEnd},
    /* PUSH EAX ; POP EBX */
    {"push_pop", {0x50, 0x5B}, 2, 1, .eax = 0xCAFEF00D, .esp = STACK, .expect_exit = kX86pJitExitBlockEnd},
    /* PUSH imm32 */
    {"push_imm", {0x68, 0x21, 0x43, 0x65, 0x87}, 5, 1, .esp = STACK, .expect_exit = kX86pJitExitBlockEnd},
    /* LEA ECX,[EBX+EAX+8] -- address arithmetic with no access at all. */
    {"lea", {0x8D, 0x4C, 0x03, 0x08}, 4, 1, .eax = 0x10, .ebx = 0x20, .expect_exit = kX86pJitExitBlockEnd},
    /* MOVZX EAX,BL ; MOVSX ECX,BL -- the same byte, widened both ways. */
    {"movzx_movsx", {0x0F, 0xB6, 0xC3, 0x0F, 0xBE, 0xCB}, 6, 1, .ebx = 0x000000F0, .expect_exit = kX86pJitExitBlockEnd},
    {"xchg", {0x87, 0xC3}, 2, 1, .eax = 0x11111111, .ebx = 0x22222222, .expect_exit = kX86pJitExitBlockEnd},
    {"cdq", {0x99}, 1, 1, .eax = 0x80000000, .expect_exit = kX86pJitExitBlockEnd},
    {"leave", {0xC9}, 1, 1, .esp = STACK, .ebp = GUEST_LO + 0x700u, .expect_exit = kX86pJitExitBlockEnd},
    {"ret", {0xC3}, 1, 0, .esp = STACK, .expect_exit = kX86pJitExitBlockEnd},
    {"ret_imm", {0xC2, 0x08, 0x00}, 3, 0, .esp = STACK, .expect_exit = kX86pJitExitBlockEnd},
    {"call_rel", {0xE8, 0x00, 0x00, 0x00, 0x00}, 5, 0, .esp = STACK, .expect_exit = kX86pJitExitBlockEnd},
    /* JMP EAX */
    {"jmp_indirect", {0xFF, 0xE0}, 2, 0, .eax = GUEST_LO + 0x300u, .expect_exit = kX86pJitExitBlockEnd},
    {"cld_std", {0xFC, 0xFD}, 2, 1, .expect_exit = kX86pJitExitBlockEnd},
    /*
     * FS IS THE ONE SEGMENT A 32-BIT WIN32 GUEST REALLY USES -- it points at
     * the thread environment block -- so an access through it is worth a case,
     * and it is the case that proves the segment base is added at all.
     *
     * The LEA beside it does NOT prove that LEA ignores the segment base: this
     * decoder leaves the operand's segment at DS for `64 8D 4B 08`, so there
     * is no base to leave out. It is here because an LEA carrying a prefix
     * must still lower and still agree with the interpreter, which is a
     * smaller claim than the one the shape of the code suggests.
     */
    {"mov_fs_segment",
     {0x64, 0x8B, 0x03},
     3,
     1,
     .ebx = 0x100,
     .fs_base = GUEST_LO + 0x200u,
     .expect_exit = kX86pJitExitBlockEnd},
    {"lea_fs_segment",
     {0x64, 0x8D, 0x4B, 0x08},
     4,
     1,
     .ebx = 0x100,
     .fs_base = GUEST_LO + 0x200u,
     .expect_exit = kX86pJitExitBlockEnd},
    /* MOV EAX,[EBX] with EBX outside the mapping: the bounds check must refuse
       it, leave EIP ON the instruction, and leave everything else alone. */
    {"memory_fault", {0x8B, 0x03}, 2, 1, .ebx = 0x00500000u, .expect_exit = kX86pJitExitMemoryFault},
    /*
     * THE FOUR EDGES OF THE BOUNDS CHECK. One unsigned compare against
     * size - w has to get all of them right, and each is an off-by-one that
     * leaves every other case passing:
     *
     *   - the last address a dword FITS at is in bounds. `>=` instead of `>`
     *     refuses it, and nothing further from the edge notices;
     *   - a dword that STARTS inside and runs one byte off the end is refused
     *     rather than truncated;
     *   - the last address a BYTE fits at is in bounds, which a check
     *     hard-coded to width four would refuse;
     *   - an address BELOW the mapping is refused, which is what makes the
     *     single unsigned compare cover both ends -- two signed comparisons
     *     are the classic way to let a negative offset through.
     */
    {"memory_last_dword", {0x8B, 0x03}, 2, 1, .ebx = GUEST_LO + ARENA_SIZE - 4u, .expect_exit = kX86pJitExitBlockEnd},
    {"memory_straddles_end",
     {0x8B, 0x03},
     2,
     1,
     .ebx = GUEST_LO + ARENA_SIZE - 3u,
     .expect_exit = kX86pJitExitMemoryFault},
    /* MOV AL,[EBX] at the very last byte of the mapping. */
    {"memory_last_byte",
     {0x8A, 0x03},
     2,
     1,
     .eax = 0x11223344,
     .ebx = GUEST_LO + ARENA_SIZE - 1u,
     .expect_exit = kX86pJitExitBlockEnd},
    {"memory_below_mapping", {0x8B, 0x03}, 2, 1, .ebx = GUEST_LO - 4u, .expect_exit = kX86pJitExitMemoryFault},
    /*
     * ADD [EBX],EAX that FAULTS, with the flag state arranged so the carry_in
     * byte disagrees with the derived CF. A refused access must leave EVERY
     * flag field exactly as it was, and the carry-in is computed before the
     * bounds check -- so a lowering that stored it before the check instead of
     * after leaves the flags half-updated. That divergence surfaces several
     * instructions later, when something finally reads CF, and no case with an
     * in-bounds access can see it.
     */
    {"alu_memory_dest_faults",
     {0x01, 0x03},
     2,
     1,
     .eax = 0x00000005,
     .ebx = 0x00500000u,
     .borrow = 1,
     .expect_exit = kX86pJitExitMemoryFault},
};

#define CASE_COUNT ((int)(sizeof kCases / sizeof kCases[0]))

/* ---- which helper an instruction needs ---------------------------------- */

typedef enum Helper { kHelperNone = 0, kHelperAlu, kHelperAluUnary, kHelperCond } Helper;

static const char *helper_field(Helper h) {
  switch (h) {
  case kHelperAlu:
    return "alu";
  case kHelperAluUnary:
    return "alu_unary";
  case kHelperCond:
    return "cond";
  case kHelperNone:
  default:
    return "";
  }
}

/*
 * Which imported helper this instruction's lowering calls.
 *
 * The shift and rotate ops plus ADC and SBB are the ALU operations whose flag
 * rules are not a lazy tuple -- a fact about x86, stated in flags.h -- so they
 * are the ones that call the semantic owner. NOT is the unary operation that
 * writes no flags, so it is the one that does not.
 */
static Helper needs_helper(const X86pInsn *insn) {
  if (insn->op == (uint8_t)kX86pInsnAlu) {
    if (insn->alu == (uint8_t)kX86pAluAdc || insn->alu == (uint8_t)kX86pAluSbb ||
        (insn->alu >= (uint8_t)kX86pAluShl && insn->alu <= (uint8_t)kX86pAluRcr)) {
      return kHelperAlu;
    }
    return kHelperNone;
  }
  if (insn->op == (uint8_t)kX86pInsnAluUnary) {
    return insn->alu == (uint8_t)kX86pAluNot ? kHelperNone : kHelperAluUnary;
  }
  if (insn->op == (uint8_t)kX86pInsnSetcc || insn->op == (uint8_t)kX86pInsnJcc) {
    return kHelperCond;
  }
  return kHelperNone;
}

/* ---- one recorded helper call ------------------------------------------- */

#define MAX_HELPER_ARGS 5

typedef struct Recording {
  Helper which;
  uint32_t arg[MAX_HELPER_ARGS];
  unsigned argc;
  uint32_t result;
  int writes_flags;
  X86pFlags after; /* the flag state the real helper left behind */
} Recording;

/* ---- the oracle --------------------------------------------------------- */

static FILE *open_pipe(const char *command) {
#if defined(_WIN32)
  return _popen(command, "r");
#else
  return popen(command, "r");
#endif
}

static int close_pipe(FILE *pipe) {
#if defined(_WIN32)
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

static int engine_present(const char *node) {
  char command[512];
  FILE *pipe;
  char line[256];
  int have_output;
  snprintf(command, sizeof command, "\"%s\" --version 2>&1", node);
  pipe = open_pipe(command);
  if (!pipe) {
    return 0;
  }
  have_output = fgets(line, sizeof line, pipe) != NULL && line[0] == 'v';
  return close_pipe(pipe) == 0 && have_output;
}

static int write_file(const char *path, const void *bytes, size_t length) {
  FILE *file = fopen(path, "wb");
  if (!file) {
    return 0;
  }
  if (fwrite(bytes, 1, length, file) != length) {
    fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

static int read_file(const char *path, void *bytes, size_t length) {
  FILE *file = fopen(path, "rb");
  size_t got;
  if (!file) {
    return 0;
  }
  got = fread(bytes, 1, length, file);
  fclose(file);
  return got == length;
}

static void write_hex(FILE *out, const void *bytes, size_t length) {
  const uint8_t *p = (const uint8_t *)bytes;
  size_t i;
  for (i = 0; i < length; i++) {
    fprintf(out, "%02x", p[i]);
  }
}

/* One helper's replay script: what to return, and the bytes the real function
   wrote, at the address the flag state lives at in the image. */
static void write_helper(FILE *out, const char *field, const Recording *r, int last) {
  fprintf(out, "    \"%s\": {\"return\": %d, \"writes\": [", field, (int)r->result);
  if (r->writes_flags) {
    fprintf(out, "{\"at\": %u, \"hex\": \"", (unsigned)(CPU_AT + offsetof(X86pCpu, flags)));
    write_hex(out, &r->after, sizeof r->after);
    fprintf(out, "\"}");
  }
  fprintf(out, "]}%s\n", last ? "" : ",");
}

/* ---- one case ------------------------------------------------------------ */

static uint8_t g_arena[ARENA_SIZE];
static uint8_t g_ref_arena[ARENA_SIZE];
static uint8_t g_image[IMAGE_BYTES];
static uint8_t g_result[IMAGE_BYTES];
static uint8_t g_module[MODULE_CAP];

static void seed_cpu(const Case *c, X86pCpu *cpu) {
  x86p_cpu_reset(cpu);
  cpu->eip = GUEST_LO + CODE_OFF;
  x86p_reg_write(cpu, kX86pEax, 4, c->eax);
  x86p_reg_write(cpu, kX86pEcx, 4, c->ecx);
  x86p_reg_write(cpu, kX86pEdx, 4, c->edx);
  x86p_reg_write(cpu, kX86pEbx, 4, c->ebx);
  x86p_reg_write(cpu, kX86pEsp, 4, c->esp ? c->esp : STACK);
  x86p_reg_write(cpu, kX86pEbp, 4, c->ebp);
  x86p_reg_write(cpu, kX86pEsi, 4, c->esi);
  x86p_reg_write(cpu, kX86pEdi, 4, c->edi);
  if (c->carry_in) {
    /* Through a real EFLAGS word, so the state is one POPFD could have left
       and x86p_flag_cf answers 1 for it without this test asserting how. */
    x86p_flags_set_explicit(&cpu->flags, X86P_EFLAGS_FIXED | X86P_CF);
  }
  cpu->fs_base = c->fs_base;
  if (c->borrow) {
    x86p_flags_set(&cpu->flags, kX86pFlagsSub, 0u, 1u, 0xFFFFFFFFu, 4);
  }
}

static uint32_t code_length(const Case *c) {
  return (uint32_t)c->code_len + (c->terminate ? 2u : 0u);
}

static void seed_arena(const Case *c) {
  memset(g_arena, 0, sizeof g_arena);
  memcpy(g_arena + CODE_OFF, c->code, c->code_len);
  if (c->terminate) {
    /* JMP +0: ends the block at the case's own last instruction. */
    g_arena[CODE_OFF + c->code_len] = 0xEB;
    g_arena[CODE_OFF + c->code_len + 1u] = 0x00;
  }
}

/*
 * Walk the block's instructions and record the ONE helper call the lowering
 * will make, refusing the case if it would make more than one or if the one it
 * makes is not the first instruction. The invariant is CHECKED here rather
 * than assumed of the case table, because a case that quietly grew a second
 * helper-using instruction would otherwise be compared against a replay that
 * answered only the first.
 */
static int record_helper(const Case *c, const X86pCpu *cpu, uint32_t insns, Recording *out) {
  uint32_t pc = GUEST_LO + CODE_OFF;
  uint32_t i;
  int found = -1;
  X86pMem mem;
  mem.host = g_arena;
  mem.lo = GUEST_LO;
  mem.size = ARENA_SIZE;
  memset(out, 0, sizeof *out);

  for (i = 0; i < insns; i++) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    X86pInsn insn;
    uint32_t avail = 0;
    uint32_t k;
    Helper which;
    for (k = 0; k < (uint32_t)X86P_MAX_INSN_LEN; k++) {
      uint32_t byte;
      if (!x86p_mem_read(&mem, pc + k, 1, &byte)) {
        break;
      }
      bytes[k] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0 || !x86p_decode(bytes, avail, &insn)) {
      fail(c->name, "the case's own bytes did not decode", NULL);
      return 0;
    }
    which = needs_helper(&insn);
    if (which != kHelperNone) {
      const X86pOperand *dst = &insn.operand[0];
      const X86pOperand *src = &insn.operand[1];
      const int w = (int)dst->size;
      X86pFlags f = cpu->flags;
      if (found >= 0) {
        fail(c->name, "more than one helper-using instruction; see the file header", NULL);
        return 0;
      }
      if (i != 0) {
        fail(c->name, "the helper-using instruction is not the first", NULL);
        return 0;
      }
      found = (int)i;
      out->which = which;
      if (which == kHelperCond) {
        out->arg[0] = insn.cond;
        out->arg[1] = CPU_AT + (uint32_t)offsetof(X86pCpu, flags);
        out->argc = 2;
        out->result = (uint32_t)x86p_cond((X86pCond)insn.cond, &cpu->flags);
      } else if (which == kHelperAluUnary) {
        if (dst->kind != kX86pOperandReg) {
          fail(c->name, "a helper case must not use a memory operand", NULL);
          return 0;
        }
        out->arg[0] = insn.alu;
        out->arg[1] = x86p_reg_read(cpu, dst->reg, w);
        out->arg[2] = (uint32_t)w;
        out->arg[3] = CPU_AT + (uint32_t)offsetof(X86pCpu, flags);
        out->argc = 4;
        out->result = x86p_alu_unary((X86pAluUnOp)insn.alu, out->arg[1], w, &f);
        out->writes_flags = 1;
        out->after = f;
      } else {
        const int shift = insn.alu >= (uint8_t)kX86pAluShl && insn.alu <= (uint8_t)kX86pAluRcr;
        if (dst->kind != kX86pOperandReg || (src->kind != kX86pOperandReg && src->kind != kX86pOperandImm)) {
          fail(c->name, "a helper case must not use a memory operand", NULL);
          return 0;
        }
        out->arg[0] = insn.alu;
        out->arg[1] = x86p_reg_read(cpu, dst->reg, w);
        if (src->kind == kX86pOperandImm) {
          out->arg[2] = shift ? (src->imm & 0xFFu) : (src->imm & x86p_width_mask(w));
        } else {
          out->arg[2] = x86p_reg_read(cpu, src->reg, shift ? 1 : w);
        }
        out->arg[3] = (uint32_t)w;
        out->arg[4] = CPU_AT + (uint32_t)offsetof(X86pCpu, flags);
        out->argc = 5;
        out->result = x86p_alu((X86pAluOp)insn.alu, out->arg[1], out->arg[2], w, &f);
        out->writes_flags = 1;
        out->after = f;
      }
    }
    pc += insn.length;
  }
  return 1;
}

/* The interpreter's answer: the same starting state, stepped for as many
   instructions as the block covered, stopping at the first non-Ok status --
   which is itself part of what the block must reproduce. */
static void run_reference(X86pCpu *cpu, uint32_t insns) {
  X86pMem mem;
  uint32_t i;
  mem.host = g_ref_arena;
  mem.lo = GUEST_LO;
  mem.size = ARENA_SIZE;
  for (i = 0; i < insns; i++) {
    X86pStepReport report;
    if (x86p_step(cpu, &mem, &report) != kX86pStepOk) {
      return;
    }
  }
}

static int write_job(const char *dir,
                     const Case *c,
                     const Recording *helper,
                     const Recording *flag_cf,
                     char *job_path,
                     size_t job_path_len) {
  char path[512];
  FILE *out;
  Recording empty;
  const Recording *alu = &empty;
  const Recording *alu_unary = &empty;
  const Recording *cond = &empty;
  memset(&empty, 0, sizeof empty);
  if (helper->which == kHelperAlu) {
    alu = helper;
  } else if (helper->which == kHelperAluUnary) {
    alu_unary = helper;
  } else if (helper->which == kHelperCond) {
    cond = helper;
  }

  snprintf(job_path, job_path_len, "%s/%s.json", dir, c->name);
  out = fopen(job_path, "wb");
  if (!out) {
    return 0;
  }
  snprintf(path, sizeof path, "%s.wasm", c->name);
  fprintf(out, "{\n  \"wasm\": \"%s\",\n", path);
  fprintf(out, "  \"image\": \"%s.bin\",\n", c->name);
  fprintf(out, "  \"out\": \"%s.out\",\n", c->name);
  fprintf(out, "  \"pages\": %u,\n", (unsigned)IMAGE_PAGES);
  fprintf(out, "  \"imageBytes\": %u,\n", (unsigned)IMAGE_BYTES);
  fprintf(out, "  \"entry\": \"%s\",\n", x86p_wasm_body_name(0));
  fprintf(out, "  \"cpu\": %u,\n", (unsigned)CPU_AT);
  fprintf(out, "  \"helpers\": {\n");
  write_helper(out, "alu", alu, 0);
  write_helper(out, "alu_unary", alu_unary, 0);
  write_helper(out, "cond", cond, 0);
  write_helper(out, "flag_cf", flag_cf, 1);
  fprintf(out, "  }\n}\n");
  return fclose(out) == 0;
}

typedef struct OracleResult {
  int have_exit;
  uint32_t exit;
  char refusal[256];
  unsigned calls[4]; /* indexed by Helper */
  uint32_t arg[MAX_HELPER_ARGS];
  unsigned argc;
} OracleResult;

static Helper helper_by_field(const char *field) {
  if (strcmp(field, "alu") == 0) {
    return kHelperAlu;
  }
  if (strcmp(field, "alu_unary") == 0) {
    return kHelperAluUnary;
  }
  if (strcmp(field, "cond") == 0) {
    return kHelperCond;
  }
  return kHelperNone;
}

static int run_oracle(const char *node, const char *oracle, const char *job, OracleResult *out) {
  char command[1024];
  char line[512];
  FILE *pipe;
  memset(out, 0, sizeof *out);
  snprintf(command, sizeof command, "\"%s\" \"%s\" \"%s\" 2>&1", node, oracle, job);
  pipe = open_pipe(command);
  if (!pipe) {
    snprintf(out->refusal, sizeof out->refusal, "could not run %s", command);
    return 0;
  }
  while (fgets(line, sizeof line, pipe)) {
    char field[32];
    if (line[0] == '!') {
      size_t n = strlen(line);
      while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
      }
      snprintf(out->refusal, sizeof out->refusal, "%s", line);
      continue;
    }
    if (sscanf(line, "exit %u", &out->exit) == 1) {
      out->have_exit = 1;
      continue;
    }
    if (sscanf(line, "call %31s", field) == 1) {
      Helper which = helper_by_field(field);
      const char *rest = strstr(line, field);
      unsigned n = 0;
      if (which == kHelperNone) {
        /* flag_cf, whose arguments are not compared: its only argument is the
           flag address, which every other call already carries. */
        out->calls[kHelperNone]++;
        continue;
      }
      out->calls[which]++;
      rest += strlen(field);
      while (n < MAX_HELPER_ARGS) {
        char *end;
        unsigned long value = strtoul(rest, &end, 10);
        if (end == rest) {
          break;
        }
        out->arg[n++] = (uint32_t)value;
        rest = end;
      }
      out->argc = n;
      continue;
    }
  }
  close_pipe(pipe);
  return out->have_exit || out->refusal[0] != '\0';
}

static void compare_state(const Case *c, const X86pCpu *want, const X86pCpu *got) {
  int i;
  for (i = 0; i < 8; i++) {
    char what[64];
    snprintf(what, sizeof what, "register %d", i);
    check_u32(c->name, what, got->reg[i], want->reg[i]);
  }
  check_u32(c->name, "eip", got->eip, want->eip);
  check_u32(c->name, "flags.a", got->flags.a, want->flags.a);
  check_u32(c->name, "flags.b", got->flags.b, want->flags.b);
  check_u32(c->name, "flags.r", got->flags.r, want->flags.r);
  check_u32(c->name, "flags.kind", got->flags.kind, want->flags.kind);
  check_u32(c->name, "flags.w", got->flags.w, want->flags.w);
  check_u32(c->name, "flags.carry_in", got->flags.carry_in, want->flags.carry_in);
  check_u32(c->name, "df", got->df, want->df);
  /*
   * And then the whole struct, byte for byte. The named fields above are what
   * a report should say; this is what stops a field nobody thought to name --
   * a segment selector, MXCSR, the FPU -- from being written by emitted code
   * without anything noticing.
   */
  g_checks++;
  if (memcmp(want, got, sizeof *want) != 0) {
    size_t offset = 0;
    const uint8_t *a = (const uint8_t *)want;
    const uint8_t *b = (const uint8_t *)got;
    char detail[128];
    while (offset < sizeof *want && a[offset] == b[offset]) {
      offset++;
    }
    snprintf(detail, sizeof detail, "first difference at byte %u of X86pCpu", (unsigned)offset);
    fail(c->name, "the CPU state differs from the interpreter's", detail);
  }
}

static void run_case(const char *node, const char *oracle, const Case *c) {
  X86pCpu cpu;
  X86pCpu reference;
  X86pCpu observed;
  X86pMem mem;
  X86pWasmModule module;
  X86pWasmPlan plan;
  X86pJitBlock block;
  Recording helper;
  Recording flag_cf;
  OracleResult result;
  char reason[256];
  char path[512];
  char job[512];
  size_t length;
  X86pJitStatus status;

  g_cases++;
  seed_cpu(c, &cpu);
  seed_arena(c);

  mem.host = g_arena;
  mem.lo = GUEST_LO;
  mem.size = ARENA_SIZE;
  plan.base = ARENA_AT;
  plan.lo = GUEST_LO;
  plan.size = ARENA_SIZE;

  x86p_wasm_module_init(&module, g_module, sizeof g_module, 1u);
  reason[0] = '\0';
  status = x86p_wasm_lower_block(&module, &mem, &plan, GUEST_LO + CODE_OFF, NULL, NULL, &block, reason, sizeof reason);
  g_checks++;
  if (status != kX86pJitOk) {
    fail(c->name, "lowering refused the block", reason);
    return;
  }
  length = x86p_wasm_module_finish(&module);
  g_checks++;
  if (length == 0) {
    fail(c->name, "the module could not be closed", NULL);
    return;
  }
  g_checks++;
  if (block.insns == 0) {
    fail(c->name, "the block covers no instructions", NULL);
    return;
  }
  check_u32(c->name, "guest bytes covered", block.guest_len, code_length(c));
  g_checks++;
  if (block.flag_helper_calls > 1u) {
    char detail[64];
    snprintf(detail, sizeof detail, "%u calls", block.flag_helper_calls);
    fail(c->name, "more than one carry-in helper call in a block", detail);
    return;
  }

  if (!record_helper(c, &cpu, block.insns, &helper)) {
    return;
  }
  memset(&flag_cf, 0, sizeof flag_cf);
  flag_cf.result = (uint32_t)x86p_flag_cf(&cpu.flags);

  /* The interpreter's answer, from an independent copy of the same picture. */
  reference = cpu;
  memcpy(g_ref_arena, g_arena, sizeof g_ref_arena);
  run_reference(&reference, block.insns);

  /* The engine's picture: the same CPU and the same arena, in linear memory. */
  memset(g_image, 0, sizeof g_image);
  memcpy(g_image + CPU_AT, &cpu, sizeof cpu);
  memcpy(g_image + ARENA_AT, g_arena, sizeof g_arena);

  snprintf(path, sizeof path, "%s/%s.wasm", CASE_DIR, c->name);
  if (!write_file(path, g_module, length)) {
    fail(c->name, "could not write the module", path);
    return;
  }
  snprintf(path, sizeof path, "%s/%s.bin", CASE_DIR, c->name);
  if (!write_file(path, g_image, sizeof g_image)) {
    fail(c->name, "could not write the memory image", path);
    return;
  }
  if (!write_job(CASE_DIR, c, &helper, &flag_cf, job, sizeof job)) {
    fail(c->name, "could not write the oracle job", NULL);
    return;
  }

  if (!run_oracle(node, oracle, job, &result)) {
    fail(c->name, "the oracle produced nothing", result.refusal[0] ? result.refusal : NULL);
    return;
  }
  if (result.refusal[0] != '\0') {
    fail(c->name, "the engine refused the module or the call", result.refusal);
    return;
  }

  check_u32(c->name, "exit", result.exit, (uint32_t)c->expect_exit);
  check_u32(c->name, "carry-in helper calls", result.calls[kHelperNone], block.flag_helper_calls);
  check_u32(c->name, "alu helper calls", result.calls[kHelperAlu], helper.which == kHelperAlu ? 1u : 0u);
  check_u32(
      c->name, "alu_unary helper calls", result.calls[kHelperAluUnary], helper.which == kHelperAluUnary ? 1u : 0u);
  check_u32(c->name, "cond helper calls", result.calls[kHelperCond], helper.which == kHelperCond ? 1u : 0u);
  if (helper.which != kHelperNone) {
    unsigned i;
    check_u32(c->name, "helper argument count", result.argc, helper.argc);
    for (i = 0; i < helper.argc && i < result.argc; i++) {
      char what[64];
      snprintf(what, sizeof what, "%s argument %u", helper_field(helper.which), i);
      check_u32(c->name, what, result.arg[i], helper.arg[i]);
    }
  }

  snprintf(path, sizeof path, "%s/%s.out", CASE_DIR, c->name);
  if (!read_file(path, g_result, sizeof g_result)) {
    fail(c->name, "the oracle wrote no result image", path);
    return;
  }
  memcpy(&observed, g_result + CPU_AT, sizeof observed);
  compare_state(c, &reference, &observed);
  g_checks++;
  if (memcmp(g_ref_arena, g_result + ARENA_AT, sizeof g_ref_arena) != 0) {
    fail(c->name, "guest memory differs from the interpreter's", NULL);
  }
  g_ran++;
}

int main(void) {
  const char *node = getenv("X86P_NODE");
  const char *oracle = getenv("X86P_WASM_JIT_ORACLE");
  int i;

  if (!node || !*node) {
    node = "node";
  }
  if (!oracle || !*oracle) {
    printf("test_jit_wasm: X86P_WASM_JIT_ORACLE is unset, so no lowered block could be run in an engine. "
           "Nothing about the lowering has been established.\n");
    return 77;
  }
  if (!engine_present(node)) {
    printf("test_jit_wasm: no WebAssembly engine found (tried `%s`). Set X86P_NODE to one. Nothing about the "
           "lowering has been established.\n",
           node);
    return 77;
  }
  if (x86p_mkdir(CASE_DIR) != 0) {
    /* Already there from a previous run is fine; anything else surfaces as a
       failed write below, with the path in the message. */
  }

  for (i = 0; i < CASE_COUNT; i++) {
    run_case(node, oracle, &kCases[i]);
  }

  if (g_ran == 0) {
    printf("test_jit_wasm: an engine was present but NOT ONE of the %d blocks reached it, so this run has "
           "established nothing. Refusing to report success.\n",
           CASE_COUNT);
    return 1;
  }
  printf("test_jit_wasm: %d checks, %d failed; %d of %d lowered blocks ran in a real WebAssembly engine and "
         "matched the interpreter field for field\n",
         g_checks,
         g_failed,
         g_ran,
         g_cases);
  return g_failed == 0 ? 0 : 1;
}
